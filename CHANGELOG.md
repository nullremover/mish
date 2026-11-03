# Changelog

## v0.3.0-dev (Ponzu, milestone 1 foundation)

Mish is now Ponzu: same player, new interface.

- Dear ImGui + vendored static SFML removed; RmlUi interface (`ui/`), SFML 3 as dynamic libraries via vcpkg.
- `mishplay.cpp` (1,043 lines) split into `library`, `player`, `config`, `ui` and a thin `main`.
- Config moves to the per-user app-data folder; Mish's `mish_config.txt` is imported once and left untouched.
- Album art is read only for the track being shown (Mish loaded a GL texture for every track on every scan).
- Fix (from reading the old loop, not yet run): Mish started playing track 2 right after launch.

Older history: see the Mish v0.2.4-alpha notes (calm mode, Discord RPC, FLAC/WAV fixes).
