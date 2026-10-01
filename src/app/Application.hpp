#pragma once

#include "playback/PlaybackEngine.hpp"
#include "render/VideoRenderer.hpp"

#include <SDL3/SDL.h>

#include <mutex>
#include <string>

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

private:
    void drawUI();
    void drawMenuBar();
    void drawVideoArea();
    void drawTransport();
    void drawStatsWindow();
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
    bool dragging_seek_ = false;
    float drag_position_ = 0.0f;
    const decode::DecodedFrame* uploaded_frame_ = nullptr;
    unsigned int video_texture_ = 0;
    std::string status_;

    std::mutex pending_mutex_;
    std::string pending_path_;
};

}  // namespace player
