//Copyright 2026 Joshua Cajayon.

#include "visualizer.h"
#include <cmath>
#include <cstdint>
#include <complex>
#include <vector>
#include <algorithm>

typedef std::complex<float> Complex;
const float PI = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Calm mode tuning knobs
// ---------------------------------------------------------------------------
// Which of the 512 log-spaced bars the circle covers (bar 90 ~ 65 Hz, bar 470 ~ 9 kHz).
// The bars below ~65 Hz all come from the same 3-4 FFT bins (DC leakage + sub-bass),
// so they were hogging a third of the circle with one big smooth lump.
const int   CALM_LOW_BAR     = 90;
const int   CALM_HIGH_BAR    = 470;

// Spectral tilt: music loses energy as frequency rises, so boost highs / cut lows.
// 0 = no correction (old behaviour), ~0.5 = roughly flat for typical music.
const float CALM_TILT        = 0.5f;

// How many pixels of movement per unit of (tilted) magnitude, and the most the
// shape may ever grow. Excess is soft-clipped, so loud bands round off
// instead of hitting a hard flat ceiling.
const float CALM_GAIN        = 200.0f;
const float CALM_MAX_WAVE    = 350.0f;

const float CALM_BASE_RADIUS = 225.0f;

// Where the bass sits on the circle. PI/2 = bottom (shape is left/right symmetric),
// -PI/2 = top, 0 = right side (old behaviour).
const float CALM_ANGLE_OFFSET = PI / 2.0f;

// Cooley-Tukey FFT
void runFFT(std::vector<Complex>& a) {
    size_t n = a.size();
    if (n <= 1) return;

    std::vector<Complex> a0(n / 2), a1(n / 2);
    for (size_t i = 0; 2 * i < n; i++) {
        a0[i] = a[2 * i];
        a1[i] = a[2 * i + 1];
    }

    runFFT(a0);
    runFFT(a1);

    float angle = 2 * PI / n;
    Complex w(1), wn(std::cos(angle), -std::sin(angle));
    for (size_t i = 0; 2 * i < n; i++) {
        Complex t = w * a1[i];
        a[i] = a0[i] + t;
        a[i + n / 2] = a0[i] - t;
        w *= wn;
    }
}

Visualizer::Visualizer(unsigned int width, unsigned int height) 
    : m_width(width), 
      m_height(height), 
      m_primaryBuffer({width, height}), 
      m_feedbackBuffer({width, height}),
      m_feedbackSprite(m_feedbackBuffer.getTexture()), // Now safely binds the texture!
      m_rotationAngle(0.0f),
	  m_calmMode(false) {
    
    m_magnitudes.resize(512, 0.0f); // Map to 512 frequency bars

    m_primaryBuffer.clear(sf::Color::Black);
    m_feedbackBuffer.clear(sf::Color::Black);

    // Dynamic central ring driven by audio peaks
    m_innerRing = sf::VertexArray(sf::PrimitiveType::TriangleFan, 362); 
}

// Resize the off-screen buffers to match the window, so the visualizer is drawn
// 1:1 (no stretching) and stays centred (m_width/m_height drive the centre point).
void Visualizer::resize(unsigned int width, unsigned int height)
{
    if (width == 0 || height == 0)
        return; // minimised window
    if (width == m_width && height == m_height)
        return;

    m_width = width;
    m_height = height;

    (void)m_primaryBuffer.resize({width, height});
    (void)m_feedbackBuffer.resize({width, height});

    m_primaryBuffer.clear(sf::Color::Black);
    m_primaryBuffer.display();
    m_feedbackBuffer.clear(sf::Color::Black);
    m_feedbackBuffer.display();

    // The texture was re-created at a new size, so reset the sprite's texture rect too
    m_feedbackSprite.setTexture(m_feedbackBuffer.getTexture(), true);
}

void Visualizer::toggleCalmMode()
{
    m_calmMode = !m_calmMode;

    m_feedbackBuffer.clear(sf::Color::Black);
    m_feedbackBuffer.display();
}

