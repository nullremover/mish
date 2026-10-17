#include "nowplaying_popup.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Look & timing
// ---------------------------------------------------------------------------
constexpr unsigned kWidth  = 360;
constexpr unsigned kHeight = 96;
constexpr int      kMargin = 16;   // distance from the screen edges / taskbar

constexpr float kSlideMs = 250.f;  // slide + fade in
constexpr float kHoldMs  = 5000.f; // fully visible
constexpr float kFadeMs  = 500.f;  // fade out
constexpr float kSlideDistance = 40.f;

const sf::Color kBackground(22, 24, 30);
const sf::Color kBorder(60, 64, 74);
const sf::Color kAccent(0, 220, 255);   // same cyan as the track title in the main UI
const sf::Color kTextMain(240, 240, 240);
const sf::Color kTextDim(150, 150, 160);

constexpr float kArtSize = 80.f;
constexpr float kArtX = 14.f;
constexpr float kArtY = 8.f;
constexpr float kTextX = kArtX + kArtSize + 12.f;
constexpr float kTextMaxWidth = kWidth - kTextX - 12.f;

// Debug log (next to where you run mish): says why the pop-up didn't appear
constexpr const char* kLogFile = "mish_popup.log";

void logPopup(const std::string& msg)
{
    std::ofstream f(kLogFile, std::ios::app);
    if (f)
        f << msg << "\n";
}

// Zero-width / variation-selector characters have no glyph of their own: drop them
// instead of letting them show up as tofu boxes.
bool isIgnorable(char32_t c)
{
    return (c >= 0x200B && c <= 0x200F) || c == 0x2060 || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F);
}

// Index of the font that draws this character: plain ASCII always uses the main font (index 0),
// anything else uses the first font in the chain that has a glyph for it.
std::size_t pickFont(const std::vector<std::unique_ptr<sf::Font>>& chain, char32_t c)
{
    if (c < 0x80)
        return 0;
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (chain[i]->hasGlyph(c))
            return i;
    return 0; // nobody has it: tofu from the main font is the best we can do
}

} // namespace

NowPlayingPopup::~NowPlayingPopup()
{
    if (m_hwnd)
        DestroyWindow(static_cast<HWND>(m_hwnd));
    if (m_memDC)
        DeleteDC(static_cast<HDC>(m_memDC));
    if (m_dib)
        DeleteObject(static_cast<HGDIOBJ>(m_dib));
}

bool NowPlayingPopup::init()
{
    { std::ofstream(kLogFile, std::ios::trunc); } // fresh log each run

    // --- fonts (Segoe UI ships with Windows) ---
    char winDir[MAX_PATH] = "";
    const UINT n = GetWindowsDirectoryA(winDir, MAX_PATH);
    const std::string fonts = (n > 0 ? std::string(winDir) : std::string("C:\\Windows")) + "\\Fonts\\";

    // Each slot is a list of candidate files; the first one that loads wins. Slot 0 is the main
    // font, the others are fallbacks for characters it doesn't have.
    auto loadChain = [&](FontChain& chain, const std::vector<std::vector<std::string>>& slots) {
        for (const auto& candidates : slots) {
            for (const std::string& name : candidates) {
                auto font = std::make_unique<sf::Font>();
                if (font->openFromFile(fonts + name)) {
                    chain.push_back(std::move(font));
                    break;
                }
            }
        }
    };

    loadChain(m_regularFonts, {
        {"segoeui.ttf", "arial.ttf"},                       // main font
        {"seguisym.ttf"},                                   // symbols: stars, notes, arrows, shapes...
        {"seguiemj.ttf"},                                   // emoji (monochrome outlines)
        {"YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"},      // Japanese
        {"malgun.ttf"},                                     // Korean
        {"msyh.ttc", "simsun.ttc"},                         // Chinese
    });
    loadChain(m_boldFonts, {
        {"segoeuib.ttf", "arialbd.ttf"},
        {"seguisym.ttf"},
        {"seguiemj.ttf"},
        {"YuGothB.ttc", "YuGothM.ttc", "meiryob.ttc", "meiryo.ttc", "msgothic.ttc"},
        {"malgunbd.ttf", "malgun.ttf"},
        {"msyhbd.ttc", "msyh.ttc", "simsun.ttc"},
    });

    if (m_regularFonts.empty() || m_boldFonts.empty()) {
        logPopup("init failed: could not load any font from " + fonts);
        return false;
    }
    logPopup("fonts loaded: " + std::to_string(m_regularFonts.size()) + " regular, " +
             std::to_string(m_boldFonts.size()) + " bold");

    // --- offscreen surface the card is drawn on ---
    try {
        m_rt.emplace(sf::Vector2u(kWidth, kHeight));
    } catch (const std::exception& e) {
        logPopup(std::string("init failed: could not create the offscreen render texture: ") + e.what());
        return false;
    }

    // --- text objects (re-filled by show()) ---
    m_label.emplace(*m_regularFonts[0], "NOW PLAYING", 10);
    m_label->setFillColor(kAccent);
    m_label->setPosition(sf::Vector2f(kTextX, 6.f));

    // --- the window: layered (per-pixel alpha), topmost, click-through, never focused, not in the taskbar ---
    HINSTANCE inst = GetModuleHandleA(nullptr);

    WNDCLASSEXA wc;
    std::memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = "MishNowPlayingPopup";
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        logPopup("init failed: RegisterClassEx error " + std::to_string(GetLastError()));
        return false;
    }

    HWND hwnd = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "MishNowPlayingPopup", "mish now playing", WS_POPUP,
        0, 0, static_cast<int>(kWidth), static_cast<int>(kHeight),
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        logPopup("init failed: CreateWindowEx error " + std::to_string(GetLastError()));
        return false;
    }
    m_hwnd = hwnd;

    // --- 32-bit top-down bitmap that holds the pre-multiplied pixels handed to Windows ---
    BITMAPINFO bi;
    std::memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(kWidth);
    bi.bmiHeader.biHeight = -static_cast<LONG>(kHeight); // negative = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!memDC || !dib || !bits) {
        logPopup("init failed: could not create the bitmap (CreateDIBSection)");
        if (memDC) DeleteDC(memDC);
        if (dib) DeleteObject(dib);
        return false;
    }
    SelectObject(memDC, dib);
    m_memDC = memDC;
    m_dib = dib;
    m_bits = bits;

    logPopup("init ok");
    return true;
}

