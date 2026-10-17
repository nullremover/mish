#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <vector>

class Visualizer {
public:
    Visualizer(unsigned int width, unsigned int height);
    
    void update(const sf::Sound& sound, const sf::SoundBuffer& buffer);
    void draw(sf::RenderWindow& window);

    // Resize the internal buffers to match the window (keeps the visuals centred, unstretched)
    void resize(unsigned int width, unsigned int height);

    // Switch between full chaos mode and NCS-style calm mode
    void setCalmMode(bool enabled);
	void toggleCalmMode();
	bool isCalmMode() const;

private:
    unsigned int m_width;
    unsigned int m_height;
    
    // FFT and color tracking
    sf::Clock m_colorClock;
    std::vector<float> m_magnitudes; 

    // Milkdrop-style feedback system buffers (Order matters for C++ initialization!)
    sf::RenderTexture m_primaryBuffer;
    sf::RenderTexture m_feedbackBuffer;
    sf::Sprite m_feedbackSprite;
    
    // Graphic shapes driven by FFT
    float m_rotationAngle;
    sf::VertexArray m_innerRing;

    // Visualizer mode
    bool m_calmMode = false;
};

#endif // VISUALIZER_H