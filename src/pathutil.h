#pragma once
// Unicode-safe conversions between std::filesystem::path and UTF-8 std::string.
//
// Why: on Windows a plain `path.string()` / `const char*` path is interpreted in the ANSI
// code page, which can't represent characters like a star or kanji. APIs that take a narrow
// string (TagLib's FileRef(const char*)) then fail to find the file, even though SFML
// (which takes a std::filesystem::path) plays it fine.
//   * keep the real fs::path around and open files with it (path.c_str() is wide on Windows)
//   * keep a UTF-8 std::string only for display, comparisons and passing between modules

#include <filesystem>
#include <string>

namespace pathutil {

inline std::string toUtf8(const std::filesystem::path& p)
{
#ifdef __cpp_lib_char8_t
    const std::u8string u = p.u8string(); // C++20: u8string() returns std::u8string
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
#else
    return p.u8string();                  // C++17: returns std::string (already UTF-8)
#endif
}

inline std::filesystem::path fromUtf8(const std::string& s)
{
#ifdef __cpp_lib_char8_t
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
#else
    return std::filesystem::u8path(s);
#endif
}

} // namespace pathutil
