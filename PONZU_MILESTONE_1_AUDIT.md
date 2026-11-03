# Ponzu — Milestone 1 Source Audit

**Input:** `mish-ponzu.zip`  
**Audit status:** Source inspected. Foundation pass written (section 11) but **not compiled**: no SFML, TagLib or RmlUi were available in the authoring environment.  
**Chosen direction:** maximized borderless window, elaborate citrus splash, preserve existing Mish functionality.

## 1. Current technical baseline

| Area | Finding |
|---|---|
| Language | C++ |
| Window/rendering | SFML 3.2 (vendored under `src/SFML`) |
| UI | Dear ImGui + ImGui-SFML, vendored in `src/` |
| Audio | SFML Audio (`sf::SoundBuffer` + `sf::Sound`) |
| Metadata | TagLib |
| Supported local formats | MP3, OGG, WAV, FLAC |
| Album art | ID3 APIC for MP3 and FLAC picture metadata |
| Visualizer | Custom SFML implementation in `visualizer.cpp/.h` |
| Discord | Custom Discord IPC implementation in `discordrpc.cpp/.h` |
| Platform helpers | Windows, Linux, and macOS source files are present |
| Build system | Three handwritten Makefiles; no unified top-level CMake project |

The project currently has SFML 3.2, confirmed by `src/SFML/include/SFML/Config.hpp`.

## 2. Main structural finding

`src/mishplay.cpp` is approximately 1,043 lines and currently owns several unrelated responsibilities:

- window creation and event loop;
- ImGui initialization, font setup, and all visible UI;
- configuration read/write;
- library folder scanning and metadata extraction;
- track selection and playback state;
- repeat and shuffle behavior;
- visualizer updates;
- Discord presence updates;
- now-playing notification integration.

This is the central refactor target. The UI migration should not require rewriting the playback or metadata behavior. First extract stable application services, then replace the presentation layer.

## 3. Existing features to preserve

- Recursive local folder scan, currently every 120 seconds.
- MP3, OGG, WAV, and FLAC discovery.
- TagLib title, artist, and album metadata.
- Embedded MP3 and FLAC artwork loading.
- Unicode-aware filesystem path handling through `pathutil.h`.
- Playback, pause, stop, next, previous, shuffle, and repeat modes.
- Visualizer calm/portal modes and resize handling.
- Discord Rich Presence on supported builds.
- Windows now-playing popup.
- Folder selection helpers for Windows and Linux.

Preservation should be tested feature-by-feature. In particular, keep the existing `Track` filesystem path and UTF-8 display path distinction; it is deliberate and helps avoid Unicode path regressions.

## 4. Configuration reality check

The current `mish_config.txt` is not a database. It contains one line: the configured music-folder path. The source reads and writes it relative to the process working directory.

Some runtime options are not persisted today:

- repeat mode;
- popup enabled state;
- current track / playback position;
- window dimensions and position.

The README also mentions `imgui.ini`, but the inspected application does not explicitly configure or manage it. It is ImGui's own layout persistence, not the application's music-library database.

### Ponzu configuration proposal

Use a per-user application-data directory, not the executable's working directory. Store preferences in `config.dat` using a versioned binary container:

- fixed magic identifier, e.g. `PONZCFG`;
- format/schema version;
- serialized MessagePack payload;
- optional checksum for corruption detection.

Write changes atomically (temporary file, flush, then replace) and keep a backup of the previous valid config. The `.dat` extension is a filename convention; MessagePack is the actual serialization format.

Suggested configuration fields:

- display name / greeting preferences;
- theme and citrus accent settings;
- window state and dimensions;
- library folder paths and scan interval;
- playback preferences;
- last selected page;
- integration preferences.

Use a separate `library.db` (SQLite) only when we introduce a persistent indexed library, playlists, richer track records, or queryable listening history. Do not turn `config.dat` into a database-shaped dumping ground.

### Mish migration behavior

On first Ponzu launch:

1. Check the new Ponzu config location.
2. If no valid Ponzu config exists, look for `mish_config.txt` in the legacy working directory and any explicitly supported legacy location.
3. Import the configured music folder if it still exists.
4. Write a new versioned `config.dat`.
5. Preserve the original Mish config unchanged; optionally copy it to a clearly named backup.
6. Never delete the old config automatically.

Only migrate values that actually exist in the old application. Do not pretend transient settings were saved by Mish when they were not.

## 5. RmlUi compatibility assessment

RmlUi is a viable candidate for the requested refresh. Current upstream RmlUi documentation lists an SFML platform backend and an `SFML_GL2` backend, and the upstream changelog states that its SFML backend supports both SFML 2 and SFML 3. The SFML 3 selection can be made explicit with `RMLUI_SFML_VERSION_MAJOR=3` during RmlUi's CMake configuration.

