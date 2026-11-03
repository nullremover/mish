#include "ui.hpp"
#include "window_platform.hpp"

#include <RmlUi/Core.h>
#include <RmlUi_Platform_SFML.h>   // from RmlUi's Backends/ folder (see CMakeLists.txt)
#include <RmlUi_Renderer_GL2.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default:  o += c;
        }
    }
    return o;
}

// RmlUi's stock GL2 renderer only loads TGA. Route image loading through sf::Image instead
// (png/jpg/bmp/...), and let the app hand it embedded album art under an "art://N" name.
class Renderer final : public RenderInterface_GL2 {
public:
    std::map<std::string, sf::Image> art;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        sf::Image img;
        if (source.rfind("art://", 0) == 0) {
            const auto it = art.find(source);
            if (it == art.end()) return 0;
            img = it->second;
        } else if (!img.loadFromFile(source)) {
            return 0;
        }
        const sf::Vector2u sz = img.getSize();
        dimensions = {static_cast<int>(sz.x), static_cast<int>(sz.y)};
        std::vector<Rml::byte> px(img.getPixelsPtr(), img.getPixelsPtr() + std::size_t(sz.x) * sz.y * 4);
        for (std::size_t i = 0; i < px.size(); i += 4) {   // RmlUi expects premultiplied alpha
            const unsigned a = px[i + 3];
            px[i] = Rml::byte(px[i] * a / 255);
            px[i + 1] = Rml::byte(px[i + 1] * a / 255);
            px[i + 2] = Rml::byte(px[i + 2] * a / 255);
        }
        return GenerateTexture(Rml::Span<const Rml::byte>(px.data(), px.size()), dimensions);
    }
};

void loadFonts(const std::filesystem::path& assets) {
    namespace fs = std::filesystem;
    std::error_code ec;
    // Wordmark / headings
    const fs::path dela = assets / "fonts" / "DelaGothicOne-Regular.ttf";
    if (fs::exists(dela, ec)) Rml::LoadFontFace(dela.string());
    else std::cerr << "[UI] " << dela << " not found: wordmark falls back to the body font\n";

    // Body + fallbacks (accents, symbols, Japanese) from the system, like Mish did
    std::vector<std::string> body, fallback;
#ifdef _WIN32
    const char* win = std::getenv("WINDIR");
    const std::string f = std::string(win ? win : "C:\\Windows") + "\\Fonts\\";
    body = {f + "segoeui.ttf", f + "arial.ttf"};
    fallback = {f + "seguisym.ttf", f + "YuGothM.ttc", f + "msgothic.ttc", f + "meiryo.ttc"};
#else
    body = {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc"};
    fallback = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"};
#endif
    // Load body fonts (try to load all valid paths for regular and bold variants)
    for (const auto& p : body) {
        if (fs::exists(p, ec)) {
            Rml::LoadFontFace(p);
        }
    }

    // Load fallback fonts
    for (const auto& p : fallback) {
        if (fs::exists(p, ec)) {
            Rml::LoadFontFace(p, /*fallback_face=*/true);
        }
    }
}

} // namespace

struct Ui::Impl final : public Rml::EventListener {
    Ui& owner;
    sf::RenderWindow& window;
    SystemInterface_SFML system;
    Renderer renderer;
    Rml::Context* ctx = nullptr;
    Rml::ElementDocument* doc = nullptr;
    std::unordered_map<std::string, std::string> cache;  // last value written per key: skip no-op updates
    int currentRow = -1;
    int artCounter = 0;

    explicit Impl(Ui& o, sf::RenderWindow& w) : owner(o), window(w) {}

    Rml::Element* el(const std::string& id) const { return doc ? doc->GetElementById(id) : nullptr; }

    void ProcessEvent(Rml::Event& ev) override {
        // Walk up from the clicked element to the nearest one carrying data-action.
        Rml::Element* e = ev.GetTargetElement();
        while (e && !e->HasAttribute("data-action")) e = e->GetParentNode();
        if (!e) return;
        const Rml::String action = e->GetAttribute<Rml::String>("data-action", "");
        const Rml::String type = ev.GetType();

        if (type == "mousedown") {
            if (action == "drag") platform::beginWindowDrag(window.getNativeHandle());
        } else if (type == "dblclick") {
            if (action == "drag" && owner.onAction) owner.onAction("maximize", "");
        } else if (type == "click") {
            if (action == "drag") return;
            if (action == "seek") {
                const float w = e->GetBox().GetSize(Rml::BoxArea::Border).x;
                const float x = ev.GetParameter("mouse_x", 0.f) - e->GetAbsoluteOffset(Rml::BoxArea::Border).x;
                if (w > 0 && owner.onSeek) owner.onSeek(std::clamp(x / w, 0.f, 1.f));
            } else if (owner.onAction) {
                owner.onAction(action, e->GetAttribute<Rml::String>("data-arg", ""));
            }
        }
    }
};

