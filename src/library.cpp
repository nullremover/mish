#include "library.hpp"
#include "pathutil.h"

#include <taglib/attachedpictureframe.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/flacpicture.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tag.h>

#include <algorithm>
#include <iostream>

namespace fs = std::filesystem;

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool supported(const fs::path& p) {
    const std::string ext = lowerAscii(pathutil::toUtf8(p.extension()));
    return ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac";
}

std::vector<std::uint8_t> toBytes(const TagLib::ByteVector& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(v.data());
    return {p, p + v.size()};
}

} // namespace

namespace library {

std::vector<Track> scan(const fs::path& folder, std::atomic<size_t>* progress) {
    std::vector<Track> out;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return out;

    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !supported(it->path())) continue;

        Track t;
        t.path     = it->path();                           // exact path, used to open the file
        t.fullPath = pathutil::toUtf8(t.path);             // UTF-8 copy for display/compare/Discord
        t.filename = pathutil::toUtf8(t.path.filename());
        t.title    = t.filename;

        TagLib::FileRef f(t.path.c_str());                 // wide path on Windows: handles any character
        if (!f.isNull() && f.tag()) {
            const TagLib::Tag* tag = f.tag();
            if (!tag->title().isEmpty())  t.title  = tag->title().to8Bit(true);
            if (!tag->artist().isEmpty()) t.artist = tag->artist().to8Bit(true);
            if (!tag->album().isEmpty())  t.album  = tag->album().to8Bit(true);
        }
        out.push_back(std::move(t));
        if (progress) ++*progress;
    }
    if (ec) std::cerr << "[Library] scan stopped early: " << ec.message() << '\n';

    // The iterator's order is arbitrary: sort so tracks group by folder and stay stable across scans/OSes.
    std::sort(out.begin(), out.end(), [](const Track& a, const Track& b) {
        return lowerAscii(a.fullPath) < lowerAscii(b.fullPath);
    });
    return out;
}

std::vector<std::uint8_t> readArt(const Track& track) {
    TagLib::FileRef f(track.path.c_str());
    if (f.isNull()) return {};

    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(f.file())) {
        if (auto* id3 = mpeg->ID3v2Tag()) {
            for (auto* frame : id3->frameList("APIC")) {
                if (auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame))
                    if (!pic->picture().isEmpty()) return toBytes(pic->picture());
            }
        }
    } else if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(f.file())) {
        const auto& pics = flac->pictureList();
        if (pics.isEmpty()) return {};
        const TagLib::FLAC::Picture* cover = pics.front();
        for (const auto* pic : pics)
            if (pic->type() == TagLib::FLAC::Picture::FrontCover) { cover = pic; break; }
        return toBytes(cover->data());
    }
    return {};
}

} // namespace library
