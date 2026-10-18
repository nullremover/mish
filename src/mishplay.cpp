#include <SFML/Graphics.hpp>
#include <SFML/Audio.hpp>
#include <SFML/Graphics/Texture.hpp>
#include <memory>
#include "imgui.h"
#include "imgui-SFML.h"
#include "visualizer.h"
#include <optional>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/mpegfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/tfile.h>
#include <taglib/tfilestream.h>
#include <taglib/tbytevector.h>
#include <taglib/flacpicture.h> // Required for modern Windows dialogs
#include <platform.h>
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <random>
#include "pathutil.h"
#ifndef __APPLE__
#include "discord_config.h"
#include "discordrpc.h"
#include "nowplaying_popup.h"
#endif



namespace fs = std::filesystem;
const std::string CONFIG_FILE = "mish_config.txt";
const float AUTO_SCAN_INTERVAL = 120.0f; 

struct Track {
    std::string filename;
    std::string fullPath;
    
    // Extracted Metadata
    std::string title;
    std::string artist;
    std::string album;
    
    // Embedded Album Art
    std::shared_ptr<sf::Texture> albumArt = nullptr;

    // The exact filesystem path. Open files with THIS (Unicode-safe); fullPath is a UTF-8 copy
    // used for display, comparisons and Discord.
    fs::path path; 
};


void extractMetadata(Track& track) {
    std::cout << "\n[TagLib] Reading: " << track.filename << std::endl;

    // Defaults
    track.title = track.filename;
    track.artist = "Unknown Artist";
    track.album = "Unknown Album";
    track.albumArt = nullptr;


    // ----------------------------
    // Read normal metadata
    // ----------------------------

    TagLib::FileRef f(track.path.c_str()); // wide path on Windows: handles any character
	
	std::cout << "Opening path: " << track.fullPath << "\n";
	std::cout << "Exists: "
          << std::filesystem::exists(track.path)
          << "\n";

    if (!f.isNull() && f.tag()) {
        TagLib::Tag* tag = f.tag();

        if (!tag->title().isEmpty())
            track.title = tag->title().to8Bit(true);

        if (!tag->artist().isEmpty())
            track.artist = tag->artist().to8Bit(true);

        if (!tag->album().isEmpty())
            track.album = tag->album().to8Bit(true);


        std::cout
            << "   -> Title:  " << track.title
            << "\n   -> Artist: " << track.artist
            << "\n   -> Album:  " << track.album
            << "\n";
    }
    else {
        std::cout << "   -> Failed reading basic tags\n";
    }


    // ----------------------------
    // Extension
    // ----------------------------

    size_t dotPos = track.fullPath.find_last_of('.');

    if (dotPos == std::string::npos)
        return;


    std::string ext = track.fullPath.substr(dotPos + 1);

    std::transform(
        ext.begin(),
        ext.end(),
        ext.begin(),
        [](unsigned char c)
        {
            return std::tolower(c);
        }
    );


    // ----------------------------
    // MP3 album art
    // ----------------------------

    if (ext == "mp3") {

    if (f.isNull()) {
        std::cout << "   No TagLib file handle available\n";
        return;
    }

    TagLib::File* baseFile = f.file();

    auto* mpegFile =
        dynamic_cast<TagLib::MPEG::File*>(baseFile);


    if (!mpegFile) {
        std::cout << "   Dynamic cast to MPEG failed\n";
        return;
    }


    std::cout << "   MPEG cast OK!\n";


    auto* id3 = mpegFile->ID3v2Tag();

    if (!id3) {
        std::cout << "   No ID3v2 tag found\n";
        return;
    }


    auto frames = id3->frameList();

    std::cout
        << "   Total ID3 frames: "
        << frames.size()
        << "\n";


    for (auto frame : frames) {

        std::cout
            << "   Frame: "
            << frame->frameID().data()
            << "\n";


        if (frame->frameID() == TagLib::ByteVector("APIC")) {

            auto* picture =
                dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame);


            if (!picture) {
                std::cout << "   APIC cast failed\n";
                continue;
            }


            auto data = picture->picture();

            std::cout
                << "   MIME: "
                << picture->mimeType().to8Bit(true)
                << "\n";

            std::cout
                << "   Image bytes: "
                << data.size()
                << "\n";


            auto texture =
                std::make_shared<sf::Texture>();


            if(texture->loadFromMemory(data.data(), data.size()))
            {
                track.albumArt = texture;
                std::cout << "   SFML loaded album art!\n";
                return;
            }
        }
    }


        std::cout
            << "   No APIC frame found\n";
    }


    // ----------------------------
    // FLAC art
    // ----------------------------

		else if (ext == "flac") {

		if (f.isNull()) {
			std::cout << "   No TagLib file handle available\n";
			return;
		}

		TagLib::File* baseFile = f.file();

		auto* flacFile =
			dynamic_cast<TagLib::FLAC::File*>(baseFile);

		if (!flacFile) {
			std::cout << "   Dynamic cast to FLAC failed\n";
			return;
		}

		std::cout << "   FLAC cast OK!\n";


		auto pictures = flacFile->pictureList();

		std::cout << "   FLAC pictures found: "
				  << pictures.size()
				  << "\n";


		if (!pictures.isEmpty())
		{
			TagLib::FLAC::Picture* cover = nullptr;

			for (auto* pic : pictures)
			{
				if (pic->type() == TagLib::FLAC::Picture::FrontCover)
				{
					cover = pic;
					break;
				}
			}

			if (!cover)
				cover = pictures.front();


			auto data = cover->data();

			std::cout
				<< "   FLAC art bytes: "
				<< data.size()
				<< "\n";


			auto texture =
				std::make_shared<sf::Texture>();


			if(texture->loadFromMemory(
					data.data(),
					data.size()))
			{
				track.albumArt = texture;

				std::cout
					<< "   FLAC art loaded!\n";
			}
			else
			{
				std::cout
					<< "   FLAC art decode failed!\n";
			}
		}
	}
}