void Visualizer::update(const sf::Sound& sound, const sf::SoundBuffer& buffer) {
    // High-velocity clock to drive fast-paced chaos and smooth orbits simultaneously
    float time = m_colorClock.getElapsedTime().asSeconds();
    float fastTime = time * 8.0f; 
    
    float bassIntensity = 0.0f;
    float midIntensity = 0.0f;
    float trebleIntensity = 0.0f;

    if (sound.getStatus() == sf::Sound::Status::Playing) {
        sf::Time offset = sound.getPlayingOffset();
        unsigned int sampleRate = buffer.getSampleRate();
        unsigned int channelCount = buffer.getChannelCount();
        const std::int16_t* samples = buffer.getSamples();
        
        std::uint64_t currentSampleIdx = static_cast<std::uint64_t>(offset.asSeconds() * sampleRate * channelCount);
        std::uint64_t totalSamples = buffer.getSampleCount();

        const size_t fftSize = 2048;
        std::vector<Complex> fftBuffer(fftSize, 0.0f);

        for (size_t i = 0; i < fftSize; ++i) {
            std::uint64_t idx = currentSampleIdx + (i * channelCount);
            if (idx < totalSamples) {
                float monoSample = 0.0f;
                for (unsigned int c = 0; c < channelCount; ++c) {
                    monoSample += samples[idx + c];
                }
                monoSample /= (channelCount * 32768.0f);

                float windowFactor = 0.5f * (1.0f - std::cos(2.0f * PI * i / (fftSize - 1)));
                fftBuffer[i] = monoSample * windowFactor;
            } else {
                fftBuffer[i] = 0.0f;
            }
        }

        runFFT(fftBuffer);

        float minFreq = 20.0f;
        float maxFreq = 16000.0f;
        float binWidth = static_cast<float>(sampleRate) / fftSize;

        for (unsigned int i = 0; i < 512; i++) {
            float normX = static_cast<float>(i) / 512.0f;
            float targetFreq = minFreq * std::pow(maxFreq / minFreq, normX);
            float binIndex = targetFreq / binWidth;

            size_t lowIdx = std::clamp(static_cast<size_t>(std::floor(binIndex)), size_t(0), fftSize / 2 - 2);
            size_t highIdx = lowIdx + 1;
            float interpolation = binIndex - lowIdx;

            float magLow = std::abs(fftBuffer[lowIdx]);
            float magHigh = std::abs(fftBuffer[highIdx]);
            float rawMagnitude = magLow + (magHigh - magLow) * interpolation;

            float normalizedMag = rawMagnitude / (fftSize / 2.0f);
            float processedMag = std::pow(normalizedMag * 100.0f, 0.6f);

            if (processedMag > m_magnitudes[i]) {
                m_magnitudes[i] = processedMag;
            } else {
                m_magnitudes[i] += (processedMag - m_magnitudes[i]) * 0.12f; 
            }

            if (i < 35)   bassIntensity += m_magnitudes[i];
            if (i >= 35 && i < 220) midIntensity += m_magnitudes[i];
            if (i >= 220) trebleIntensity += m_magnitudes[i];
        }
        bassIntensity /= 35.0f;
        midIntensity /= 185.0f;
        trebleIntensity /= 292.0f;
    } else {
        for (unsigned int i = 0; i < 512; i++) {
            m_magnitudes[i] += (0.0f - m_magnitudes[i]) * 0.08f;
        }
    }
    
	if (m_calmMode)
{
    sf::Vector2f center(
        m_width / 2.0f,
        m_height / 2.0f
    );

    m_primaryBuffer.clear(sf::Color(3, 5, 15));

    const int points = 128;
    const int halfPoints = points / 2;

    // --- 0. PRE-COMPUTE THE WAVE HEIGHT FOR EACH POINT ---
    // One value per mirrored point (0 = bass ... halfPoints = treble). Each point
    // averages a small window of neighbouring bars (instead of sampling a single
    // bar), applies the spectral tilt, then soft-clips. That fixes both the
    // blown-out bass side and the spiky treble side.
    float wave[halfPoints + 1];
    const float slotWidth = static_cast<float>(CALM_HIGH_BAR - CALM_LOW_BAR) / halfPoints;

    for (int p = 0; p <= halfPoints; p++)
    {
        float centerBar = CALM_LOW_BAR + (p / static_cast<float>(halfPoints)) * (CALM_HIGH_BAR - CALM_LOW_BAR);

        // Window is +/- one slot wide, so neighbouring points overlap and blend smoothly
        int lo = std::max(0, static_cast<int>(std::floor(centerBar - slotWidth)));
        int hi = std::min(511, static_cast<int>(std::ceil(centerBar + slotWidth)));

        float sum = 0.0f;
        for (int b = lo; b <= hi; b++)
        {
            float freq = 20.0f * std::pow(800.0f, b / 512.0f); // same log mapping as the FFT bars
            float tilt = std::pow(freq / 1000.0f, CALM_TILT);
            sum += m_magnitudes[b] * tilt;
        }
        float level = sum / (hi - lo + 1);

        // Soft clip: ~linear for small values, rounds off smoothly towards CALM_MAX_WAVE
        wave[p] = CALM_MAX_WAVE * std::tanh(level * CALM_GAIN / CALM_MAX_WAVE);
    }

    // --- 1. FILLED PORTAL MESH ---
    sf::VertexArray portal(
        sf::PrimitiveType::TriangleFan,
        points + 2
    );

    // Center glow
    portal[0].position = center;
    portal[0].color = sf::Color(220, 245, 255, 170);

    for (int i = 0; i <= points; i++)
    {
        float angle = (i / static_cast<float>(points)) * 2.0f * PI + CALM_ANGLE_OFFSET;

        // Mirror frequencies (0..64 rises bass->treble, 64..128 falls treble->bass)
        int mirroredI = (i <= halfPoints) ? i : (points - i);

        float breathing = std::sin(time * 2.0f + i * 0.15f) * 4.0f;

        float radius = CALM_BASE_RADIUS + wave[mirroredI] + breathing;

        portal[i + 1].position = {
            center.x + std::cos(angle) * radius,
            center.y + std::sin(angle) * radius
        };

        portal[i + 1].color = sf::Color(80, 200, 255, 150);
    }

    m_primaryBuffer.draw(portal);

    // --- 2. OUTER WAVEFORM OUTLINE ---
    sf::VertexArray outline(
        sf::PrimitiveType::LineStrip,
        points + 1
    );

    for (int i = 0; i <= points; i++)
    {
        float angle = (i / static_cast<float>(points)) * 2.0f * PI + CALM_ANGLE_OFFSET;

        int mirroredI = (i <= halfPoints) ? i : (points - i);

        float radius = CALM_BASE_RADIUS + wave[mirroredI];

        outline[i].position = {
            center.x + std::cos(angle) * radius,
            center.y + std::sin(angle) * radius
        };

        outline[i].color = sf::Color(120, 230, 255, 220);
    }

    m_primaryBuffer.draw(outline);

    // --- 3. REACTIVE CORE ---
    float coreSize = 4.0f + bassIntensity * 8.0f;

    sf::CircleShape core(coreSize);
    core.setOrigin({coreSize, coreSize});
    core.setPosition(center);
    core.setFillColor(sf::Color(230, 250, 255, 120));

    m_primaryBuffer.draw(core);

    m_primaryBuffer.display();

    m_feedbackBuffer.clear(sf::Color::Black);
    m_feedbackBuffer.display();

    return;
}

    // --- PHASE 1: THE FEEDBACK CYCLONE WARP ---
    m_feedbackSprite.setTexture(m_feedbackBuffer.getTexture(), true);
    m_feedbackSprite.setOrigin({m_width / 2.0f, m_height / 2.0f});

    // Subtly drift the screen center while injecting high-speed bass jitter (Combining styles!)
    float driftX = std::sin(time * 1.0f) * 12.0f;
    float driftY = std::cos(time * 1.3f) * 12.0f;
    float jitterX = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 14.0f * bassIntensity;
    float jitterY = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 14.0f * bassIntensity;
    
    m_feedbackSprite.setPosition({m_width / 2.0f + driftX + jitterX, m_height / 2.0f + driftY + jitterY});

    // Intense feedback zooming + dual-direction spin control
    float zoomFactor = 0.975f + (bassIntensity * 0.06f);
    float spinDirection = (std::sin(time * 0.4f) > 0.0f) ? 1.0f : -1.0f;
    float rotationDelta = (0.8f + (trebleIntensity * 8.0f)) * spinDirection;
    m_rotationAngle += rotationDelta;	
	m_rotationAngle = std::fmod(m_rotationAngle, 360.0f);

    m_feedbackSprite.setScale({zoomFactor, zoomFactor});
    m_feedbackSprite.setRotation(sf::degrees(m_rotationAngle * 0.12f));
    
    // Ghostly trails that shift color over time
    std::uint8_t trailR = static_cast<std::uint8_t>(245 + std::sin(time) * 8.0f);
    std::uint8_t trailG = static_cast<std::uint8_t>(245 + std::sin(time + 2.094f) * 8.0f);
    std::uint8_t trailB = static_cast<std::uint8_t>(245 + std::sin(time + 4.188f) * 8.0f);
    m_feedbackSprite.setColor(sf::Color(trailR, trailG, trailB, 246)); 

    m_primaryBuffer.clear(sf::Color(0, 0, 0, 255));
    m_primaryBuffer.draw(m_feedbackSprite);

    sf::Vector2f center(m_width / 2.0f, m_height / 2.0f);

    // --- PHASE 2: CHAOTIC GEOMETRIC FRACTURE WEB & SPARKS ---
    // Draw the unstable, sharp laser web spanning the dynamic outer bounds
    sf::VertexArray web(sf::PrimitiveType::LineStrip, 90);
    for (size_t i = 0; i < 90; ++i) {
        float angle = (i * 4.0f * PI / 45.0f) + (time * 0.8f);
        int magIdx = static_cast<int>(i * (512.0f / 90.0f)) % 512;
        
        float dist = 100.0f + (m_magnitudes[magIdx] * 290.0f) + (std::sin(angle * 6.0f + fastTime) * 35.0f);
        float x = center.x + std::cos(angle) * dist;
        float y = center.y + std::sin(angle) * dist;

        std::uint8_t r = static_cast<std::uint8_t>(std::sin(angle * 2.0f + time) * 127.0f + 128.0f);
        std::uint8_t g = static_cast<std::uint8_t>(std::cos(angle * 3.0f + time * 1.2f) * 127.0f + 128.0f);
        std::uint8_t b = static_cast<std::uint8_t>(std::sin(angle * 1.5f - time * 1.5f) * 127.0f + 128.0f);

        web[i].position = sf::Vector2f(x, y);
		web[i].color = sf::Color(r, g, b, 200);
    }
    m_primaryBuffer.draw(web);

    // Random explosive neon static sparks shooting out of the center
    if (bassIntensity > 0.45f || trebleIntensity > 0.35f) {
        sf::VertexArray sparks(sf::PrimitiveType::Lines, 16);
        for (size_t i = 0; i < 16; i += 2) {
            float angle = (static_cast<float>(rand()) / static_cast<float>(RAND_MAX)) * 2.0f * PI;
            float startDist = 30.0f + (bassIntensity * 50.0f);
            float endDist = startDist + 80.0f + (trebleIntensity * 180.0f);

            float startX = center.x + std::cos(angle) * startDist;
            float startY = center.y + std::sin(angle) * startDist;
            float endX = center.x + std::cos(angle) * endDist + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 40.0f;
            float endY = center.y + std::sin(angle) * endDist + (static_cast<float>(rand()) / static_cast<float>(RAND_MAX) - 0.5f) * 40.0f;

            std::uint8_t r = rand() % 256;
            std::uint8_t g = rand() % 256;
            std::uint8_t b = rand() % 256;

            sparks[i].position = sf::Vector2f(startX, startY);
			sparks[i].color = sf::Color(r, g, b, 255);

			sparks[i + 1].position = sf::Vector2f(endX, endY);
			sparks[i + 1].color = sf::Color(r, g, b, 0);
        }
        m_primaryBuffer.draw(sparks);
    }

    // --- PHASE 3: SMOOTH ORBITING ENERGY ORBS & THE VOID PORTAL ---
    // Smooth foreground orbs floating calmly over the geometric background chaos
    int numOrbs = 6;
    for (int k = 0; k < numOrbs; ++k) {
        float orbitAngle = (time * 1.2f) + (k * (2.0f * PI / numOrbs));
        int freqSegment = k * (512 / numOrbs);
        float pulseRadius = 80.0f + (m_magnitudes[freqSegment] * 180.0f);

        float ox = center.x + std::cos(orbitAngle) * pulseRadius;
        float oy = center.y + std::sin(orbitAngle) * pulseRadius;

        std::uint8_t r = static_cast<std::uint8_t>(std::sin(orbitAngle + time * 2.0f) * 127.0f + 128.0f);
        std::uint8_t g = static_cast<std::uint8_t>(std::sin(orbitAngle + time * 2.0f + 2.094f) * 127.0f + 128.0f);
        std::uint8_t b = static_cast<std::uint8_t>(std::sin(orbitAngle + time * 2.0f + 4.188f) * 127.0f + 128.0f);

        sf::CircleShape orb(6.0f + bassIntensity * 10.0f);
        orb.setOrigin({orb.getRadius(), orb.getRadius()});
        orb.setPosition({ox, oy});
        orb.setFillColor(sf::Color(r, g, b, 255));
        
        m_primaryBuffer.draw(orb);
    }

    // Soft dark center well to anchoring the visualizer depth
    sf::CircleShape portalCenter(25.0f + (bassIntensity * 15.0f));
    portalCenter.setOrigin({portalCenter.getRadius(), portalCenter.getRadius()});
    portalCenter.setPosition(center);
    portalCenter.setFillColor(sf::Color(0, 0, 0, 80)); 
    m_primaryBuffer.draw(portalCenter);

    m_primaryBuffer.display();

    // Copy to feedback buffer
    m_feedbackBuffer.clear(sf::Color::Black);
    sf::Sprite tempSprite(m_primaryBuffer.getTexture());
    m_feedbackBuffer.draw(tempSprite);
    m_feedbackBuffer.display();
}

void Visualizer::draw(sf::RenderWindow& window) {
    // Draw the final compound texture straight to our display window
    sf::Sprite finalDraw(m_primaryBuffer.getTexture());
    window.draw(finalDraw);
}

bool Visualizer::isCalmMode() const
{
    return m_calmMode;
}