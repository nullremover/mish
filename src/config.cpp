#include "config.hpp"
#include "pathutil.h"

#include <cstdlib>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

namespace {
std::string env(const char* name) { const char* v = std::getenv(name); return v ? v : ""; }
fs::path file() { return config::dir() / "config.txt"; }
}

namespace config {

fs::path dir() {
#if defined(_WIN32)
    const std::string base = env("APPDATA");
    return fs::path(base.empty() ? "." : base) / "Ponzu";
#elif defined(__APPLE__)
    return fs::path(env("HOME")) / "Library" / "Application Support" / "Ponzu";
#else
    const std::string xdg = env("XDG_CONFIG_HOME");
    return xdg.empty() ? fs::path(env("HOME")) / ".config" / "ponzu" : fs::path(xdg) / "ponzu";
#endif
}

Config load() {
    Config c;
    std::ifstream in(file());
    if (in) {
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == "music_folder") c.musicFolder = val;
            else if (key == "display_name") c.displayName = val;
        }
    } else {
        // First Ponzu launch: pick up Mish's one-line config if it is still around. Only the music
        // folder existed in Mish, so only that is imported. The old file is left untouched.
        std::ifstream legacy("mish_config.txt");
        std::string folder;
        if (legacy && std::getline(legacy, folder) && !folder.empty() && fs::is_directory(pathutil::fromUtf8(folder))) {
            c.musicFolder = folder;
            std::cout << "[Config] imported music folder from mish_config.txt\n";
        }
    }
    if (c.displayName.empty()) c.displayName = env("USERNAME").empty() ? env("USER") : env("USERNAME");
    return c;
}

bool save(const Config& c) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path tmp = file().string() + ".tmp", bak = file().string() + ".bak";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << "music_folder=" << c.musicFolder << "\ndisplay_name=" << c.displayName << "\n";
        out.flush();
        if (!out) return false;
    }
    if (fs::exists(file(), ec)) fs::copy_file(file(), bak, fs::copy_options::overwrite_existing, ec);
    fs::rename(tmp, file(), ec);  // replaces the old file on Windows too (std::filesystem uses MoveFileEx)
    return !ec;
}

} // namespace config
