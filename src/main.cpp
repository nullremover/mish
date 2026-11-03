// Ponzu: a refresh of Mish. SFML window/audio + RmlUi interface.
// This file only wires the pieces together; the work lives in library / player / config / ui.
#include "config.hpp"
#include "library.hpp"
#include "pathutil.h"
#include "platform.h"
#include "player.hpp"
#include "ui.hpp"
#include "visualizer.h"
#include "window_platform.hpp"

#include <SFML/Graphics.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iostream>
#include <random>

#ifndef __APPLE__
#include "discord_config.h"
#include "discordrpc.h"
#include "nowplaying_popup.h"
#endif

namespace fs = std::filesystem;

namespace {

constexpr float kAutoScanSeconds = 120.f;

std::string fmtTime(float s) {
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    return b;
}

std::string greeting(const std::string& name) {
    const std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    const char* part = lt.tm_hour < 5 ? "Still up" : lt.tm_hour < 12 ? "Good morning" : lt.tm_hour < 18 ? "Good afternoon" : "Good evening";
    return std::string(part) + (name.empty() ? "." : ", " + name + ".");
}

const char* phrase() {
    static const char* kPhrases[] = {
        "You call it a refresh, I call it refreshing.",
        "A little zest makes your songs brighter.",
        "Your library, cold off the press.",
        "Don't you like your juice with ice?'",
        "Make some noise.",
        "Citrus o'clock, tick tock.",
        "Zero pulp, all pop.",
        "Let's get a little tangy.",
        "Ugh, this orange has too much sour!",
    };
    static std::mt19937 rng(std::random_device{}());
    return kPhrases[rng() % (sizeof(kPhrases) / sizeof(kPhrases[0]))];
}

enum class Page { Splash, Setup, Main };

} // namespace

