#pragma once
// Per-user settings, stored in the OS app-data folder (not the working directory).
// v1 is a plain key=value text file (config.txt). The audit plans a versioned MessagePack
// config.dat: replace load/save bodies when that dependency is pinned; the interface stays.
#include <filesystem>
#include <string>

struct Config {
    std::string musicFolder;   // UTF-8
    std::string displayName;   // used by the greeting; defaults to the OS user name
};

namespace config {
std::filesystem::path dir();   // %APPDATA%\Ponzu | ~/Library/Application Support/Ponzu | $XDG_CONFIG_HOME/ponzu
Config load();                 // imports Mish's mish_config.txt (working dir) on first run; never deletes it
bool   save(const Config& c);  // atomic: temp file + rename, previous config kept as config.txt.bak
}
