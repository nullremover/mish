#include "discordrpc.h"

// Cross-platform Discord Rich Presence client.
//   Windows: named pipe (\\.\pipe\discord-ipc-N) + WinHTTP   (link: -lwinhttp)
//   Linux:   unix socket ($XDG_RUNTIME_DIR/discord-ipc-N)    + libcurl (link: -lcurl -pthread)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <winhttp.h>
#else
  #include <curl/curl.h>
  #include <poll.h>
  #include <sys/ioctl.h>
  #include <sys/socket.h>
  #include <sys/un.h>
  #include <unistd.h>
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
#endif

#include <SFML/Graphics/Image.hpp>
#include "pathutil.h"
#include <taglib/fileref.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>

#if defined(_WIN32) && defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib") // MinGW: add -lwinhttp to the link line
#endif

namespace DiscordRPC {
namespace {

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
// 2 = "Listening to mish" (Spotify-style progress bar: 00:22 ---- 05:10)
// 0 = "Playing mish"      (shows "xx:xx left" text instead of a bar)
// (the "mish" part is the application name from the Discord Developer Portal)
constexpr int kActivityType = 2;

// Rich Presence art-asset key you uploaded in the Developer Portal
constexpr const char* kLogoKey  = "mish";
constexpr const char* kLogoText = "mish";

// Discord rate-limits presence updates; rapid track skipping is coalesced
// into one update every few seconds.
constexpr std::chrono::seconds kMinSendInterval(4);
constexpr std::chrono::seconds kReconnectInterval(5);

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::int64_t nowEpoch()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

unsigned long processId()
{
#ifdef _WIN32
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Debug log written to the working directory (mish_discord.log). Handy because the app has no console.
constexpr const char* kLogFile = "mish_discord.log";
std::mutex g_logMutex;

void logLine(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    std::ofstream f(kLogFile, std::ios::app);
    if (!f)
        return;
    const std::time_t t = std::time(nullptr);
    char stamp[32] = "";
    if (const std::tm* tm = std::localtime(&t))
        std::strftime(stamp, sizeof stamp, "%H:%M:%S", tm);
    f << stamp << "  " << msg << "\n";
}

std::string clip(const std::string& s, size_t n = 300)
{
    return s.size() <= n ? s : s.substr(0, n) + "...";
}

std::string jsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

// Discord limits text fields to 128 bytes and rejects 1-character strings.
std::string fixField(std::string s)
{
    if (s.size() > 128) {
        size_t cut = 128;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
            --cut; // don't cut a UTF-8 character in half
        s.resize(cut);
    }
    if (s.size() == 1)
        s += "\xE2\x80\x8B"; // zero-width space
    return s;
}

std::string urlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// lower-case ASCII letters/digits only, for loose title comparisons
std::string normalize(const std::string& s)
{
    std::string o;
    for (unsigned char c : s)
        if (c < 128 && std::isalnum(c))
            o += static_cast<char>(std::tolower(c));
    return o;
}

bool similar(const std::string& a, const std::string& b)
{
    const std::string na = normalize(a), nb = normalize(b);
    if (na.empty() || nb.empty())
        return false;
    return na.find(nb) != std::string::npos || nb.find(na) != std::string::npos;
}

// ---------------------------------------------------------------------------
// HTTPS client: one function, two backends.
// Returns the response body ("" on network failure). HTTP error pages are returned
// as-is, so callers can tell "host refused us" from "no network".
// ---------------------------------------------------------------------------
#ifdef _WIN32

struct WinHttpHandle {
    HINTERNET h;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
};

std::string httpsRequest(const std::string& host, const std::string& asciiPath, const char* method,
                         const std::string& header = "", const std::string& postBody = {})
{
    // host, path, method and header are plain ASCII, so a byte-wise widen is fine
    const std::wstring whost(host.begin(), host.end());
    const std::wstring wpath(asciiPath.begin(), asciiPath.end());
    const std::wstring wmethod(method, method + std::strlen(method));
    const std::wstring wheader(header.begin(), header.end());

    WinHttpHandle session(WinHttpOpen(L"mish/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return {};
    WinHttpSetTimeouts(session.h, 4000, 5000, 15000, 15000);

    WinHttpHandle conn(WinHttpConnect(session.h, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!conn.h) return {};

    WinHttpHandle req(WinHttpOpenRequest(conn.h, wmethod.c_str(), wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req.h) return {};

    LPCWSTR hdr    = wheader.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wheader.c_str();
    DWORD   hdrLen = wheader.empty() ? 0 : static_cast<DWORD>(-1L); // -1 = null-terminated
    LPVOID  data   = postBody.empty() ? WINHTTP_NO_REQUEST_DATA
                                      : const_cast<char*>(postBody.data());
    const DWORD dataLen = static_cast<DWORD>(postBody.size());

    if (!WinHttpSendRequest(req.h, hdr, hdrLen, data, dataLen, dataLen, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return {};

    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail) || avail == 0) break;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req.h, &chunk[0], avail, &read)) break;
        body.append(chunk, 0, read);
        if (body.size() > (1u << 20)) break;
    }
    return body;
}

#else // Linux / other unix: libcurl

size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > (1u << 20))
        return 0; // absurdly large response: abort the transfer
    out->append(ptr, n);
    return n;
}

std::string httpsRequest(const std::string& host, const std::string& asciiPath, const char* method,
                         const std::string& header = "", const std::string& postBody = {})
{
    static std::once_flag curlInit;
    std::call_once(curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl)
        return {};

    std::string response;
    const std::string url = "https://" + host + asciiPath;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mish/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // we're not on the main thread

    curl_slist* headers = nullptr;
    if (!header.empty()) {
        headers = curl_slist_append(headers, header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    if (std::strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(postBody.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.data()); // not copied: postBody outlives the call
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        logLine(std::string("HTTP request failed: ") + curl_easy_strerror(rc));
        response.clear();
    }

    if (headers)
        curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

#endif

std::string httpsGet(const std::string& host, const std::string& asciiPath)
{
    return httpsRequest(host, asciiPath, "GET");
}

// ---------------------------------------------------------------------------
// Album art lookup (iTunes Search API, no key needed).
// Discord can't show a local file, it needs a public https URL.
// ---------------------------------------------------------------------------

// Crude "key":"value" extractor (enough for the iTunes response).
std::string jsonStringAfter(const std::string& json, const std::string& key)
{
    size_t p = json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p += key.size() + 2;
    while (p < json.size() && (json[p] == ' ' || json[p] == ':' || json[p] == '\t' ||
                               json[p] == '\n' || json[p] == '\r'))
        ++p;
    if (p >= json.size() || json[p] != '"') return {};
    ++p;

    std::string out;
    for (; p < json.size() && json[p] != '"'; ++p) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            ++p;
            out += json[p];
        } else {
            out += json[p];
        }
    }
    return out;
}

// nullopt  = network failure (don't cache, try again later)
// ""       = no (trustworthy) match found
// otherwise the artwork URL
std::optional<std::string> lookupArtwork(const std::string& artist, const std::string& album)
{
    const bool haveAlbum  = !album.empty()  && album  != "Unknown Album";
    const bool haveArtist = !artist.empty() && artist != "Unknown Artist";
    if (!haveAlbum)
        return std::string();

    const std::string term = haveArtist ? artist + " " + album : album;
    const std::string body = httpsGet(
        "itunes.apple.com", "/search?media=music&entity=album&limit=1&term=" + urlEncode(term));
    if (body.empty())
        return std::nullopt;

    std::string art = jsonStringAfter(body, "artworkUrl100");
    if (art.empty())
        return std::string();

    // Don't show the wrong cover: the first hit must really look like our album/artist
    if (!similar(album, jsonStringAfter(body, "collectionName")))
        return std::string();
    if (haveArtist && !similar(artist, jsonStringAfter(body, "artistName")))
        return std::string();

    const size_t pos = art.rfind("100x100");
    if (pos != std::string::npos)
        art.replace(pos, 7, "600x600");
    return art;
}

// ---------------------------------------------------------------------------
// Embedded album art -> small jpeg -> temporary public URL
// ---------------------------------------------------------------------------

// Raw image bytes of the cover embedded in an mp3 (APIC) or flac file, or "".
std::string readEmbeddedArt(const std::string& path)
{
    // path is UTF-8; hand TagLib the real (wide on Windows) path so any character works
    const std::filesystem::path fsPath = pathutil::fromUtf8(path);
    TagLib::FileRef f(fsPath.c_str());
    if (f.isNull() || !f.file())
        return {};

    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(f.file())) {
        TagLib::ID3v2::Tag* id3 = mpeg->ID3v2Tag();
        if (!id3)
            return {};

        TagLib::ID3v2::AttachedPictureFrame* best = nullptr;
        for (auto* frame : id3->frameList(TagLib::ByteVector("APIC"))) {
            auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame);
            if (!pic)
                continue;
            if (!best)
                best = pic;
            if (pic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover) {
                best = pic;
                break;
            }
        }
        if (!best)
            return {};
        const TagLib::ByteVector data = best->picture();
        return std::string(data.data(), data.size());
    }