int main() {
    Config cfg = config::load();

    // Borderless, sized to the work area (maximized, but not exclusive fullscreen). Ponzu draws its own title bar.
    const sf::IntRect work = platform::workArea();
    sf::RenderWindow window(sf::VideoMode(sf::Vector2u(work.size)), "Ponzu", sf::Style::None);
    window.setPosition(work.position);
    window.setFramerateLimit(60);
    bool maximized = true;

    sf::Image icon;
    if (loadAppIcon(icon)) window.setIcon(icon);

    Ui ui(window);
    Visualizer visualizer(window.getSize().x, window.getSize().y);
    Player player;

#ifndef __APPLE__
    DiscordRPC::init(kDiscordAppId);
    NowPlayingPopup nowPlaying;
    const bool popupAvailable = nowPlaying.init();
    bool popupEnabled = true;
    std::string lastNotifiedPath;
    window.requestFocus();  // creating the pop-up window can briefly steal focus
#endif

    // ---- library loading (worker thread; tag reading only, no GL) ---------------------------------
    std::atomic<size_t> found{0};
    std::future<std::vector<Track>> scanJob;
    bool initialLoad = true;     // first scan of a folder: select track 0 (paused) when it finishes
    std::string pendingFolder;   // folder being scanned; becomes cfg.musicFolder once it has songs
    sf::Clock autoScan;
    Page page = Page::Splash;

    auto startScan = [&](const std::string& folder, bool initial) {
        pendingFolder = folder;
        initialLoad = initial;
        found = 0;
        scanJob = std::async(std::launch::async, [&found, folder] {
            return library::scan(pathutil::fromUtf8(folder), &found);
        });
    };
    auto setPage = [&](Page p) {
        page = p;
        ui.show("splash", p == Page::Splash);
        ui.show("setup", p == Page::Setup);
        ui.show("main", p == Page::Main);
    };

    std::string chosenFolder;  // setup page
    if (!cfg.musicFolder.empty() && fs::is_directory(pathutil::fromUtf8(cfg.musicFolder))) {
        setPage(Page::Splash);
        startScan(cfg.musicFolder, true);
    } else {
        setPage(Page::Setup);
    }

    // ---- UI -> app ---------------------------------------------------------------------------------
    auto showGreeting = [&] { ui.text("greeting", greeting(cfg.displayName)); ui.text("phrase", phrase()); };
    auto showView = [&](const std::string& v) {
        ui.show("view-listen", v == "listen");
        ui.show("view-songs", v == "songs", "flex");
        ui.setClass("nav-listen", "on", v == "listen");
        ui.setClass("nav-songs", "on", v == "songs");
        if (v == "listen") showGreeting();
    };
    showView("listen");

    ui.onSeek = [&](float f) { player.seek(f * player.duration()); };
    ui.onAction = [&](const std::string& a, const std::string& arg) {
        if (a == "play")         { if (player.playing()) player.pause(); else player.play(); }
        else if (a == "stop")    player.stop();
        else if (a == "next")    player.next();
        else if (a == "prev")    player.prev();
        else if (a == "shuffle") { player.shuffle(); ui.setLibrary(player.tracks()); }
        else if (a == "repeat")  player.cycleRepeat();
        else if (a == "calm")    visualizer.toggleCalmMode();
        else if (a == "track")   player.select(std::atoi(arg.c_str()), true);
        else if (a == "view")    showView(arg);
        else if (a == "rescan" && !scanJob.valid() && !cfg.musicFolder.empty()) startScan(cfg.musicFolder, false);
#ifndef __APPLE__
        else if (a == "popup")   { popupEnabled = !popupEnabled; if (!popupEnabled) nowPlaying.hide(); }
#endif
        else if (a == "minimize") platform::minimize(window.getNativeHandle());
        else if (a == "close")    window.close();
        else if (a == "maximize") {
            maximized = !maximized;
            if (maximized) { window.setSize(sf::Vector2u(work.size)); window.setPosition(work.position); }
            else {
                const sf::Vector2i s(1280, 800);
                window.setSize(sf::Vector2u(s));
                window.setPosition(work.position + (work.size - s) / 2);
            }
        }
        else if (a == "browse") {
            const std::string p = openFolderDialog();
            if (!p.empty()) { chosenFolder = p; ui.text("setup-path", p); ui.text("setup-error", ""); }
        }
        else if (a == "savefolder" && !chosenFolder.empty() && !scanJob.valid()) {
            ui.text("splash-stage", "Scanning your library");
            setPage(Page::Splash);
            startScan(chosenFolder, true);
        }
        else if (a == "change-folder") {
            player.stop();
            player.clear();
            ui.setLibrary({});
            chosenFolder = cfg.musicFolder;
            ui.text("setup-path", chosenFolder.empty() ? "no folder selected" : chosenFolder);
            setPage(Page::Setup);
        }
    };

    // Optional test hook for CI/remote GUI sessions: PONZU_TEST_AUTOPLAY=1 starts the first track.
    const char* autoplayEnv = std::getenv("PONZU_TEST_AUTOPLAY");
    bool testAutoplayPending = autoplayEnv && std::string(autoplayEnv) == "1";

    std::string shownPath;                       // track currently shown in the now-playing views
    std::shared_ptr<sf::Texture> popupArt;

    // ---- main loop ----------------------------------------------------------------------------------
    while (window.isOpen()) {
        while (auto event = window.pollEvent()) {
            ui.handle(*event);
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* r = event->getIf<sf::Event::Resized>()) {
                if (r->size.x > 0 && r->size.y > 0) {   // ignore minimise (0x0)
                    window.setView(sf::View(sf::FloatRect({0.f, 0.f}, sf::Vector2f(r->size))));
                    visualizer.resize(r->size.x, r->size.y);
                    ui.resize(r->size);
                }
            }
        }

        // Scan finished?
        if (scanJob.valid()) {
            if (page == Page::Splash) ui.text("splash-count", std::to_string(found.load()) + " songs found");
            if (scanJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                std::vector<Track> tracks = scanJob.get();
                if (tracks.empty() && initialLoad) {
                    chosenFolder = pendingFolder;
                    ui.text("setup-path", chosenFolder);
                    ui.text("setup-error", "No supported audio files (mp3, ogg, wav, flac) were found there.");
                    setPage(Page::Setup);
                } else {
                    if (initialLoad) {
                        ui.text("splash-stage", "Warming up the player");
                        cfg.musicFolder = pendingFolder;
                        config::save(cfg);
                        player.setTracks(std::move(tracks));
                        player.select(0, false);
                        shownPath.clear();
                        showGreeting();
                        setPage(Page::Main);
                    } else {
                        player.setTracks(std::move(tracks));   // keeps the playing track selected if it is still there
                    }
                    ui.setLibrary(player.tracks());
                    autoScan.restart();
                }
            }
        } else if (page == Page::Main && autoScan.getElapsedTime().asSeconds() >= kAutoScanSeconds) {
            autoScan.restart();
            startScan(cfg.musicFolder, false);
        }

        if (testAutoplayPending && player.hasSound()) { player.play(); testAutoplayPending = false; }
        player.tick();
        if (const sf::Sound* s = player.sound()) visualizer.update(*s, player.buffer());

        // ---- push state into the UI ----
        if (page == Page::Main) {
            const Track* t = player.currentTrack();
            if (t && t->fullPath != shownPath) {
                shownPath = t->fullPath;
                const auto art = library::readArt(*t);
                ui.setArt(art);
                popupArt = std::make_shared<sf::Texture>();
                if (art.empty() || !popupArt->loadFromMemory(art.data(), art.size())) popupArt.reset();
                for (const char* p : {"hero-", "bar-"}) {
                    ui.text(std::string(p) + "title", t->title);
                    ui.text(std::string(p) + "artist", t->artist);
                }
                ui.text("hero-album", t->album);
            } else if (!t && !shownPath.empty()) {
                shownPath.clear();
            }
            ui.setCurrent(player.current());
            ui.text("bar-time", fmtTime(player.position()) + " / " + fmtTime(player.duration()));
            ui.progress("seek-fill", player.duration() > 0 ? player.position() / player.duration() : 0.f);
            ui.text("btn-play", player.playing() ? "Pause" : "Play");
            ui.text("btn-repeat", player.repeat() == Player::Repeat::Off ? "Repeat off"
                                : player.repeat() == Player::Repeat::One ? "Repeat one" : "Repeat all");
            ui.text("btn-calm", visualizer.isCalmMode() ? "Visualizer: Calm" : "Visualizer: Portal");
#ifndef __APPLE__
            ui.text("btn-popup", popupEnabled ? "Popup: on" : "Popup: off");
#endif
            ui.text("sync-text", "Auto-sync in " +
                    std::to_string(std::max(0, static_cast<int>(kAutoScanSeconds - autoScan.getElapsedTime().asSeconds()))) + "s");
        }

#ifndef __APPLE__
        // Track-change pop-up (shown when a different track starts playing)
        if (const Track* t = player.currentTrack(); t && player.hasSound()) {
            if (player.playing()) {
                if (t->fullPath != lastNotifiedPath) {
                    lastNotifiedPath = t->fullPath;
                    if (popupAvailable && popupEnabled) nowPlaying.show(t->title, t->artist, t->album, popupArt);
                }
            } else if (!player.paused()) {
                lastNotifiedPath.clear();   // pressing play again shows it again
            }

            DiscordRPC::Presence p;   // only talks to Discord when something changes
            p.title = t->title; p.artist = t->artist; p.album = t->album; p.filePath = t->fullPath;
            p.positionSeconds = player.position();
            p.durationSeconds = player.duration();
            p.state = player.playing() ? DiscordRPC::PlayState::Playing
                    : player.paused()  ? DiscordRPC::PlayState::Paused : DiscordRPC::PlayState::Stopped;
            DiscordRPC::setPresence(p);
        } else {
            DiscordRPC::setPresence({});
        }
#endif

        ui.update();
        window.clear(sf::Color(10, 10, 12));
        visualizer.draw(window);   // backdrop; UI panels are translucent on top
        ui.render();
        window.display();
#ifndef __APPLE__
        nowPlaying.update();       // the pop-up is its own little window; then back to the main one
#endif
        window.setActive(true);
    }

#ifndef __APPLE__
    DiscordRPC::shutdown();
#endif
    return 0;
}
