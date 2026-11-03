# ponzu

A refresh of **mish**, the minimalist music player. Citrusy, sour, and it says hello.

Status: milestone 1 foundation. The code has **not been compiled yet** (see `PONZU_MILESTONE_1_AUDIT.md`).

## Build (Windows, dynamic SFML via vcpkg)

```
git clone https://github.com/microsoft/vcpkg && vcpkg\bootstrap-vcpkg.bat
cmake -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
```

vcpkg builds SFML and TagLib the first time and caches them; later builds only compile Ponzu.
Drop `DelaGothicOne-Regular.ttf` into `assets/fonts/` (see the README there).

## Layout

```
src/library   scan folders, read tags + embedded art
src/player    queue, repeat/shuffle, auto-advance, SFML playback
src/config    per-user settings + Mish import
src/ui        RmlUi host (window <-> UI glue)
ui/           main.rml + styles/ponzu.rcss
src/main.cpp  wiring only
```

License: GPL v3 (see LICENSE.txt).
