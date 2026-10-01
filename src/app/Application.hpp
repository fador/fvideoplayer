#pragma once

#include <SDL3/SDL.h>

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

private:
    void drawUI();

    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;
    bool imgui_ready_ = false;
    int frame_count_ = 0;
};

}  // namespace player