    if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(f.file())) {
        const auto& pictures = flac->pictureList();
        if (pictures.isEmpty())
            return {};

        TagLib::FLAC::Picture* best = pictures.front();
        for (auto* pic : pictures) {
            if (pic->type() == TagLib::FLAC::Picture::FrontCover) {
                best = pic;
                break;
            }
        }
        const TagLib::ByteVector data = best->data();
        return std::string(data.data(), data.size());
    }

    return {};
}

// Decode, shrink to at most maxSide pixels (box filter) and re-encode as jpeg.
// Embedded covers can be several MB, which is too much to upload and to show.
std::optional<std::vector<std::uint8_t>> toSmallJpeg(const std::string& bytes, unsigned maxSide = 512)
{
    sf::Image src;
    if (!src.loadFromMemory(bytes.data(), bytes.size()))
        return std::nullopt;

    const sf::Vector2u size = src.getSize();
    if (size.x == 0 || size.y == 0)
        return std::nullopt;

    const unsigned longSide = std::max(size.x, size.y);
    if (longSide <= maxSide)
        return src.saveToMemory("jpg");

    const double   scale = static_cast<double>(maxSide) / longSide;
    const unsigned nw = std::max(1u, static_cast<unsigned>(size.x * scale));
    const unsigned nh = std::max(1u, static_cast<unsigned>(size.y * scale));

    sf::Image dst(sf::Vector2u(nw, nh), sf::Color::Black);
    for (unsigned y = 0; y < nh; ++y) {
        const unsigned y0 = static_cast<unsigned>(static_cast<std::uint64_t>(y) * size.y / nh);
        const unsigned y1 = std::max(y0 + 1, static_cast<unsigned>(static_cast<std::uint64_t>(y + 1) * size.y / nh));
        for (unsigned x = 0; x < nw; ++x) {
            const unsigned x0 = static_cast<unsigned>(static_cast<std::uint64_t>(x) * size.x / nw);
            const unsigned x1 = std::max(x0 + 1, static_cast<unsigned>(static_cast<std::uint64_t>(x + 1) * size.x / nw));

            unsigned r = 0, g = 0, b = 0, n = 0;
            for (unsigned sy = y0; sy < y1 && sy < size.y; ++sy) {
                for (unsigned sx = x0; sx < x1 && sx < size.x; ++sx) {
                    const sf::Color c = src.getPixel(sf::Vector2u(sx, sy));
                    r += c.r;
                    g += c.g;
                    b += c.b;
                    ++n;
                }
            }
            if (n > 0)
                dst.setPixel(sf::Vector2u(x, y),
                             sf::Color(static_cast<std::uint8_t>(r / n),
                                       static_cast<std::uint8_t>(g / n),
                                       static_cast<std::uint8_t>(b / n)));
        }
    }
    return dst.saveToMemory("jpg");
}