void saveConfig(const std::string& path) {
    std::ofstream cfg(CONFIG_FILE);
    if (cfg.is_open()) { cfg << path; cfg.close(); }
}

std::string loadConfig() {
    std::ifstream cfg(CONFIG_FILE);
    if (cfg.is_open()) {
        std::string path;
        std::getline(cfg, path);
        cfg.close();
        return path;
    }
    return "";
}

static std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Track> scanMusicFolder(const std::string& folderPath) {
    std::vector<Track> playlist;
    if (!fs::exists(folderPath) || !fs::is_directory(folderPath)) return playlist;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(folderPath, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;

            const std::string ext = lowerAscii(pathutil::toUtf8(entry.path().extension()));
            if (ext != ".mp3" && ext != ".ogg" && ext != ".wav" && ext != ".flac") continue;

            // 1. Add the basic track to the playlist
            Track track;
            track.path = entry.path();                                   // exact path, used to open the file
            track.fullPath = pathutil::toUtf8(entry.path());             // UTF-8 copy for display/compare/Discord
            track.filename = pathutil::toUtf8(entry.path().filename());
            playlist.push_back(std::move(track));

            // 2. Extract metadata using the last item in the vector
            extractMetadata(playlist.back());
        }
    } catch (const fs::filesystem_error& e) {
        std::cerr << "[Filesystem Error during scan]: " << e.what() << '\n';
    }

    // The recursive iterator returns files in an arbitrary order: sort so tracks are grouped by
    // folder and the order is the same on every scan (and on every OS).
    std::sort(playlist.begin(), playlist.end(), [](const Track& a, const Track& b) {
        return lowerAscii(a.fullPath) < lowerAscii(b.fullPath);
    });

    return playlist;
}
// ImGui's built-in font only has basic Latin, so characters like a star or a note show up as "?".
// Merge in system fonts that cover accents, Greek/Cyrillic, symbols (and Japanese if installed).
static void addMergedFont(const std::vector<std::string>& candidates, float size, const ImWchar* ranges)
{
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    for (const std::string& file : candidates) {
        std::error_code ec;
        if (!fs::exists(fs::path(file), ec)) continue;
        if (io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, ranges)) return;
    }
}

static void setupUiFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->AddFontDefault(); // keep the current look for plain text

    static const ImWchar textRanges[] = {
        0x0100, 0x024F, // Latin Extended
        0x0370, 0x03FF, // Greek
        0x0400, 0x052F, // Cyrillic
        0
    };
    static const ImWchar symbolRanges[] = {
        0x2000, 0x206F, // punctuation
        0x2190, 0x21FF, // arrows
        0x2300, 0x23FF, // technical
        0x2500, 0x25FF, // shapes
        0x2600, 0x27BF, // misc symbols + dingbats (stars, notes, hearts...)
        0x2B00, 0x2BFF, // more symbols and arrows
        0
    };

    std::vector<std::string> textFonts, symbolFonts, cjkFonts;
