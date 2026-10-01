#include "app/Application.hpp"

#include <SDL3/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace player {
namespace {

std::string formatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return "--:--";
    }
    const int total = static_cast<int>(seconds);
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    char buffer[32] = {};
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    }
    return buffer;
}

const char* stateName(playback::State state) {
    switch (state) {
        case playback::State::Empty:
            return "no file";
        case playback::State::Paused:
            return "paused";
        case playback::State::Playing:
            return "playing";
        case playback::State::Ended:
            return "ended";
    }
    return "?";
}

const char* chromaName(int chroma_format) {
    switch (chroma_format) {
        case 0:
            return "4:0:0";
        case 1:
            return "4:2:0";
        case 2:
            return "4:2:2";
        case 3:
            return "4:4:4";
        default:
            return "?";
    }
}

const char* sliceName(int slice_type) {
    switch (slice_type) {
        case KVZDEC_SLICE_B:
            return "B";
        case KVZDEC_SLICE_P:
            return "P";
        case KVZDEC_SLICE_I:
            return "I";
        default:
            return "?";
    }
}

const char* hashResultName(int result) {
    switch (result) {
        case KVZDEC_HASH_NOT_PRESENT:
            return "n/a";
        case KVZDEC_HASH_MATCH:
            return "match";
        case KVZDEC_HASH_MISMATCH:
            return "MISMATCH";
        default:
            return "?";
    }
}

}  // namespace

Application::~Application() {
    shutdown();
}

bool Application::init(const std::string& title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    window_ = SDL_CreateWindow(title.c_str(), 1280, 720,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_context_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForOpenGL(window_, gl_context_)) {
        std::fprintf(stderr, "ImGui SDL3 backend init failed\n");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init("#version 330")) {
        std::fprintf(stderr, "ImGui OpenGL3 backend init failed\n");
        return false;
    }
    imgui_ready_ = true;

    std::string renderer_error;
    if (!renderer_.init(&renderer_error)) {
        std::fprintf(stderr, "video renderer init failed: %s\n", renderer_error.c_str());
        return false;
    }
    return true;
}

bool Application::openFile(const std::string& path) {
    std::string error;
    if (!engine_.open(path, error)) {
        status_ = "Failed to open: " + error;
        std::fprintf(stderr, "failed to open %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    uploaded_frame_ = nullptr;
    video_texture_ = 0;
    status_ = "Opened " + path;
    return true;
}

void Application::seek(double seconds) {
    engine_.seek(seconds);
}

void Application::play() {
    engine_.play();
}

int Application::run(int max_frames) {
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                       event.window.windowID == SDL_GetWindowID(window_)) {
                running = false;
            } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                openFile(event.drop.data);
            }
        }

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (!pending_path_.empty()) {
                const std::string path = pending_path_;
                pending_path_.clear();
                openFile(path);
            }
        }

        engine_.update();
        const decode::DecodedFrame* current = engine_.currentFrame();
        if (current && current != uploaded_frame_ && current->picture) {
            renderer_.upload(current->picture);
            uploaded_frame_ = current;
        }
        video_texture_ = renderer_.render(color_);
        if (!screenshot_path_.empty() && video_texture_ != 0) {
            saveScreenshot();
            running = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        drawUI();

        ImGui::Render();

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);

        ++frame_count_;
        if (max_frames > 0 && frame_count_ >= max_frames) {
            running = false;
        }
    }
    return 0;
}

void Application::drawUI() {
    handleShortcuts();
    drawMenuBar();
    drawVideoArea();
    drawStatsWindow();
}