// Lay out one line of text, switching fonts per character, and shorten it with an
// ellipsis until it fits in maxWidth pixels
void NowPlayingPopup::layoutLine(std::vector<sf::Text>& runs, const std::string& utf8, const FontChain& chain,
                                 unsigned characterSize, sf::Color color, float x, float y, float maxWidth)
{
    std::u32string text = sf::String::fromUtf8(utf8.begin(), utf8.end()).toUtf32();
    text.erase(std::remove_if(text.begin(), text.end(), isIgnorable), text.end());

    // Build the runs for `str`; returns its total width in pixels
    auto build = [&](const std::u32string& str) -> float {
        runs.clear();
        float cursor = x;
        std::size_t i = 0;
        while (i < str.size()) {
            const std::size_t fontIndex = pickFont(chain, str[i]);
            std::size_t j = i + 1;
            while (j < str.size() && pickFont(chain, str[j]) == fontIndex)
                ++j;

            sf::Text run(*chain[fontIndex], sf::String(str.substr(i, j - i)), characterSize);
            run.setFillColor(color);
            run.setPosition(sf::Vector2f(cursor, y));
            cursor = run.findCharacterPos(j - i).x; // global x just after the run's last character
            runs.push_back(std::move(run));
            i = j;
        }
        return cursor - x;
    };

    float width = build(text);
    while (!text.empty() && width > maxWidth) {
        text.pop_back();
        width = build(text + U"\u2026");
    }
}

// Draw the card offscreen once per track, then convert it for Windows
void NowPlayingPopup::renderContent()
{
    m_rt->clear(kBackground);

    sf::RectangleShape border(sf::Vector2f(static_cast<float>(kWidth), static_cast<float>(kHeight)));
    border.setFillColor(sf::Color::Transparent);
    border.setOutlineColor(kBorder);
    border.setOutlineThickness(-1.f); // draw the outline inside the card
    m_rt->draw(border);

    sf::RectangleShape accent(sf::Vector2f(4.f, static_cast<float>(kHeight)));
    accent.setFillColor(kAccent);
    m_rt->draw(accent);

    if (m_art) {
        const sf::Vector2u size = m_art->getSize();
        if (size.x > 0 && size.y > 0) {
            sf::Sprite sprite(*m_art);
            sprite.setPosition(sf::Vector2f(kArtX, kArtY));
            sprite.setScale(sf::Vector2f(kArtSize / size.x, kArtSize / size.y));
            m_rt->draw(sprite);
        }
    } else {
        sf::RectangleShape placeholder(sf::Vector2f(kArtSize, kArtSize));
        placeholder.setPosition(sf::Vector2f(kArtX, kArtY));
        placeholder.setFillColor(sf::Color(45, 45, 45));
        m_rt->draw(placeholder);
    }

    m_rt->draw(*m_label);
    for (const sf::Text& run : m_titleRuns)
        m_rt->draw(run);
    for (const sf::Text& run : m_artistRuns)
        m_rt->draw(run);
    for (const sf::Text& run : m_albumRuns)
        m_rt->draw(run);
    m_rt->display();

    // RGBA (straight alpha) -> BGRA (pre-multiplied alpha), as UpdateLayeredWindow expects
    const sf::Image img = m_rt->getTexture().copyToImage();
    const std::uint8_t* src = img.getPixelsPtr();
    std::uint8_t* dst = static_cast<std::uint8_t*>(m_bits);
    const std::size_t count = static_cast<std::size_t>(kWidth) * kHeight;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned a = src[i * 4 + 3];
        dst[i * 4 + 0] = static_cast<std::uint8_t>(src[i * 4 + 2] * a / 255); // B
        dst[i * 4 + 1] = static_cast<std::uint8_t>(src[i * 4 + 1] * a / 255); // G
        dst[i * 4 + 2] = static_cast<std::uint8_t>(src[i * 4 + 0] * a / 255); // R
        dst[i * 4 + 3] = static_cast<std::uint8_t>(a);
    }
}

