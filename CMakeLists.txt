cmake_minimum_required(VERSION 3.24)
project(Ponzu VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(APPLE)
  enable_language(OBJCXX)
endif()

# ---- Dependencies --------------------------------------------------------------------------------
# SFML + TagLib come from vcpkg as DYNAMIC libraries (vcpkg builds them once and caches them, so they are
# not recompiled with the app; the vcpkg toolchain copies the DLLs next to ponzu.exe on Windows).
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
# Any other way of providing SFML 3 + TagLib (apt/brew/own build) works too: this only needs find_package().
set(PONZU_SFML_COMPONENTS Graphics Audio Window System)
if(WIN32)
  list(APPEND PONZU_SFML_COMPONENTS Main)
endif()
find_package(SFML 3 CONFIG REQUIRED COMPONENTS ${PONZU_SFML_COMPONENTS})

find_package(TagLib CONFIG QUIET)
if(NOT TagLib_FOUND)
  find_package(taglib CONFIG REQUIRED)
endif()
find_package(OpenGL REQUIRED)
find_package(Freetype REQUIRED)   # RmlUi's font engine

# RmlUi is fetched and built once per build folder (small, static). Its SFML/GL2 *backend* files are not part
# of the library, so we compile those two files ourselves; the event loop stays ours.
include(FetchContent)
set(PONZU_RMLUI_TAG "6.1" CACHE STRING "RmlUi release tag: verify it supports SFML 3 (audit milestone 1)")
set(BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(RmlUi
  GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
  GIT_TAG        ${PONZU_RMLUI_TAG}
  GIT_SHALLOW    ON)
FetchContent_MakeAvailable(RmlUi)

# ---- Sources -------------------------------------------------------------------------------------
set(PONZU_SOURCES
  src/main.cpp src/ui.cpp src/library.cpp src/player.cpp src/config.cpp
  src/visualizer.cpp src/window_platform.cpp
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Platform_SFML.cpp
  ${rmlui_SOURCE_DIR}/Backends/RmlUi_Renderer_GL2.cpp)

if(WIN32)
  list(APPEND PONZU_SOURCES src/platform-win.cpp src/discordrpc.cpp src/nowplaying_popup.cpp src/resource.rc)
elseif(APPLE)
  list(APPEND PONZU_SOURCES src/platform-mac.mm)          # no Discord / pop-up on macOS (as in Mish)
else()
  list(APPEND PONZU_SOURCES src/platform-linux.cpp src/discordrpc.cpp src/nowplaying_popup.cpp)
endif()

add_executable(ponzu ${PONZU_SOURCES})
if(WIN32)
  set_target_properties(ponzu PROPERTIES WIN32_EXECUTABLE ON)
endif()

target_include_directories(ponzu PRIVATE src ${rmlui_SOURCE_DIR}/Backends)
target_compile_definitions(ponzu PRIVATE RMLUI_SFML_VERSION_MAJOR=3)
target_link_libraries(ponzu PRIVATE
  SFML::Graphics SFML::Audio SFML::Window SFML::System
  RmlUi::RmlUi OpenGL::GL Freetype::Freetype)

# TagLib's imported target name differs between versions/packagings
foreach(t TagLib::tag TagLib::TagLib taglib::tag)
  if(TARGET ${t})
    target_link_libraries(ponzu PRIVATE ${t})
    break()
  endif()
endforeach()

if(WIN32)
  target_link_libraries(ponzu PRIVATE SFML::Main winhttp ole32 uuid gdi32 user32 winmm rpcrt4)
elseif(NOT APPLE)
  find_package(CURL REQUIRED)
  target_link_libraries(ponzu PRIVATE CURL::libcurl pthread)
endif()

# ---- Runtime files next to the executable ---------------------------------------------------------
add_custom_command(TARGET ponzu POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/ui     $<TARGET_FILE_DIR:ponzu>/ui
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:ponzu>/assets)