// POST a jpeg as multipart/form-data and return the raw response body.
std::string postMultipart(const std::string& host, const std::string& path,
                          const std::vector<std::pair<std::string, std::string>>& fields,
                          const std::string& fileField, const std::vector<std::uint8_t>& jpg)
{
    const std::string boundary = "----mishCoverBoundary7MA4YWxkTrZu0gW";

    std::string body;
    for (const auto& kv : fields)
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + kv.first +
                "\"\r\n\r\n" + kv.second + "\r\n";
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + fileField +
            "\"; filename=\"cover.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
    body.append(reinterpret_cast<const char*>(jpg.data()), jpg.size());
    body += "\r\n--" + boundary + "--\r\n";

    const std::string header = "Content-Type: multipart/form-data; boundary=" + boundary;
    return httpsRequest(host, path, "POST", header, body);
}

std::string trimmed(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    return s;
}

// Hosts that refuse us (error page, block page) are skipped for a while, so we don't
// wait on a dead host for every album. Worker thread only.
std::map<std::string, std::chrono::steady_clock::time_point> g_hostBlockedUntil;

bool hostAvailable(const std::string& name)
{
    const auto it = g_hostBlockedUntil.find(name);
    return it == g_hostBlockedUntil.end() || std::chrono::steady_clock::now() >= it->second;
}

