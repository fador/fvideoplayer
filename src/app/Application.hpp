#pragma once

#include "audio/SdlAudioSink.hpp"
#include "playback/PlaybackEngine.hpp"
#include "render/VideoRenderer.hpp"
#include "utils/Log.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace player {

class Application {
public:
    Application() = default;
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    bool init(const std::string& title = "HEVC Player");
    int run(int max_frames = -1);
    void shutdown();

    bool openFile(const std::string& path);
    void seek(double seconds);
    void play();
    void setScreenshotPath(const std::string& path) { screenshot_path_ = path; }
    void setScreenshotEvery(int frames) { screenshot_every_ = frames; }
    void setScreenshotWindow(bool window) { screenshot_window_ = window; }
    void selectSubtitle(int option) { activateSubtitle(option); }
    void selectAudio(int track) { engine_.selectAudioTrack(static_cast<size_t>(track)); }
    void setVolume(float volume) { engine_.setVolume(volume); }
    void setMuted(audio::Mute mute) { engine_.setMuted(mute); }

private:
    void drawUI();
    void drawMenuBar();
    void drawVideoArea();
    void drawTransport();
    void drawAudioControls();
    void drawStatsWindow();
    void drawLogWindow();
    void handleShortcuts();
    void openFileDialog();
    void openSubtitleDialog();
    static void fileDialogCallback(void* userdata, const char* const* filelist, int filter);
    static void subtitleDialogCallback(void* userdata, const char* const* filelist, int filter);

    struct SubtitleOption {
        std::string label;
        bool embedded = false;
        size_t track_index = 0;
        std::filesystem::path path;
        bool bitmap = false;
        bool loaded = false;
        subtitle::Track track;
    };

    void discoverSubtitles(const std::string& video_path);
    bool activateSubtitle(int option);
    void addExternalSubtitle(const std::string& path);
    void drawSubtitles(const ImVec2& video_pos, const ImVec2& video_size);
    // Font for a subtitle style, falling back to the CJK face.
    ImFont* subtitleFont(const std::string& family);

    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    bool imgui_ready_ = false;
    int frame_count_ = 0;
    // CJK fallback font and the per style fonts loaded on demand.
    ImFont* subtitle_font_ = nullptr;
    std::vector<std::pair<std::string, ImFont*>> subtitle_fonts_;

    playback::PlaybackEngine engine_;
    audio::SdlAudioSink audio_sink_;
    render::VideoRenderer renderer_;
    render::VideoColorSettings color_;

    bool show_stats_ = true;
    bool show_log_ = false;
    bool log_autoscroll_ = true;
    uint64_t log_version_ = 0;
    std::vector<log::Entry> log_cache_;
    std::vector<SubtitleOption> subtitle_options_;
    int active_subtitle_ = -1;
    bool dragging_seek_ = false;
    float drag_position_ = 0.0f;
    uint64_t uploaded_serial_ = 0;
    unsigned int video_texture_ = 0;
    std::string status_;
    std::string screenshot_path_;
    int screenshot_every_ = 0;
    bool screenshot_window_ = false;
    bool screenshot_pending_ = false;
    bool saveScreenshot();
    bool saveScreenshotTo(const std::string& path);
    bool readWindowPixels(std::vector<uint8_t>& rgb, int& width, int& height);

    std::mutex pending_mutex_;
    std::string pending_path_;
    bool pending_is_subtitle_ = false;
};

}  // namespace player
