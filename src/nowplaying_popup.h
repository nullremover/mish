#pragma once
// Winamp-style "now playing" pop-up: a small always-on-top, click-through card that
// slides in at the bottom-right of the screen (above the taskbar), holds for a few
// seconds and fades out. It never takes focus and shows even when mish is minimised.
//
// Windows: the card is drawn offscreen with SFML and shown through a Win32 layered
// window (UpdateLayeredWindow), which doesn't depend on OpenGL window tricks.
// Other platforms: unavailable (init() returns false and everything is a no-op).

#include <SFML/Graphics.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class NowPlayingPopup {
public:
    NowPlayingPopup() = default;
    NowPlayingPopup(const NowPlayingPopup&) = delete;
    NowPlayingPopup& operator=(const NowPlayingPopup&) = delete;
    ~NowPlayingPopup();

    // Returns false if the pop-up can't be created (see mish_popup.log for why).
    // The other calls are then harmless no-ops.
    bool init();

    // Show the pop-up for a track (restarts the animation if already visible).
    void show(const std::string& title,
              const std::string& artist,
              const std::string& album,
              std::shared_ptr<sf::Texture> art);

    // Call once per frame. Does nothing while the pop-up is hidden.
    void update();

    void hide();

private:
    // A main font followed by fallback fonts (symbols, emoji, Japanese, Korean, Chinese).
    // SFML text uses a single font, so each character is drawn with the first font in the
    // chain that actually has a glyph for it.
    using FontChain = std::vector<std::unique_ptr<sf::Font>>;

    // Lays the string out as one or more sf::Text "runs" (one per font used), shortened with
    // an ellipsis if it is wider than maxWidth.
    void layoutLine(std::vector<sf::Text>& runs, const std::string& utf8, const FontChain& chain,
                    unsigned characterSize, sf::Color color, float x, float y, float maxWidth);
    void renderContent();

    FontChain m_regularFonts;
    FontChain m_boldFonts;
    std::optional<sf::Text> m_label; // plain ASCII, always the main font
    std::vector<sf::Text> m_titleRuns;
    std::vector<sf::Text> m_artistRuns;
    std::vector<sf::Text> m_albumRuns;
    std::optional<sf::RenderTexture> m_rt; // the card is drawn here, offscreen
    std::shared_ptr<sf::Texture> m_art;

    sf::Clock m_clock;
    bool m_active = false;
    bool m_loggedPresentFailure = false;
    int  m_baseX = 0;
    int  m_baseY = 0;

    // Win32 handles, stored as void* so this header stays platform-neutral
    void* m_hwnd = nullptr;
    void* m_memDC = nullptr;
    void* m_dib = nullptr;
    void* m_bits = nullptr;
};