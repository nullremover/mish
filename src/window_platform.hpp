#pragma once
// Small native helpers the borderless Ponzu window needs. (Folder dialog + app icon live in platform.h.)
#include <SFML/Window.hpp>
#include <SFML/Graphics/Rect.hpp>
#include <filesystem>

namespace platform {
sf::Rect<int> workArea();                      // desktop minus taskbar/dock where the OS tells us
void beginWindowDrag(sf::WindowHandle h);    // start an OS-native window move (call on titlebar mouse-down)
void minimize(sf::WindowHandle h);           // SFML has no minimize call
std::filesystem::path exeDir();              // where ui/ and assets/ are found at runtime
}