Recommended integration strategy:

- keep SFML as the application window, audio, texture, and visualizer foundation;
- integrate RmlUi's SFML platform and OpenGL 2 renderer components with the existing SFML window;
- do not run a second independent window loop from an RmlUi sample shell;
- route SFML events to the RmlUi context and keep the application's event ownership clear;
- verify texture sharing / custom texture loading for SFML album art;
- keep custom native window hit-testing and drag/resize behavior in a separate platform layer.

Important limitation: the documented SFML backend covers basic windowing and input integration. It does not automatically provide Ponzu's custom frameless title bar, platform-native snap behavior, or every high-DPI detail. Those need separate platform work and testing on Windows, macOS, and Linux.

## 6. Proposed Ponzu source layout

```text
ponzu/
├── CMakeLists.txt
├── cmake/
├── assets/
│   ├── fonts/
│   │   ├── DelaGothicOne.ttf
│   │   └── InterfaceSans.ttf
│   ├── branding/
│   │   ├── ponzu-logo.*
│   │   └── citrus/
│   └── icons/
├── ui/
│   ├── documents/
│   │   ├── splash.rml
│   │   ├── main.rml
│   │   ├── listen-now.rml
│   │   ├── albums.rml
│   │   ├── artists.rml
│   │   ├── songs.rml
│   │   └── settings.rml
│   └── styles/
│       ├── ponzu.rcss
│       ├── layout.rcss
│       └── components.rcss
├── src/
│   ├── main.cpp
│   ├── app/              # lifecycle, startup, application state
│   ├── audio/            # playback engine and queue
│   ├── library/          # scanning, tracks, indexing
│   ├── metadata/         # TagLib readers and artwork
│   ├── config/           # config.dat, migrations, serialization
│   ├── ui/               # RmlUi host, documents, bindings
│   ├── platform/         # native window controls and dialogs
│   ├── visualizer/
│   └── integrations/     # Discord and notifications
└── tests/
```

This is a target structure, not a claim that all these files already exist. We should extract modules incrementally rather than move everything in one risky pass.

## 7. Build-system recommendation

Move toward one top-level CMake project for all three desktop platforms. The existing Makefiles have diverged:

- Windows links static SFML and several platform libraries directly.
- Linux uses a different dependency list and currently includes curl.
- macOS references a platform-specific Objective-C++ source and Homebrew paths.

CMake can express SFML, TagLib, RmlUi, and platform-specific system dependencies once, with generator/platform conditions. Keep the vendored SFML 3.2 source only if there is a concrete reason to pin/build it from source; otherwise prefer a reproducible dependency strategy and remove the full SFML source tree from the application repository.

Do not delete the vendored SFML tree until all three platform builds are reproducible against the selected dependency source.

## 8. Design and startup decisions recorded

- Product name: **Ponzu**; described as a refresh of Mish, not an unrelated replacement.
- Initial window: maximized, borderless (not exclusive fullscreen).
- Window chrome: custom in-window minimize, maximize/restore, and close controls.
- Startup: elaborate citrus-themed splash screen with logo and real loading stages.
- Main page: Apple Music-inspired Listen Now / editorial layout, familiar navigation, large local album artwork, persistent bottom player.
- Greeting: personal and playful, time-aware, e.g. “Good afternoon, Josh.” followed by a rotating zany phrase.
- Logo type: Dela Gothic One; reserve it primarily for the wordmark and selected headings.
- Palette: tea orange `#923C01`, bright orange `#FF5F00`, warm dark backgrounds, restrained cream text.
- Citrus graphics: branding, splash, empty states, and subtle decoration; album artwork remains visually dominant.

## 9. Milestone 1 exit criteria

Before replacing ImGui, complete these checks:

- [ ] Establish a clean Ponzu branch/repository snapshot.
- [ ] Build the untouched Mish baseline on at least one target platform and record compiler/dependency versions.
- [ ] Confirm the SFML 3.2 backend path against the chosen RmlUi release.
- [ ] Create the unified CMake skeleton without changing runtime behavior.
- [ ] Identify and pin dependency versions for SFML, TagLib, RmlUi, and MessagePack.
- [ ] Add a small RmlUi + SFML 3.2 integration test window before integrating the full app.
- [ ] Define config schema version 1 and test Mish config import.
- [ ] Capture baseline behavior for playback, scanning, artwork, Discord, and visualizer.

## 10. Immediate next implementation step

Create a separate `ponzu-foundation` branch and introduce the top-level CMake project while retaining the current executable as a baseline target. Then build a tiny SFML 3.2 + RmlUi proof-of-integration window. Only after that compiles and receives input correctly should we start extracting the existing app services and building the splash screen.

## References

