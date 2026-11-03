#include "window_platform.hpp"

#if defined(_WIN32)
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
#elif defined(__APPLE__)
  #include <mach-o/dyld.h>
#else
  #include <unistd.h>
#endif

namespace platform {

sf::Rect<int> workArea() {
#if defined(_WIN32)
    RECT r{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0))
        return {{r.left, r.top}, {r.right - r.left, r.bottom - r.top}};
#endif
    const auto m = sf::VideoMode::getDesktopMode();
    return {{0, 0}, {static_cast<int>(m.size.x), static_cast<int>(m.size.y)}};
}

void beginWindowDrag(sf::WindowHandle h) {
#if defined(_WIN32)
    ReleaseCapture();
    SendMessageW(static_cast<HWND>(h), WM_NCLBUTTONDOWN, HTCAPTION, 0);
#else
    (void)h;  // TODO(linux/mac): _NET_WM_MOVERESIZE on X11 / performWindowDragWithEvent on Cocoa
#endif
}

void minimize(sf::WindowHandle h) {
#if defined(_WIN32)
    ShowWindow(static_cast<HWND>(h), SW_MINIMIZE);
#else
    (void)h;  // TODO(linux/mac)
#endif
}

std::filesystem::path exeDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::path(buf).parent_path();
    return ".";
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::filesystem::path(std::string(buf, n)).parent_path() : std::filesystem::path(".");
#endif
}

} // namespace platform