void Application::drawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                openFileDialog();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) {
                engine_.close();
                uploaded_frame_ = nullptr;
                video_texture_ = 0;
                status_.clear();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Playback")) {
            const bool has_file = engine_.hasFile();
            if (ImGui::MenuItem(engine_.state() == playback::State::Playing ? "Pause" : "Play",
                                "Space", false, has_file)) {
                engine_.togglePlayPause();
            }
            if (ImGui::MenuItem("Stop", nullptr, false, has_file)) {
                engine_.stop();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Statistics", "S", &show_stats_);
            ImGui::Separator();
            ImGui::MenuItem("BT.709 color", nullptr, &color_.bt709);
            ImGui::MenuItem("Full range", nullptr, &color_.full_range);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Space: play/pause  Left/Right: seek 5s", nullptr, false, false);
            ImGui::MenuItem("F: fullscreen  S: statistics", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void Application::drawVideoArea() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float menu_height = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menu_height));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, viewport->Size.y - menu_height));
    ImGui::Begin("##video", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);

    const float transport_height = ImGui::GetFrameHeightWithSpacing() + 8.0f;
    ImVec2 available = ImGui::GetContentRegionAvail();
    ImVec2 video_size(available.x, std::max(1.0f, available.y - transport_height));

    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    if (video_texture_ != 0 && renderer_.hasFrame()) {
        const float video_w = static_cast<float>(renderer_.videoWidth());
        const float video_h = static_cast<float>(renderer_.videoHeight());
        const float scale = std::min(video_size.x / video_w, video_size.y / video_h);
        const ImVec2 draw_size(video_w * scale, video_h * scale);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + (video_size.x - draw_size.x) * 0.5f,
                                         cursor.y + (video_size.y - draw_size.y) * 0.5f));
        ImGui::Image(static_cast<ImTextureID>(video_texture_), draw_size);
        ImGui::SetCursorScreenPos(cursor);
    } else {
        const char* message = "No file open";
        if (engine_.hasFile()) {
            message = engine_.buffering() ? "Buffering..." : "No frame";
        }
        const ImVec2 text_size = ImGui::CalcTextSize(message);
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + (video_size.x - text_size.x) * 0.5f,
                                         cursor.y + (video_size.y - text_size.y) * 0.5f));
        ImGui::TextUnformatted(message);
        ImGui::SetCursorScreenPos(cursor);
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + video_size.y + 4.0f));
    drawTransport();
    ImGui::End();
}

void Application::drawTransport() {
    const bool has_file = engine_.hasFile();
    const double duration = engine_.duration();

    if (ImGui::Button(engine_.state() == playback::State::Playing ? "Pause" : "Play")) {
        engine_.togglePlayPause();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_file);
    if (ImGui::Button("Stop")) {
        engine_.stop();
    }
    ImGui::SameLine();

    const double current = engine_.position();
    if (!dragging_seek_) {
        drag_position_ = static_cast<float>(current);
    }
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 220.0f));
    if (ImGui::SliderFloat("##seek", &drag_position_, 0.0f,
                           static_cast<float>(duration > 0.0 ? duration : 1.0), "%.3f s")) {
    }
    if (ImGui::IsItemActivated()) {
        dragging_seek_ = true;
    }
    if (ImGui::IsItemDeactivated()) {
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            engine_.seek(static_cast<double>(drag_position_));
        }
        dragging_seek_ = false;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("%s / %s", formatTime(dragging_seek_ ? drag_position_ : current).c_str(),
                formatTime(duration).c_str());

    ImGui::Text("State: %s", stateName(engine_.state()));
    if (engine_.hasFile()) {
        const stats::Snapshot snapshot = engine_.stats().snapshot();
        ImGui::SameLine();
        ImGui::Text("| %.1f decode fps | %.1f display fps | buffer %.2f s | dropped %lld",
                    snapshot.decode_fps, snapshot.display_fps, snapshot.buffered_seconds,
                    static_cast<long long>(snapshot.frames_dropped));
    }
    if (!status_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", status_.c_str());
    }
}

