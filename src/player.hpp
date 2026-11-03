#pragma once
// Playback state: queue, repeat/shuffle, auto-advance. Owns the SFML buffer + sound.
// Behaviour matches Mish's old main loop, minus the UI.
#include "library.hpp"

#include <SFML/Audio.hpp>

#include <optional>
#include <vector>

class Player {
public:
    enum class Repeat { Off, One, All };

    // Replaces the track list. The current track stays selected if it is still in the new list.
    void setTracks(std::vector<Track> tracks);
    void clear();

    bool select(int index, bool autoplay);  // loads tracks[index]; false if the file could not be loaded
    void play();
    void pause();
    void stop();
    void next();                            // no-op on the last track (as in Mish)
    void prev();                            // no-op on the first track
    void shuffle();                         // shuffles the list in place and starts track 0
    void cycleRepeat();
    void seek(float seconds);
    void tick();                            // call every frame: auto-advance when a track ends

    const std::vector<Track>& tracks() const { return m_tracks; }
    int current() const { return m_cur; }
    const Track* currentTrack() const;
    Repeat repeat() const { return m_repeat; }

    bool  hasSound() const { return m_sound.has_value(); }
    bool  playing() const { return m_sound && m_sound->getStatus() == sf::Sound::Status::Playing; }
    bool  paused() const { return m_sound && m_sound->getStatus() == sf::Sound::Status::Paused; }
    float position() const { return m_sound ? m_sound->getPlayingOffset().asSeconds() : 0.f; }
    float duration() const { return m_sound ? m_buffer.getDuration().asSeconds() : 0.f; }

    // For the visualizer (needs the decoded samples).
    const sf::Sound* sound() const { return m_sound ? &*m_sound : nullptr; }
    const sf::SoundBuffer& buffer() const { return m_buffer; }

private:
    std::vector<Track> m_tracks;
    int m_cur = -1;
    Repeat m_repeat = Repeat::Off;
    bool m_userStopped = true;  // true => don't auto-advance. Starts true so loading a track never auto-plays.
    sf::SoundBuffer m_buffer;
    std::optional<sf::Sound> m_sound;
};