void NowPlayingPopup::show(const std::string& title,
                           const std::string& artist,
                           const std::string& album,
                           std::shared_ptr<sf::Texture> art)
{
    if (!m_hwnd)
        return;

    layoutLine(m_titleRuns, title.empty() ? "Unknown title" : title, m_boldFonts, 16, kTextMain,
               kTextX, 20.f, kTextMaxWidth);
    layoutLine(m_artistRuns, artist, m_regularFonts, 13, kTextMain, kTextX, 45.f, kTextMaxWidth);
    layoutLine(m_albumRuns, album, m_regularFonts, 12, kTextDim, kTextX, 65.f, kTextMaxWidth);

    m_art = std::move(art);
    if (m_art)
        m_art->setSmooth(true); // the cover is scaled down a lot

    renderContent();

    // Bottom-right corner of the work area (the desktop minus the taskbar)
    RECT rc{0, 0, 1920, 1080};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
    m_baseX = rc.right - static_cast<int>(kWidth) - kMargin;
    m_baseY = rc.bottom - static_cast<int>(kHeight) - kMargin;

    m_clock.restart();
    m_active = true;
    m_loggedPresentFailure = false;

    HWND hwnd = static_cast<HWND>(m_hwnd);
    ShowWindow(hwnd, SW_SHOWNA); // show without activating
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    logPopup("show: \"" + title + "\" at " + std::to_string(m_baseX) + "," + std::to_string(m_baseY) +
             " (work area right=" + std::to_string(rc.right) + " bottom=" + std::to_string(rc.bottom) + ")");
}

void NowPlayingPopup::hide()
{
    if (!m_hwnd)
        return;
    m_active = false;
    ShowWindow(static_cast<HWND>(m_hwnd), SW_HIDE);
}

void NowPlayingPopup::update()
{
    if (!m_hwnd || !m_active)
        return;

    const float ms = static_cast<float>(m_clock.getElapsedTime().asMilliseconds());
    if (ms >= kSlideMs + kHoldMs + kFadeMs) {
        hide();
        return;
    }

    // Animation: slide + fade in, hold, fade out
    float alpha = 1.f;
    float slide = 0.f;
    if (ms < kSlideMs) {
        const float t = ms / kSlideMs;
        const float eased = 1.f - (1.f - t) * (1.f - t); // ease-out
        alpha = eased;
        slide = (1.f - eased) * kSlideDistance;
    } else if (ms > kSlideMs + kHoldMs) {
        alpha = 1.f - (ms - kSlideMs - kHoldMs) / kFadeMs;
    }
    alpha = std::clamp(alpha, 0.f, 1.f);

    POINT pos;
    pos.x = m_baseX + static_cast<LONG>(slide);
    pos.y = m_baseY;
    SIZE size;
    size.cx = static_cast<LONG>(kWidth);
    size.cy = static_cast<LONG>(kHeight);
    POINT srcPt;
    srcPt.x = 0;
    srcPt.y = 0;

    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = static_cast<BYTE>(alpha * 245.f);
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screen = GetDC(nullptr);
    const BOOL ok = UpdateLayeredWindow(static_cast<HWND>(m_hwnd), screen, &pos, &size,
                                        static_cast<HDC>(m_memDC), &srcPt, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);

    if (!ok && !m_loggedPresentFailure) {
        m_loggedPresentFailure = true;
        logPopup("UpdateLayeredWindow failed, error " + std::to_string(GetLastError()));
    }
}

#else // ---------------------------------------------------------------------------
// Non-Windows: the pop-up relies on Win32 layered windows, so it's unavailable here.
// init() returns false and everything else does nothing, so main.cpp builds and runs
// unchanged.
// ---------------------------------------------------------------------------

NowPlayingPopup::~NowPlayingPopup() {}

bool NowPlayingPopup::init() { return false; }

void NowPlayingPopup::show(const std::string&, const std::string&, const std::string&,
                           std::shared_ptr<sf::Texture>) {}

void NowPlayingPopup::update() {}

void NowPlayingPopup::hide() {}

void NowPlayingPopup::layoutLine(std::vector<sf::Text>&, const std::string&, const FontChain&, unsigned,
                                 sf::Color, float, float, float) {}

void NowPlayingPopup::renderContent() {}

#endif // _WIN32