void Application::drawStatsWindow() {
    if (!show_stats_) {
        return;
    }
    stats::Snapshot snapshot = engine_.stats().snapshot();

    ImGui::Begin("Statistics", &show_stats_);
    if (ImGui::CollapsingHeader("Stream", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Container: %s", snapshot.stream.container.c_str());
        ImGui::Text("Codec: %s %s @ %s", snapshot.stream.codec.c_str(),
                    snapshot.stream.profile.c_str(), snapshot.stream.level.c_str());
        ImGui::Text("Resolution: %ux%u", snapshot.stream.width, snapshot.stream.height);
        ImGui::Text("Bit depth: %d  Chroma: %s", snapshot.stream.bitdepth,
                    chromaName(snapshot.stream.chroma_format));
        ImGui::Text("Frame rate: %.3f fps", snapshot.stream.fps);
        ImGui::Text("Duration: %s", formatTime(snapshot.stream.duration).c_str());
        const demux::AudioTrackInfo& audio = engine_.audioInfo();
        if (audio.present) {
            const char fourcc[5] = {
                static_cast<char>((audio.codec_fourcc >> 24) & 0xFF),
                static_cast<char>((audio.codec_fourcc >> 16) & 0xFF),
                static_cast<char>((audio.codec_fourcc >> 8) & 0xFF),
                static_cast<char>(audio.codec_fourcc & 0xFF),
                0,
            };
            ImGui::Text("Audio: '%s' %u ch @ %u Hz (dummy sink, not decoded)", fourcc,
                        audio.channels, audio.sample_rate);
        } else {
            ImGui::TextUnformatted("Audio: none");
        }
    }

    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("State: %s", stateName(engine_.state()));
        ImGui::Text("Decode FPS: %.1f   Display FPS: %.1f", snapshot.decode_fps,
                    snapshot.display_fps);
        ImGui::Text("Frames: %lld decoded / %lld displayed / %lld dropped",
                    static_cast<long long>(snapshot.frames_decoded),
                    static_cast<long long>(snapshot.frames_displayed),
                    static_cast<long long>(snapshot.frames_dropped));
        ImGui::Text("Bitrate: %.2f Mbps", snapshot.bitrate_mbps);
        ImGui::Text("Buffer: %zu/%zu frames (%.2f s)", snapshot.queue_depth,
                    snapshot.queue_capacity, snapshot.buffered_seconds);
        ImGui::Text("Seeks: %lld (last %.1f ms)", static_cast<long long>(snapshot.seeks),
                    snapshot.last_seek_ms);
    }

    if (ImGui::CollapsingHeader("Decode performance", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Avg: %.2f ms   Min: %.2f ms   Max: %.2f ms   P95: %.2f ms",
                    snapshot.avg_decode_ms, snapshot.min_decode_ms, snapshot.max_decode_ms,
                    snapshot.p95_decode_ms);
        if (!snapshot.decode_history_ms.empty() &&
            ImPlot::BeginPlot("##decode_time", ImVec2(-1, 140))) {
            ImPlot::SetupAxes("frame", "ms");
            ImPlot::PlotLine("decode ms", snapshot.decode_history_ms.data(),
                             static_cast<int>(snapshot.decode_history_ms.size()));
            ImPlot::EndPlot();
        }
    }

    if (ImGui::CollapsingHeader("Current frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (snapshot.current.valid) {
            ImGui::Text("PTS: %.3f s   POC: %d", snapshot.current.pts / 1000.0,
                        snapshot.current.poc);
            ImGui::Text("QP: %d   TemporalId: %d   Slice: %s   NAL type: %d",
                        snapshot.current.qp, snapshot.current.temporal_id,
                        sliceName(snapshot.current.slice_type), snapshot.current.nal_unit_type);
            ImGui::Text("Decode: %.2f ms   Sample: %zu bytes", snapshot.current.decode_ms,
                        snapshot.current.sample_bytes);
            ImGui::Text("Hash check: %s", hashResultName(snapshot.current.hash_result));
        } else {
            ImGui::TextUnformatted("No frame decoded yet");
        }
    }

    if (ImGui::CollapsingHeader("Buffer level (s)")) {
        if (!snapshot.queue_history.empty() && ImPlot::BeginPlot("##buffer", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("sample", "seconds");
            ImPlot::PlotLine("buffered", snapshot.queue_history.data(),
                             static_cast<int>(snapshot.queue_history.size()));
            ImPlot::EndPlot();
        }
    }

    if (ImGui::CollapsingHeader("Bitrate (Mbps)")) {
        if (!snapshot.bitrate_history_mbps.empty() &&
            ImPlot::BeginPlot("##bitrate", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("sample", "Mbps");
            ImPlot::PlotLine("bitrate", snapshot.bitrate_history_mbps.data(),
                             static_cast<int>(snapshot.bitrate_history_mbps.size()));
            ImPlot::EndPlot();
        }
    }
    ImGui::End();
}

void Application::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false) && engine_.hasFile()) {
        engine_.togglePlayPause();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && engine_.hasFile()) {
        engine_.seek(engine_.position() - 5.0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && engine_.hasFile()) {
        engine_.seek(engine_.position() + 5.0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        show_stats_ = !show_stats_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
        const bool fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
        SDL_SetWindowFullscreen(window_, fullscreen ? 0 : SDL_WINDOW_FULLSCREEN);
    }
}

void Application::openFileDialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"Video files", "mp4;mkv;avi;hevc;h265;265;mov"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(&Application::fileDialogCallback, this, window_, filters, 2, nullptr,
                           false);
}

void Application::fileDialogCallback(void* userdata, const char* const* filelist, int filter) {
    (void)filter;
    Application* app = static_cast<Application*>(userdata);
    if (!app || !filelist || !filelist[0]) {
        return;
    }
    std::lock_guard<std::mutex> lock(app->pending_mutex_);
    app->pending_path_ = filelist[0];
}

bool Application::saveScreenshot() {
    std::vector<uint8_t> rgb;
    int width = 0;
    int height = 0;
    if (!renderer_.readback(rgb, width, height)) {
        return false;
    }
    FILE* file = std::fopen(screenshot_path_.c_str(), "wb");
    if (!file) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    std::fprintf(stderr, "screenshot written to %s (%dx%d)\n", screenshot_path_.c_str(), width,
                 height);
    screenshot_path_.clear();
    return true;
}

void Application::shutdown() {
    engine_.close();
    if (imgui_ready_) {
        renderer_.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    if (gl_context_) {
        SDL_GL_DestroyContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

}  // namespace player
