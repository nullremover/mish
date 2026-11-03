#pragma once
// Library scanning + tag/artwork reading. No SFML or UI in here, so scans can run on a worker thread.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct Track {
    std::filesystem::path path;  // exact filesystem path: open files with THIS (Unicode-safe)
    std::string fullPath;        // UTF-8 copy for display / comparisons / Discord
    std::string filename;
    std::string title;
    std::string artist = "Unknown Artist";
    std::string album  = "Unknown Album";
};

namespace library {

// Recursively finds mp3/ogg/wav/flac files and reads title/artist/album. Sorted by path (case-insensitive).
// `progress` (optional) is incremented once per file found, for the splash screen.
std::vector<Track> scan(const std::filesystem::path& folder, std::atomic<size_t>* progress = nullptr);

// Embedded cover (MP3 APIC / FLAC picture) as raw encoded image bytes; empty if none.
// Called only for the track being shown, not for the whole library.
std::vector<std::uint8_t> readArt(const Track& track);

} // namespace library