// An empty response means no network at all, which says nothing about the host.
void noteHostRefused(const std::string& name, const std::string& response)
{
    if (response.empty())
        return;
    g_hostBlockedUntil[name] = std::chrono::steady_clock::now() + std::chrono::minutes(30);
    logLine("Skipping " + name + " for 30 minutes");
}

// Upload the cover to a free temporary image host and return its public URL.
// Tries several hosts in order, so one being down or refusing us doesn't lose the art.
std::optional<std::string> uploadCover(const std::vector<std::uint8_t>& jpg)
{
    if (!hostAvailable("litterbox") && !hostAvailable("uguu") && !hostAvailable("tmpfiles"))
        g_hostBlockedUntil.clear(); // everything was skipped: give them all another chance

    // 1. Litterbox (catbox.moe's temporary host): files live 72h
    if (hostAvailable("litterbox")) {
        const std::string r = trimmed(postMultipart("litterbox.catbox.moe",
                                                    "/resources/internals/api.php",
                                                    {{"reqtype", "fileupload"}, {"time", "72h"}},
                                                    "fileToUpload", jpg));
        logLine("Litterbox response: " + clip(r));
        if (r.rfind("https://", 0) == 0)
            return r;
        noteHostRefused("litterbox", r);
    }

    // 2. Uguu: files live a few hours, plain-text URL response
    if (hostAvailable("uguu")) {
        const std::string r = trimmed(postMultipart("uguu.se", "/upload?output=text", {}, "files[]", jpg));
        logLine("Uguu response: " + clip(r));
        if (r.rfind("https://", 0) == 0)
            return r;
        noteHostRefused("uguu", r);
    }

    // 3. tmpfiles.org: files live about an hour, JSON response with a page link
    //    that has to be turned into the direct-download link (/dl/)
    if (hostAvailable("tmpfiles")) {
        const std::string r = postMultipart("tmpfiles.org", "/api/v1/upload", {}, "file", jpg);
        logLine("tmpfiles response: " + clip(r));
        const std::string url = jsonStringAfter(r, "url");
        const std::string marker = "tmpfiles.org/";
        const size_t pos = url.find(marker);
        if (pos != std::string::npos) {
            std::string rest = url.substr(pos + marker.size());
            if (rest.rfind("dl/", 0) == 0)
                rest.erase(0, 3);
            return "https://tmpfiles.org/dl/" + rest;
        }
        noteHostRefused("tmpfiles", r);
    }

    return std::nullopt;
}

