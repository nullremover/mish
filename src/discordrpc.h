#pragma once
// Minimal Discord Rich Presence client for mish (Windows).
// Talks to the local Discord client over its named pipe (\\.\pipe\discord-ipc-N),
// so there is no external library to build. Everything runs on a background
// thread; the functions below never block the UI.

#include <string>

namespace DiscordRPC {

enum class PlayState { Playing, Paused, Stopped };

struct Presence {
    std::string title;
    std::string artist;
    std::string album;
    std::string filePath;              // full path of the track (used to read its embedded cover)
    PlayState   state = PlayState::Stopped;
    double      positionSeconds = 0.0; // current position inside the track
    double      durationSeconds = 0.0; // total length of the track (0 = unknown: no progress bar)
};

// Start the background thread. Safe to call when Discord isn't running:
// it keeps retrying in the background.
void init(const std::string& applicationId);

// Call as often as you like (every frame is fine). It only talks to Discord
// when the track / play state changes or the user seeks. A Stopped state
// clears the presence.
void setPresence(const Presence& presence);

// Clears the presence and stops the background thread.
void shutdown();

} // namespace DiscordRPC