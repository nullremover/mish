#pragma once
// RmlUi host. Owns the RmlUi context + main document (ui/main.rml) and exposes a tiny API so the
// rest of the app never touches RmlUi types. The document is driven by element ids and data-action attributes.
#include "library.hpp"

#include <SFML/Graphics.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rml { class Context; class ElementDocument; }

class Ui {
public:
    explicit Ui(sf::RenderWindow& window);
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Window plumbing
    void handle(const sf::Event& e);   // forward every SFML event
    void resize(sf::Vector2u size);
    void update();                     // layout + animations; call once per frame
    void render();                     // draws on top of whatever is already in the window

    // Content
    void text(const std::string& id, const std::string& s);       // plain text (escaped), cached
    void show(const std::string& id, bool visible, const char* display = "block"); // display: <display>/none
    void setClass(const std::string& id, const std::string& cls, bool on);
    void progress(const std::string& id, float fraction);          // sets width: N% on the element
    void setLibrary(const std::vector<Track>& tracks);             // rebuilds the Songs list
    void setCurrent(int index);                                    // highlights a row
    void setArt(const std::vector<std::uint8_t>& encodedImage);    // empty => placeholder

    // UI -> app. action ids are the data-action values in main.rml; arg is data-arg / row index.
    std::function<void(const std::string& action, const std::string& arg)> onAction;
    std::function<void(float fraction)> onSeek;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};