// Find a public image URL for this track's cover. The first track played from an
// album decides the cover for the whole album (the result is cached per album).
//   1. cover embedded in the file  -> shrink -> upload
//   2. fallback: iTunes lookup by artist + album
// Returns "" when there's nothing (the caller shows the mish logo instead).
std::string resolveArtwork(const Presence& p, std::map<std::string, std::string>& cache,
                           bool& temporaryFailureOut)
{
    temporaryFailureOut = false;
    const bool albumKnown = !p.album.empty() && p.album != "Unknown Album";
    const std::string albumKey = "album|" + p.artist + "|" + p.album;
    const std::string fileKey = "file|" + p.filePath;

    if (albumKnown) {
        auto it = cache.find(albumKey);
        if (it != cache.end())
            return it->second;
    }
    {
        auto it = cache.find(fileKey);
        if (it != cache.end())
            return it->second;
    }

    bool temporaryFailure = false;
    std::string url;

    logLine("Resolving cover for: " + p.artist + " - " + p.album);
    const std::string raw = readEmbeddedArt(p.filePath);
    logLine("Embedded art bytes: " + std::to_string(raw.size()));
    if (!raw.empty()) {
        if (auto jpg = toSmallJpeg(raw)) {
            logLine("Shrunk jpeg bytes: " + std::to_string(jpg->size()) + ", uploading...");
            const std::optional<std::string> uploaded = uploadCover(*jpg);
            if (uploaded.has_value())
                url = *uploaded;
            else
                temporaryFailure = true;
        } else {
            logLine("Could not decode the embedded image");
        }
    }

    if (url.empty()) {
        logLine("Trying iTunes lookup...");
        const std::optional<std::string> found = lookupArtwork(p.artist, p.album);
        if (found.has_value())
            url = *found;
        else
            temporaryFailure = true;
        logLine(found.has_value() ? "iTunes result: " + (found->empty() ? std::string("no match") : *found)
                                  : std::string("iTunes lookup failed (network?)"));
    }

    if (!url.empty())
        cache[albumKnown ? albumKey : fileKey] = url;
    else if (!temporaryFailure)
        cache[fileKey] = ""; // this file has no usable art; other tracks of the album may

    temporaryFailureOut = temporaryFailure;
    logLine("Cover URL: " + (url.empty() ? std::string("(none, using mish logo)") : url));
    return url;
}

// ---------------------------------------------------------------------------
// Discord IPC
//   Windows: named pipe  \\.\pipe\discord-ipc-N
//   Linux:   unix socket <runtime dir>/discord-ipc-N  (also Snap / Flatpak locations)
// Frame = uint32 opcode, uint32 length, JSON payload (little endian)
// Opcodes: 0 handshake, 1 frame, 2 close, 3 ping, 4 pong
// ---------------------------------------------------------------------------
class Pipe {
public:
    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() { close(); }

    bool connected() const { return isOpen(); }

    bool connect(const std::string& clientId)
    {
        for (const std::string& path : candidatePaths()) {
            if (!openPath(path))
                continue;

            std::uint32_t op = 0;
            std::string payload;
            if (write(0, "{\"v\":1,\"client_id\":\"" + clientId + "\"}") &&
                waitForFrame(3000) && readFrame(op, payload) && op == 1 &&
                payload.find("READY") != std::string::npos)
                return true;

            close();
        }
        return false;
    }

    void close()
    {
        if (isOpen())
            closeRaw();
    }

    bool write(std::uint32_t op, const std::string& json)
    {
        std::string buf(8 + json.size(), '\0');
        const std::uint32_t len = static_cast<std::uint32_t>(json.size());
        std::memcpy(&buf[0], &op, 4);
        std::memcpy(&buf[4], &len, 4);
        std::memcpy(&buf[8], json.data(), json.size());
        return writeRaw(buf.data(), buf.size());
    }