#ifdef _WIN32
    const char* winDir = std::getenv("WINDIR");
    const std::string fonts = std::string(winDir ? winDir : "C:\\Windows") + "\\Fonts\\";
    textFonts   = { fonts + "segoeui.ttf", fonts + "arial.ttf" };
    symbolFonts = { fonts + "seguisym.ttf" };
    cjkFonts    = { fonts + "YuGothM.ttc", fonts + "msgothic.ttc", fonts + "meiryo.ttc" };
#else
    textFonts = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf"
    };
    symbolFonts = textFonts;
    cjkFonts = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc"
    };
#endif

    // ImGui 1.92+ (dynamic fonts): the built-in font has an "implicit" size, so merged fonts
    // must use size 0 too (an explicit size trips an assert). Older ImGui needs a real size.
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
    const float mergedSize = 0.0f;
#else
    const float mergedSize = 13.0f;
#endif
    addMergedFont(textFonts,   mergedSize, textRanges);
    addMergedFont(symbolFonts, mergedSize, symbolRanges);
    addMergedFont(cjkFonts,    mergedSize, io.Fonts->GetGlyphRangesJapanese());

    ImGui::SFML::UpdateFontTexture();
}

int main() {
    sf::RenderWindow window(
		sf::VideoMode(sf::Vector2u(1024, 768)),
		"mish"
	);
    window.setFramerateLimit(60);
	
	// --- LOAD BUNDLED ICON DIRECTLY FROM EXECUTABLE MEMORY ---
    sf::Image icon;
    if (loadAppIcon(icon))
    {
        window.setIcon(icon);
    } else {
        std::cerr << "Warning: Could not load bundled PNG icon from resources\n";
    }

    // Center the window on the desktop monitor screen
    sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
	window.setPosition(sf::Vector2i(
		static_cast<int>((desktop.size.x - window.getSize().x) / 2),
		static_cast<int>((desktop.size.y - window.getSize().y) / 2)
	));

    if (!ImGui::SFML::Init(window, false)) {
        std::cerr << "Failed to initialize ImGui-SFML\n";
        return -1;
    }
    setupUiFonts(); // after Init(window, false): load our own fonts

    // Look for updates off github.com/nullremover/mish(no UI first, just open window then UI)


#ifndef __APPLE__
    // Discord Rich Presence (the Application ID is set in discord_config.h)
    DiscordRPC::init(kDiscordAppId);

    // Winamp-style "now playing" pop-up (bottom-right corner of the screen)
    NowPlayingPopup nowPlaying;
    const bool popupAvailable = nowPlaying.init();
    window.requestFocus(); // creating the pop-up window can briefly steal focus
    bool popupEnabled = true;
    std::string lastNotifiedPath;
#else
    window.requestFocus();
#endif

    sf::SoundBuffer buffer;
    std::optional<sf::Sound> sound; 
    Visualizer visualizer(1024, 768);
    
    bool setupDone = false;
    char pathBuffer[256] = "";
    std::string savedPath = loadConfig();
    std::vector<Track> tracks;
    int currentTrackIndex = -1;

    // True when the user pressed "stop" (so auto-advance must not kick in).
    // Declared once here so it persists across frames.
    bool userStopped = false;
	
	enum class RepeatMode {
		Off,
		Track,
		Playlist
	};

	RepeatMode repeatMode = RepeatMode::Off;

    if (!savedPath.empty() && fs::exists(savedPath) && fs::is_directory(savedPath)) {
        tracks = scanMusicFolder(savedPath);
        if (!tracks.empty()) {
            currentTrackIndex = 0;
            if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {
                sound.emplace(buffer); 
                setupDone = true;
            }
        }
    }

    sf::Clock deltaClock;
    sf::Clock autoScanClock;

    while (window.isOpen()) {
		while (auto event = window.pollEvent())
		{
			ImGui::SFML::ProcessEvent(window, *event);

			if (event->is<sf::Event::Closed>())
			{
				window.close();
			}
			else if (const auto* resized = event->getIf<sf::Event::Resized>())
			{
				// Ignore minimise (0x0). Otherwise make the view 1:1 with the window's
				// pixels (stops SFML stretching the scene) and resize the visualizer.
				if (resized->size.x > 0 && resized->size.y > 0)
				{
					window.setView(sf::View(sf::FloatRect(
						{0.f, 0.f},
						sf::Vector2f(resized->size))));
					visualizer.resize(resized->size.x, resized->size.y);
				}
			}
		}

        // Auto rescan
        if (setupDone && autoScanClock.getElapsedTime().asSeconds() >= AUTO_SCAN_INTERVAL) {
            autoScanClock.restart();
            std::string currentPlayingPath = (currentTrackIndex != -1) ? tracks[currentTrackIndex].fullPath : "";
            tracks = scanMusicFolder(savedPath);
            currentTrackIndex = -1;
            for (int i = 0; i < (int)tracks.size(); ++i) {
                if (tracks[i].fullPath == currentPlayingPath) { currentTrackIndex = i; break; }
            }
        }

        ImGui::SFML::Update(window, deltaClock.restart());

        if (sound.has_value()) {
            visualizer.update(*sound, buffer);
        }

		// Automatic song advancing
		if (sound.has_value() &&
			!userStopped &&
			sound->getStatus() == sf::Sound::Status::Stopped &&
			currentTrackIndex != -1)
		{

			if (repeatMode == RepeatMode::Track) {
				userStopped = false;
				sound->play();
			}

			else if (repeatMode == RepeatMode::Playlist) {

				currentTrackIndex++;

				if (currentTrackIndex >= (int)tracks.size()) {
					currentTrackIndex = 0;
				}

				if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {

					sound.emplace(buffer);
					userStopped = false;
					sound->play();
				}
			}

			else {

				// Normal mode
				if (currentTrackIndex < (int)tracks.size() - 1) {

					currentTrackIndex++;

					if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {

						sound.emplace(buffer);
						userStopped = false;
						sound->play();
					}

				}
				else {
					// Last track finished: mark the playlist as done so this
					// block doesn't keep re-running every frame.
					userStopped = true;
				}
			}
		}
		

        if (!setupDone) {
			ImGui::Begin("first-time setup", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize);
			ImGui::SetWindowSize(ImVec2(500, 300));
			ImGui::Text("mish Setup");
			ImGui::Separator();
			ImGui::Spacing();

			// 1. Display the selected path (Read-only so they don't have to type)
			if (pathBuffer[0] == '\0') {
				ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "selected Path: [no folder selected]");
			} else {
				ImGui::Text("selected path: %s", pathBuffer);
			}
			ImGui::Spacing();

			// 2. The Browse button that opens the native Windows Explorer window
			if (ImGui::Button("browse folder...")) {
				std::string chosenPath = openFolderDialog();
				if (!chosenPath.empty()) {
					// Copy the selected path into your pathBuffer securely
					strncpy(pathBuffer, chosenPath.c_str(), sizeof(pathBuffer) - 1);
					pathBuffer[sizeof(pathBuffer) - 1] = '\0';
				}
			}
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			// Disable the save button if they haven't picked a folder yet
			bool hasPath = (pathBuffer[0] != '\0');
			if (!hasPath) {
				ImGui::BeginDisabled();
			}

			// 3. Your original Save Path / Confirm button logic
			if (ImGui::Button("Save Path")) {
				std::string pathStr(pathBuffer);
				tracks = scanMusicFolder(pathStr);
				if (tracks.empty()) {
					ImGui::OpenPopup("Scan Error");
				} else {
					savedPath = pathStr;
					saveConfig(savedPath);
					currentTrackIndex = 0;
					if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {
						sound.emplace(buffer); 
						setupDone = true; 
						autoScanClock.restart();
					}
				}
			}

			if (!hasPath) {
				ImGui::EndDisabled();
			}

			// Your original popup modal code remains exactly where it was
			if (ImGui::BeginPopupModal("scan Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::Text("no files found!");
				if (ImGui::Button("OK")) { ImGui::CloseCurrentPopup(); }
				ImGui::EndPopup();
			}

			ImGui::End();
			} else {
				ImGui::SetNextWindowPos(
				ImVec2(10, 10),
				ImGuiCond_Always
			);
			ImGui::SetNextWindowSize(
				ImVec2(500, 300),
				ImGuiCond_Always
			);
			ImGui::Begin(
				"mish player"
			);
            if (currentTrackIndex >= 0 && currentTrackIndex < (int)tracks.size()) {
                ImGui::Text("now playing: %s", tracks[currentTrackIndex].filename.c_str());
            }
            float timeUntilScan = AUTO_SCAN_INTERVAL - autoScanClock.getElapsedTime().asSeconds();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Auto-sync in: %.0fs", timeUntilScan);
            ImGui::Separator();

            bool isPlaying = (sound.has_value() && sound->getStatus() == sf::Sound::Status::Playing);

            if (!isPlaying) {
                if (ImGui::Button("play") && currentTrackIndex != -1) {
                    userStopped = false;
                    if (sound.has_value()) sound->play();
                }
            } else {
                if (ImGui::Button("pause")) {
                    if (sound.has_value()) sound->pause();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("stop")) {
                userStopped = true;
                if (sound.has_value()) sound->stop();
            }
            ImGui::SameLine();
            if (ImGui::Button("next")) {
                if (!tracks.empty() && currentTrackIndex < (int)tracks.size() - 1) {
                    currentTrackIndex++;
                    if (sound.has_value()) sound->stop();
                    if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {
                        sound.emplace(buffer);
						sound->setPlayingOffset(sf::seconds(0));
						userStopped = false;
						sound->play();
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("prev")) {
                if (!tracks.empty() && currentTrackIndex > 0) {
                    currentTrackIndex--;
                    if (sound.has_value()) sound->stop();
                    if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {
						sound.emplace(buffer);
						sound->setPlayingOffset(sf::seconds(0));
						userStopped = false;
						sound->play();
                    }
                }
            }
			static std::mt19937 rng(std::random_device{}());
            std::shuffle(tracks.begin(), tracks.end(), rng);
            if (ImGui::Button(
                visualizer.isCalmMode() ? "Visualizer: Calm" : "Visualizer: Portal"
            ))
            {
                visualizer.toggleCalmMode();
            }
			ImGui::SameLine();
			if (ImGui::Button("Force Scan")) {
				std::string currentPlayingPath = "";

				if (currentTrackIndex >= 0 &&
					currentTrackIndex < (int)tracks.size()) {
					currentPlayingPath = tracks[currentTrackIndex].fullPath;
				}

				tracks = scanMusicFolder(savedPath);

				currentTrackIndex = -1;

				// Try to keep the currently playing song selected
				for (int i = 0; i < (int)tracks.size(); i++) {
					if (tracks[i].fullPath == currentPlayingPath) {
						currentTrackIndex = i;
						break;
					}
				}

				autoScanClock.restart();

				std::cout << "Force scan complete! Found "
						  << tracks.size()
						  << " tracks.\n";
			}

			const char* repeatText = "";

			switch (repeatMode) {
				case RepeatMode::Off:
					repeatText = "Repeat Off";
					break;

				case RepeatMode::Track:
					repeatText = "Repeat One";
					break;

				case RepeatMode::Playlist:
					repeatText = "Repeat All";
					break;
			}

			if (ImGui::Button(repeatText)) {

				if (repeatMode == RepeatMode::Off) {
					repeatMode = RepeatMode::Track;
				}
				else if (repeatMode == RepeatMode::Track) {
					repeatMode = RepeatMode::Playlist;
				}
				else {
					repeatMode = RepeatMode::Off;
				}
			}
			ImGui::SameLine();
#ifndef __APPLE__
			if (ImGui::Checkbox("popup", &popupEnabled)) {
				if (!popupEnabled) nowPlaying.hide();
			}
#endif
			// --- SEEK BAR ---
			if (sound.has_value() && currentTrackIndex >= 0) {

				float duration = buffer.getDuration().asSeconds();
				float current = sound->getPlayingOffset().asSeconds();

				if (duration > 0) {

					ImGui::Text(
						"%02d:%02d / %02d:%02d",
						(int)(current / 60),
						(int)current % 60,
						(int)(duration / 60),
						(int)duration % 60
					);

					float seek = current;

					if (ImGui::SliderFloat(
						"##seek",
						&seek,
						0.0f,
						duration,
						""
					)) {
						sound->setPlayingOffset(sf::seconds(seek));
					}
				}
			}
            ImGui::Spacing();
            ImGui::Separator();
			
            
            ImGui::BeginChild("TrackList", ImVec2(0, 150), true);
            for (int i = 0; i < (int)tracks.size(); ++i) {
                bool isSelected = (i == currentTrackIndex);
                if (ImGui::Selectable(tracks[i].filename.c_str(), isSelected)) {
                    currentTrackIndex = i;
                    if (sound.has_value()) sound->stop();
                    if (buffer.loadFromFile(tracks[currentTrackIndex].path)) {
                        sound.emplace(buffer); 
                        userStopped = false;
                        sound->play();
                    }
                }
            }
            ImGui::EndChild();

            if (ImGui::Button("change music folder")) {
                if (sound.has_value()) sound->stop();
                setupDone = false; 
                tracks.clear();
                currentTrackIndex = -1;
                sound.reset(); 
                snprintf(pathBuffer, sizeof(pathBuffer), "%s", savedPath.c_str());
            }
            ImGui::End();
        }

		// Guard against currentTrackIndex being -1 (e.g. after a rescan that
		// didn't find the playing file) or out of range.
		if (setupDone && !tracks.empty() &&
			currentTrackIndex >= 0 && currentTrackIndex < (int)tracks.size()) {
		// Access the current track metadata
		const Track& currentTrack = tracks[currentTrackIndex];

		// Set a sleek style for our Now Playing Card
		ImGui::SetNextWindowPos(
			ImVec2(10, 320),
			ImGuiCond_Always
		);

		ImGui::SetNextWindowSize(
			ImVec2(350, 250),
			ImGuiCond_Always
		);

		ImGui::Begin("song info");		
		// --- ALBUM ART CONTAINER ---
		if (currentTrack.albumArt) {
			// Display the loaded dynamic texture
			// (ImTextureID conversion varies slightly depending on your ImGui-SFML backend)
			ImGui::Image(*currentTrack.albumArt, sf::Vector2f(150.f, 150.f)); //  Correct!
		} else {
			// Draw a nice fallback placeholder if there's no album cover
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			ImVec2 p = ImGui::GetCursorScreenPos();
			
			// Draw a dark gray rectangle
			drawList->AddRectFilled(p, ImVec2(p.x + 150, p.y + 150), IM_COL32(45, 45, 45, 255), 8.0f);
			// Draw a music note icon representation/text in the center
			ImGui::SetCursorScreenPos(ImVec2(p.x + 40, p.y + 65));
			ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[ NO ART ]");
			
			// Restore cursor position for next elements
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 150));
		}
		
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();

		// --- TEXT METADATA ---
		// Make the title big and highlighted
		ImGui::TextColored(ImVec4(0.0f, 0.9f, 1.0f, 1.0f), "%s", currentTrack.title.c_str());
		
		// Subtext for Artist & Album
		ImGui::Text("Artist: %s", currentTrack.artist.c_str());
		ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "album:  %s", currentTrack.album.c_str());

		ImGui::End();
	}

#ifndef __APPLE__
        // --- Track-change pop-up: shown when a different track starts playing ---
        if (setupDone && sound.has_value() &&
            currentTrackIndex >= 0 && currentTrackIndex < (int)tracks.size())
        {
            const Track& t = tracks[currentTrackIndex];
            const auto status = sound->getStatus();

            if (status == sf::Sound::Status::Playing) {
                if (t.fullPath != lastNotifiedPath) {
                    lastNotifiedPath = t.fullPath;
                    if (popupAvailable && popupEnabled)
                        nowPlaying.show(t.title, t.artist, t.album, t.albumArt);
                }
            } else if (status == sf::Sound::Status::Stopped) {
                lastNotifiedPath.clear(); // pressing play again shows it again
            }
        }
#endif

#ifndef __APPLE__
        // --- Discord Rich Presence (only talks to Discord when something changes) ---
        if (setupDone && sound.has_value() &&
            currentTrackIndex >= 0 && currentTrackIndex < (int)tracks.size())
        {
            const Track& t = tracks[currentTrackIndex];

            DiscordRPC::Presence presence;
            presence.title  = t.title;
            presence.artist = t.artist;
            presence.album  = t.album;
            presence.filePath = t.fullPath;
            presence.positionSeconds = sound->getPlayingOffset().asSeconds();
            presence.durationSeconds = buffer.getDuration().asSeconds();

            switch (sound->getStatus()) {
                case sf::Sound::Status::Playing: presence.state = DiscordRPC::PlayState::Playing; break;
                case sf::Sound::Status::Paused:  presence.state = DiscordRPC::PlayState::Paused;  break;
                default:                         presence.state = DiscordRPC::PlayState::Stopped; break;
            }
            DiscordRPC::setPresence(presence);
        }
        else
        {
            DiscordRPC::setPresence({}); // Stopped -> clears the presence
        }
#endif

        window.clear(sf::Color(10, 10, 12));
        
        visualizer.draw(window);
        
        ImGui::SFML::Render(window);
        window.display();

#ifndef __APPLE__
        // Draw the pop-up (it's its own little window), then go back to the main window
        nowPlaying.update();
#endif
        window.setActive(true);
    }

#ifndef __APPLE__
    DiscordRPC::shutdown();
#endif
    ImGui::SFML::Shutdown();
    return 0;
}