- RmlUi upstream repository/readme: https://github.com/mikke89/RmlUi
- RmlUi integration manual: https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/integrating.html
- RmlUi CMake build guide: https://mikke89.github.io/RmlUiDoc/pages/cpp_manual/building_with_cmake.html
- SFML build and linking guidance: https://www.sfml-dev.org/faq/build-use/


## 11. Foundation pass (written, not yet built)

### Decisions
- **Dependencies:** SFML 3 and TagLib as **dynamic** libraries from vcpkg (`x64-windows` triplet). vcpkg builds them once and caches them; the vcpkg toolchain copies the DLLs next to `ponzu.exe`. The vendored `src/SFML` tree is gone.
- **RmlUi:** fetched by CMake (`PONZU_RMLUI_TAG`, default `6.1`, **unverified**), built once per build folder. Its `RmlUi_Platform_SFML` and `RmlUi_Renderer_GL2` backend files are compiled into Ponzu; our own loop stays in charge (no sample shell).
- **Mish config import:** implemented as audited (read `mish_config.txt` from the working directory, only the music folder, never delete). Config v1 is a plain `config.txt` under the app-data folder; the MessagePack `config.dat` stays planned and only needs `config::load/save` swapped.

### Removed (dead weight)
| Removed | Why |
|---|---|
| `src/SFML/` (examples, tests, docs, Android/iOS projects, `extlibs`) | replaced by dynamic SFML; bulk of the 31 MB / 1,039 files |
| `imgui*.cpp/.h`, `imstb_*.h`, `imgui-SFML.*`, `imconfig*.h`, `imgui_demo.cpp` | replaced by RmlUi |
| `Makefile`, `Makefile.linux`, `Makefile.macos`, `test_macos.sh`, `macos.yml` (old CI) | replaced by one `CMakeLists.txt` (CI to be rewritten) |
| `assets/imgui.png`, `playergui.png`, `setup_gui.png`, `selected.png`, `mish_config.png`, `screenshot.png` | screenshots of the ImGui UI (new ones to be taken); the icon stays in `src/` for the resource script and in `assets/` for Linux |

### Refactor of `mishplay.cpp`
| New file | Takes over |
|---|---|
| `library.cpp/.hpp` | folder scan, TagLib tags, embedded art (as bytes, only for the shown track) |
| `player.cpp/.hpp` | queue, repeat, shuffle, auto-advance, seek |
| `config.cpp/.hpp` | app-data config, Mish import, atomic save + `.bak` |
| `ui.cpp/.hpp`, `ui/` | RmlUi host, Listen Now / Songs views, splash, setup |
| `window_platform.*` | borderless-window helpers (work area, drag, minimize, exe dir) |
| `main.cpp` | wiring only |

Kept unchanged: `visualizer.*`, `discordrpc.*`, `nowplaying_popup.*`, `pathutil.h`, `platform-*.{cpp,mm}`.

### Behaviour changes to be aware of
1. **Likely Mish bug fixed (from reading, not running):** the old loop auto-advanced whenever the sound was `Stopped` and `userStopped` was false. Right after startup the first track is loaded but not playing, so it appeared to jump straight to track 2 and start playing. `Player` starts "user stopped" until you press play.
2. Art for every track used to be decoded into a GL texture on every scan (every 120 s). Now art is read for the current track only, and scanning runs on a worker thread because it no longer touches GL.
3. Shuffle still reorders the list in place, and the next rescan re-sorts it (as before). A real shuffled queue is a later change.
4. Config moved out of the working directory; `imgui.ini` is no longer produced.
5. Test hook renamed `MISH_TEST_AUTOPLAY` -> `PONZU_TEST_AUTOPLAY`.

### Not built / not verified: check these on first compile
- RmlUi API names written from memory of RmlUi 6.x: `RenderInterface_GL2::LoadTexture/GenerateTexture(Span, size)`, `SetViewport`, `BeginFrame/EndFrame`, `RmlSFML::InputHandler(ctx, const sf::Event&)`, `Element::GetAbsoluteOffset(BoxArea)`. Fix against the tag actually pinned.
- Whether the GL2 `BeginFrame()` clears the colour buffer. If it does, the visualizer backdrop will vanish: render the visualizer into a texture and show it as a UI background instead.
- The GL header/loader the GL2 backend expects on Windows (likely first link/compile error).
- vcpkg package names/versions: confirm the `sfml` port is 3.x and what `taglib` exports (`TagLib::tag`).
- Not done yet: Albums/Artists/Settings pages, time-aware greeting is wired but fonts need `DelaGothicOne-Regular.ttf` in `assets/fonts/`, Linux/macOS window drag and minimize (stubs), CI.
- Milestone 1 checklist items still open: baseline build of untouched Mish, pinned dependency versions, RmlUi+SFML test window, config tests, behaviour capture.