    // Drain incoming frames (answer pings, notice disconnects).
    // Returns false if the connection is gone.
    bool pump()
    {
        for (;;) {
            std::size_t avail = 0;
            if (!bytesAvailable(avail))
                return false;
            if (avail < 8)
                return true;

            std::uint32_t op = 0;
            std::string payload;
            if (!readFrame(op, payload))
                return false;
            if (op == 2) {
                logLine("Discord closed the connection: " + clip(payload));
                return false;      // CLOSE
            }
            if (op == 3)
                write(4, payload); // PING -> PONG
            if (op == 1)
                logLine("Discord replied: " + clip(payload));
        }
    }

private:
    // ---- platform specific part ------------------------------------------
#ifdef _WIN32
    static std::vector<std::string> candidatePaths()
    {
        std::vector<std::string> paths;
        for (int i = 0; i < 10; ++i)
            paths.push_back("\\\\.\\pipe\\discord-ipc-" + std::to_string(i));
        return paths;
    }

    bool isOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    bool openPath(const std::string& path)
    {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;
        h_ = h;
        return true;
    }

    void closeRaw()
    {
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

    bool writeRaw(const char* data, std::size_t len)
    {
        DWORD written = 0;
        return WriteFile(h_, data, static_cast<DWORD>(len), &written, nullptr) && written == len;
    }

    // >0 bytes read, 0 / <0 = connection problem
    std::int64_t readRaw(char* dst, std::size_t len)
    {
        DWORD n = 0;
        const BOOL ok = ReadFile(h_, dst, static_cast<DWORD>(len), &n, nullptr);
        if (!ok && GetLastError() != ERROR_MORE_DATA)
            return -1;
        return static_cast<std::int64_t>(n);
    }

    // false = the connection is broken
    bool bytesAvailable(std::size_t& avail)
    {
        DWORD a = 0;
        if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &a, nullptr))
            return false;
        avail = a;
        return true;
    }

    HANDLE h_ = INVALID_HANDLE_VALUE;

#else // unix domain socket
    static std::vector<std::string> candidatePaths()
    {
        // Where Discord puts its socket: the runtime dir, plus the Snap and Flatpak sandboxes
        std::vector<std::string> bases;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
            const char* v = std::getenv(var);
            if (v && *v)
                bases.push_back(v);
        }
        bases.push_back("/tmp");

        std::vector<std::string> paths;
        for (const std::string& base : bases) {
            for (const char* sub : {"", "/snap.discord", "/app/com.discordapp.Discord",
                                    "/app/com.discordapp.DiscordCanary"}) {
                for (int i = 0; i < 10; ++i)
                    paths.push_back(base + sub + "/discord-ipc-" + std::to_string(i));
            }
        }
        return paths;
    }

    bool isOpen() const { return fd_ >= 0; }

    bool openPath(const std::string& path)
    {
        sockaddr_un addr;
        std::memset(&addr, 0, sizeof addr);
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof addr.sun_path)
            return false;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
            return false;