Ui::Ui(sf::RenderWindow& window) : m(std::make_unique<Impl>(*this, window)) {
    m->system.SetWindow(&window);
    Rml::SetSystemInterface(&m->system);
    Rml::SetRenderInterface(&m->renderer);
    if (!Rml::Initialise()) throw std::runtime_error("RmlUi failed to initialise");

    const auto size = window.getSize();
    m->renderer.SetViewport(static_cast<int>(size.x), static_cast<int>(size.y));
    m->ctx = Rml::CreateContext("main", Rml::Vector2i(static_cast<int>(size.x), static_cast<int>(size.y)));

    const auto root = platform::exeDir();
    loadFonts(root / "assets");
    m->doc = m->ctx->LoadDocument((root / "ui" / "main.rml").string());
    if (!m->doc) throw std::runtime_error("could not load ui/main.rml");
    m->doc->AddEventListener("click", m.get());
    m->doc->AddEventListener("dblclick", m.get());
    m->doc->AddEventListener("mousedown", m.get());
    m->doc->Show();
}

Ui::~Ui() { Rml::Shutdown(); }

void Ui::handle(const sf::Event& e) { RmlSFML::InputHandler(m->ctx, e); }

void Ui::resize(sf::Vector2u s) {
    if (s.x == 0 || s.y == 0) return;
    m->renderer.SetViewport(static_cast<int>(s.x), static_cast<int>(s.y));
    m->ctx->SetDimensions(Rml::Vector2i(static_cast<int>(s.x), static_cast<int>(s.y)));
}

void Ui::update() { m->ctx->Update(); }

void Ui::render() {
    m->window.pushGLStates();
    m->renderer.BeginFrame();
    m->ctx->Render();
    m->renderer.EndFrame();
    m->window.popGLStates();
}

void Ui::text(const std::string& id, const std::string& s) {
    auto& slot = m->cache["t:" + id];
    if (slot == s) return;
    slot = s;
    if (auto* e = m->el(id)) e->SetInnerRML(esc(s));
}

void Ui::show(const std::string& id, bool visible, const char* display) {
    auto& slot = m->cache["v:" + id];
    const std::string v = visible ? "1" : "0";
    if (slot == v) return;
    slot = v;
    if (auto* e = m->el(id)) e->SetProperty("display", visible ? display : "none");
}

void Ui::setClass(const std::string& id, const std::string& cls, bool on) {
    if (auto* e = m->el(id)) e->SetClass(cls, on);
}

void Ui::progress(const std::string& id, float f) {
    const std::string v = std::to_string(static_cast<int>(std::clamp(f, 0.f, 1.f) * 1000.f));  // 0.1% steps
    auto& slot = m->cache["p:" + id];
    if (slot == v) return;
    slot = v;
    if (auto* e = m->el(id)) e->SetProperty("width", std::to_string(std::stoi(v) / 10.0f) + "%");
}

void Ui::setLibrary(const std::vector<Track>& tracks) {
    std::string rml;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto n = std::to_string(i);
        rml += "<div class=\"row\" id=\"row-" + n + "\" data-action=\"track\" data-arg=\"" + n + "\">"
               "<span class=\"c-n\">" + std::to_string(i + 1) + "</span>"
               "<span class=\"c-t\">" + esc(tracks[i].title) + "</span>"
               "<span class=\"c-a\">" + esc(tracks[i].artist) + "</span>"
               "<span class=\"c-b\">" + esc(tracks[i].album) + "</span></div>";
    }
    if (auto* list = m->el("songs-list")) list->SetInnerRML(rml);
    m->currentRow = -1;
    text("songs-count", std::to_string(tracks.size()) + (tracks.size() == 1 ? " song" : " songs"));
}

void Ui::setCurrent(int index) {
    if (index == m->currentRow) return;
    if (auto* old = m->el("row-" + std::to_string(m->currentRow))) old->SetClass("on", false);
    m->currentRow = index;
    if (auto* now = m->el("row-" + std::to_string(index))) now->SetClass("on", true);
}

void Ui::setArt(const std::vector<std::uint8_t>& bytes) {
    m->renderer.art.clear();
    sf::Image img;
    const bool ok = !bytes.empty() && img.loadFromMemory(bytes.data(), bytes.size());
    if (ok) {
        const std::string key = "art://" + std::to_string(++m->artCounter);
        m->renderer.art[key] = std::move(img);
        for (const char* id : {"hero-art", "bar-art"})
            if (auto* e = m->el(id)) e->SetAttribute("src", key);
    }
    for (const char* id : {"hero-art", "bar-art"}) show(id, ok);
    for (const char* id : {"hero-noart", "bar-noart"}) show(id, !ok);
}
