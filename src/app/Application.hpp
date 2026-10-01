#pragma once

#include "playback/PlaybackEngine.hpp"
#include "render/VideoRenderer.hpp"
#include "utils/Log.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
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

private:
    void drawUI();
    void drawMenuBar();
    void drawVideoArea();
    void drawTransport();
    void drawStatsWindow();
    void drawLogWindow();
    void handleShortcuts();
    void openFileDialog();
    static void fileDialogCallback(void* userdata, const char* const* filelist, int filter);

    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    bool imgui_ready_ = false;
    int frame_count_ = 0;

    playback::PlaybackEngine engine_;
    render::VideoRenderer renderer_;
    render::VideoColorSettings color_;

    bool show_stats_ = true;
    bool show_log_ = false;
    bool log_autoscroll_ = true;
    uint64_t log_version_ = 0;
    std::vector<log::Entry> log_cache_;
    bool dragging_seek_ = false;
    float drag_position_ = 0.0f;
    uint64_t uploaded_serial_ = 0;
    unsigned int video_texture_ = 0;
    std::string status_;
    std::string screenshot_path_;
    int screenshot_every_ = 0;
    bool saveScreenshot();
    bool saveScreenshotTo(const std::string& path);

    std::mutex pending_mutex_;
    std::string pending_path_;
};

}  // namespace player