#ifdef SO_NOSIGPIPE
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(fd);
            return false;
        }
        fd_ = fd;
        return true;
    }

    void closeRaw()
    {
        ::close(fd_);
        fd_ = -1;
    }

    bool writeRaw(const char* data, std::size_t len)
    {
        std::size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd_, data + sent, len - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::int64_t readRaw(char* dst, std::size_t len)
    {
        for (;;) {
            const ssize_t n = ::read(fd_, dst, len);
            if (n < 0 && errno == EINTR)
                continue;
            return static_cast<std::int64_t>(n);
        }
    }

    bool bytesAvailable(std::size_t& avail)
    {
        avail = 0;
        pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int rc = ::poll(&pfd, 1, 0);
        if (rc < 0)
            return errno == EINTR;
        if (rc == 0)
            return true; // nothing to read right now
        if (pfd.revents & (POLLERR | POLLNVAL))
            return false;

        int n = 0;
        if (::ioctl(fd_, FIONREAD, &n) != 0)
            return false;
        avail = static_cast<std::size_t>(n);

        // "readable" but zero bytes pending = the other side closed the connection
        if (n == 0 && (pfd.revents & (POLLIN | POLLHUP)))
            return false;
        return true;
    }

    int fd_ = -1;
#endif

    // ---- shared part -------------------------------------------------------
    bool waitForFrame(int timeoutMs)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            std::size_t avail = 0;
            if (!bytesAvailable(avail))
                return false;
            if (avail >= 8)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    bool readExact(void* dst, std::size_t len)
    {
        char*       p = static_cast<char*>(dst);
        std::size_t total = 0;
        while (total < len) {
            const std::int64_t n = readRaw(p + total, len - total);
            if (n <= 0)
                return false;
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    bool readFrame(std::uint32_t& op, std::string& payload)
    {
        std::uint32_t header[2] = {0, 0};
        if (!readExact(header, 8))
            return false;
        op = header[0];
        const std::uint32_t len = header[1];
        if (len > (1u << 20))
            return false;
        payload.assign(len, '\0');
        return len == 0 || readExact(&payload[0], len);
    }
};

// ---------------------------------------------------------------------------
// Shared state between the UI thread and the worker thread
// ---------------------------------------------------------------------------
std::mutex              g_mutex;
std::condition_variable g_cv;
std::thread             g_thread;
std::string             g_clientId;
bool                    g_running = false;
bool                    g_stop = false;
bool                    g_dirty = false;
Presence                g_pending;          // starts out Stopped
std::int64_t            g_startEpoch = 0;   // wall-clock time the current track "started"

std::string buildPayload(const Presence& p, std::int64_t startEpoch, const std::string& artUrl)
{
    static std::atomic<unsigned> nonce{0};

    std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" + std::to_string(++nonce) +
                       "\",\"args\":{\"pid\":" + std::to_string(processId());

    if (p.state != PlayState::Stopped) { // no "activity" key = clear presence
        json += ",\"activity\":{\"type\":" + std::to_string(kActivityType);

        // Line 1: song
        json += ",\"details\":\"" + jsonEscape(fixField(p.title)) + "\"";

        // Line 2: artist / album
        std::string line2;
        if (!p.artist.empty() && p.artist != "Unknown Artist")
            line2 = p.artist;
        if (!p.album.empty() && p.album != "Unknown Album")
            line2 += (line2.empty() ? "" : " / ") + p.album;
        if (!line2.empty())
            json += ",\"state\":\"" + jsonEscape(fixField(line2)) + "\"";

        // Timer (only while playing). With both start and end, a Listening activity gets a
        // Spotify-style progress bar (elapsed / total). Pausing removes it.
        if (p.state == PlayState::Playing) {
            json += ",\"timestamps\":{\"start\":" + std::to_string(startEpoch);
            if (p.durationSeconds > 0.0)
                json += ",\"end\":" + std::to_string(startEpoch + static_cast<std::int64_t>(p.durationSeconds));
            json += "}";
        }

        // Big image = album art (public URL) or the mish logo as a fallback.
        // Small image = mish logo.
        json += ",\"assets\":{";
        if (!artUrl.empty()) {
            json += "\"large_image\":\"" + jsonEscape(artUrl) + "\"";
            json += ",\"large_text\":\"" + jsonEscape(fixField(p.album.empty() ? p.title : p.album)) + "\"";
            json += ",\"small_image\":\"" + std::string(kLogoKey) + "\"";
            json += ",\"small_text\":\"" +
                    std::string(p.state == PlayState::Paused ? "Paused" : kLogoText) + "\"";
        } else {
            json += "\"large_image\":\"" + std::string(kLogoKey) + "\"";
            json += ",\"large_text\":\"" +
                    std::string(p.state == PlayState::Paused ? "Paused" : kLogoText) + "\"";
        }
        json += "}}";
    }

    json += "}}";
    return json;
}

void workerMain()
{
    using clock = std::chrono::steady_clock;

    Pipe pipe;
    std::map<std::string, std::string> artCache; // album / file key -> url ("" = none)
    clock::time_point lastConnectTry = clock::now() - kReconnectInterval;
    clock::time_point lastSend = clock::now() - kMinSendInterval;
    clock::time_point retryAt = clock::now();
    bool        retryPending = false;
    int         retries = 0;
    std::string retryPath;
    bool        loggedConnectFail = false;

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return g_stop; });
            if (g_stop)
                break;
        }

        const auto now = clock::now();

        if (!pipe.connected()) {
            if (now - lastConnectTry >= kReconnectInterval) {
                lastConnectTry = now;
                if (pipe.connect(g_clientId)) {
                    logLine("Connected to Discord");
                    loggedConnectFail = false;
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_dirty = true; // (re)send whatever is current
                } else if (!loggedConnectFail) {
                    logLine("Could not connect to Discord (is the desktop app running? is the Application ID right?). Will keep retrying.");
                    loggedConnectFail = true;
                }
            }
        } else if (!pipe.pump()) {
            pipe.close();
        }
        if (!pipe.connected())
            continue;

        // A cover lookup failed earlier: resend the presence so it gets another go
        if (retryPending && now >= retryAt) {
            retryPending = false;
            std::lock_guard<std::mutex> lk(g_mutex);
            g_dirty = true;
        }

        Presence     p;
        std::int64_t start = 0;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            if (!g_dirty || now - lastSend < kMinSendInterval)
                continue;
            p = g_pending;
            start = g_startEpoch;
            g_dirty = false;
        }

        std::string art;
        if (p.state != PlayState::Stopped) {
            bool tempFail = false;
            art = resolveArtwork(p, artCache, tempFail);

            if (p.filePath != retryPath) {
                retryPath = p.filePath;
                retries = 0;
            }
            if (tempFail && art.empty() && retries < 5) {
                ++retries;
                retryPending = true;
                retryAt = clock::now() + std::chrono::seconds(30);
                logLine("Cover lookup failed, will retry in 30s (attempt " + std::to_string(retries) + "/5)");
            }
        }

        const std::string payload = buildPayload(p, start, art);
        logLine("Sending presence: " + clip(payload, 600));
        if (pipe.write(1, payload)) {
            lastSend = clock::now();
        } else {
            pipe.close();
            std::lock_guard<std::mutex> lk(g_mutex);
            g_dirty = true; // retry after reconnecting
        }
    }

    // Leaving: clear our presence
    if (pipe.connected()) {
        pipe.write(1, buildPayload(Presence{}, 0, ""));
        pipe.close();
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void init(const std::string& applicationId)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_running)
        return;
    g_clientId = applicationId;
    g_stop = false;
    g_dirty = false;
    g_pending = Presence{};
    g_running = true;
    { std::ofstream(kLogFile, std::ios::trunc); } // start a fresh log each run
    logLine("Discord RPC started");
    if (applicationId.empty() || applicationId.find("YOUR_") != std::string::npos)
        logLine("WARNING: no Discord Application ID set (edit discord_config.h). Rich Presence cannot connect without it.");
    g_thread = std::thread(workerMain);
}

void setPresence(const Presence& p)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_running)
        return;

    const std::int64_t start = nowEpoch() - static_cast<std::int64_t>(p.positionSeconds);

    bool changed;
    if (p.state == PlayState::Stopped) {
        changed = (g_pending.state != PlayState::Stopped);
    } else {
        changed = p.state != g_pending.state || p.title != g_pending.title ||
                  p.artist != g_pending.artist || p.album != g_pending.album ||
                  p.filePath != g_pending.filePath;
        // A seek (or a resume) moves the implied start time
        if (!changed && p.state == PlayState::Playing && std::llabs(start - g_startEpoch) > 2)
            changed = true;
    }

    if (!changed)
        return;

    g_pending = p;
    g_startEpoch = start;
    g_dirty = true;
}

void shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_running)
            return;
        g_stop = true;
    }
    g_cv.notify_all();
    if (g_thread.joinable())
        g_thread.join();
    std::lock_guard<std::mutex> lk(g_mutex);
    g_running = false;
}

} // namespace DiscordRPC