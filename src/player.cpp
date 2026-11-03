#include "player.hpp"

#include <algorithm>
#include <iostream>
#include <random>

void Player::setTracks(std::vector<Track> tracks) {
    const std::string playing = currentTrack() ? currentTrack()->fullPath : std::string();
    m_tracks = std::move(tracks);
    m_cur = -1;
    for (int i = 0; i < (int)m_tracks.size(); ++i)
        if (!playing.empty() && m_tracks[i].fullPath == playing) { m_cur = i; break; }
}

void Player::clear() {
    m_sound.reset();
    m_tracks.clear();
    m_cur = -1;
    m_userStopped = true;
}

const Track* Player::currentTrack() const {
    return (m_cur >= 0 && m_cur < (int)m_tracks.size()) ? &m_tracks[m_cur] : nullptr;
}

bool Player::select(int index, bool autoplay) {
    if (index < 0 || index >= (int)m_tracks.size()) return false;
    m_cur = index;
    m_sound.reset();  // the sound references the buffer: drop it before reloading
    if (!m_buffer.loadFromFile(m_tracks[index].path)) {
        std::cerr << "[Audio] could not load: " << m_tracks[index].fullPath << '\n';
        return false;
    }
    m_sound.emplace(m_buffer);
    m_userStopped = !autoplay;
    if (autoplay) m_sound->play();
    return true;
}

void Player::play() {
    if (!m_sound || m_cur < 0) return;
    m_userStopped = false;
    m_sound->play();
}

void Player::pause() { if (m_sound) m_sound->pause(); }

void Player::stop() {
    m_userStopped = true;
    if (m_sound) m_sound->stop();
}

void Player::next() { if (m_cur + 1 < (int)m_tracks.size()) select(m_cur + 1, true); }
void Player::prev() { if (m_cur > 0) select(m_cur - 1, true); }

void Player::shuffle() {
    if (m_tracks.empty()) return;
    static std::mt19937 rng(std::random_device{}());
    std::shuffle(m_tracks.begin(), m_tracks.end(), rng);
    select(0, true);
}

void Player::cycleRepeat() {
    m_repeat = m_repeat == Repeat::Off ? Repeat::One : m_repeat == Repeat::One ? Repeat::All : Repeat::Off;
}

void Player::seek(float seconds) {
    if (m_sound) m_sound->setPlayingOffset(sf::seconds(seconds));
}

void Player::tick() {
    if (!m_sound || m_userStopped || m_cur < 0 || m_sound->getStatus() != sf::Sound::Status::Stopped) return;

    switch (m_repeat) {
        case Repeat::One: m_sound->play(); break;
        case Repeat::All: select((m_cur + 1) % (int)m_tracks.size(), true); break;
        case Repeat::Off:
            if (m_cur + 1 < (int)m_tracks.size()) select(m_cur + 1, true);
            else m_userStopped = true;  // last track finished: stop re-checking every frame
            break;
    }
}
