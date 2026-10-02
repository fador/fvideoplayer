#include "app/Application.hpp"

#include <SDL3/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

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

    // Subtitles are often CJK, which the built-in font cannot draw. ImGui
    // loads glyphs on demand, so any font with the coverage works; take the
    // first one the system has.
    for (const char* path : {"C:/Windows/Fonts/YuGothM.ttc", "C:/Windows/Fonts/msgothic.ttc",
                             "C:/Windows/Fonts/meiryo.ttc", "C:/Windows/Fonts/msyh.ttc",
                             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                             "/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc"}) {
        if (!std::filesystem::exists(path)) {
            continue;
        }
        subtitle_font_ = io.Fonts->AddFontFromFileTTF(path, 32.0f);
        if (subtitle_font_) {
            break;
        }
    }
    if (!subtitle_font_) {
        log::add(log::Level::Warning, "No CJK subtitle font found, Japanese text will not draw");
    }

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
    engine_.setAudioSink(&audio_sink_);

    log::addf(log::Level::Info, "HEVC Player started (kvzdec %s)",
              kvzdec_get_version_string());
    const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    log::addf(log::Level::Info, "OpenGL: %s (%s)", gl_version ? gl_version : "?",
              gl_renderer ? gl_renderer : "?");
    return true;
}

bool Application::openFile(const std::string& path) {
    std::string error;
    if (!engine_.open(path, error)) {
        status_ = "Failed to open: " + error;
        log::addf(log::Level::Error, "Failed to open %s: %s", path.c_str(), error.c_str());
        return false;
    }
    uploaded_serial_ = 0;
    video_texture_ = 0;
    renderer_.setStreamBitDepth(engine_.streamBitDepth());
    status_ = "Opened " + path;
    log::addf(log::Level::Info, "Opened %s", path.c_str());
    discoverSubtitles(path);
    engine_.play();
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
            std::string pending;
            bool is_subtitle = false;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                if (!pending_path_.empty()) {
                    pending = pending_path_;
                    is_subtitle = pending_is_subtitle_;
                    pending_path_.clear();
                }
            }
            if (!pending.empty()) {
                if (is_subtitle) {
                    addExternalSubtitle(pending);
                } else {
                    openFile(pending);
                }
            }
        }

        engine_.update();
        const decode::DecodedFrame* current = engine_.currentFrame();
        if (current && current->picture && engine_.currentSerial() != uploaded_serial_) {
            renderer_.upload(current->picture);
            uploaded_serial_ = engine_.currentSerial();
        }
        video_texture_ = renderer_.render(color_);
        if (!screenshot_path_.empty() && video_texture_ != 0) {
            if (screenshot_every_ > 0) {
                if (frame_count_ % screenshot_every_ == 0) {
                    saveScreenshotTo(screenshot_path_ + "-" +
                                     std::to_string(frame_count_) + ".ppm");
                }
            } else {
                screenshot_pending_ = true;
            }
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

        if (screenshot_pending_) {
            saveScreenshot();
            screenshot_pending_ = false;
            running = false;
        }

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
    drawLogWindow();
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
                uploaded_serial_ = 0;
                video_texture_ = 0;
                subtitle_options_.clear();
                active_subtitle_ = -1;
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
        if (ImGui::BeginMenu("Subtitles")) {
            const bool has_file = engine_.hasFile();
            if (ImGui::MenuItem("None", nullptr, active_subtitle_ < 0, has_file)) {
                activateSubtitle(-1);
            }
            ImGui::Separator();
            for (size_t i = 0; i < subtitle_options_.size(); ++i) {
                const SubtitleOption& option = subtitle_options_[i];
                const bool selected = active_subtitle_ == static_cast<int>(i);
                if (ImGui::MenuItem(option.label.c_str(), nullptr, selected, !option.bitmap)) {
                    activateSubtitle(static_cast<int>(i));
                }
            }
            if (subtitle_options_.empty()) {
                ImGui::MenuItem("(no subtitle tracks found)", nullptr, false, false);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load from file...", nullptr, false, has_file)) {
                openSubtitleDialog();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Audio")) {
            const bool has_file = engine_.hasFile();
            const bool muted = engine_.muted() == audio::Mute::On;
            if (ImGui::MenuItem("Mute", "M", muted, has_file)) {
                engine_.setMuted(muted ? audio::Mute::Off : audio::Mute::On);
            }
            int volume_percent = static_cast<int>(engine_.volume() * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderInt("Volume", &volume_percent, 0, 100, "%d%%",
                                 ImGuiSliderFlags_AlwaysClamp)) {
                engine_.setVolume(static_cast<float>(volume_percent) / 100.0f);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("None", nullptr, !engine_.audioEnabled(), has_file)) {
                engine_.selectAudioTrack(playback::PlaybackEngine::kNoAudioTrack);
            }
            ImGui::Separator();
            const std::vector<demux::AudioTrackInfo>& tracks = engine_.audioTracks();
            for (const demux::AudioTrackInfo& track : tracks) {
                std::string label = track.name.empty() ? "Track " + std::to_string(track.index + 1)
                                                       : track.name;
                if (!track.language.empty()) {
                    label += " [" + track.language + "]";
                }
                label += " (" + track.codec + ")";
                const bool selected =
                    engine_.audioEnabled() && engine_.selectedAudioTrack() == track.index;
                if (ImGui::MenuItem(label.c_str(), nullptr, selected, has_file)) {
                    engine_.selectAudioTrack(track.index);
                }
            }
            if (tracks.empty()) {
                ImGui::MenuItem("(no audio tracks found)", nullptr, false, false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Statistics", "S", &show_stats_);
            ImGui::MenuItem("Log", "L", &show_log_);
            ImGui::Separator();
            ImGui::MenuItem("BT.709 color", nullptr, &color_.bt709);
            ImGui::MenuItem("Full range", nullptr, &color_.full_range);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::MenuItem("Space: play/pause  Left/Right: seek 5s", nullptr, false, false);
            ImGui::MenuItem("Up/Down: volume  M: mute", nullptr, false, false);
            ImGui::MenuItem("F: fullscreen  S: statistics  L: log", nullptr, false, false);
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
        const ImVec2 draw_pos(cursor.x + (video_size.x - draw_size.x) * 0.5f,
                              cursor.y + (video_size.y - draw_size.y) * 0.5f);
        ImGui::SetCursorScreenPos(draw_pos);
        ImGui::Image(static_cast<ImTextureID>(video_texture_), draw_size);
        ImGui::SetCursorScreenPos(cursor);
        drawSubtitles(draw_pos, draw_size);
    } else {
        const char* message = "No file open";
        if (engine_.hasFile()) {
            if (engine_.videoInfo().codec == player::demux::Codec::Unknown) {
                message = "Audio only";
            } else {
                message = engine_.buffering() ? "Buffering..." : "No frame";
            }
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
    drawAudioControls();
    ImGui::SameLine();

    const double current = engine_.position();
    if (!dragging_seek_) {
        drag_position_ = static_cast<float>(current);
    }
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 260.0f));
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

void Application::drawAudioControls() {
    const bool muted = engine_.muted() == audio::Mute::On;
    if (ImGui::Button(muted ? "Unmute" : "Mute", ImVec2(64.0f, 0.0f))) {
        engine_.setMuted(muted ? audio::Mute::Off : audio::Mute::On);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("M toggles mute, Up/Down adjust volume");
    }

    ImGui::SameLine();
    int volume_percent = static_cast<int>(engine_.volume() * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##volume", &volume_percent, 0, 100, "vol %d%%",
                         ImGuiSliderFlags_AlwaysClamp)) {
        engine_.setVolume(static_cast<float>(volume_percent) / 100.0f);
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
            ImGui::Text("Audio: '%s' %u ch @ %u Hz (%s)", fourcc, audio.channels, audio.sample_rate,
                        engine_.audioCodecName());
            ImGui::Text("Audio PCM: %llu frames played, %llu dropped",
                        static_cast<unsigned long long>(engine_.audioFrames()),
                        static_cast<unsigned long long>(engine_.audioFramesDropped()));
            ImGui::Text("Audio buffer: %lld ms  sink: %lld ms  A/V drift: %lld ms  underruns: %lld",
                        static_cast<long long>(snapshot.audio_buffer_ms),
                        static_cast<long long>(snapshot.audio_sink_ms),
                        static_cast<long long>(snapshot.audio_drift_ms),
                        static_cast<long long>(snapshot.audio_underruns));
        } else {
            ImGui::TextUnformatted("Audio: none");
        }
        if (active_subtitle_ >= 0 &&
            active_subtitle_ < static_cast<int>(subtitle_options_.size())) {
            const SubtitleOption& option = subtitle_options_[static_cast<size_t>(active_subtitle_)];
            ImGui::Text("Subtitles: %s (%zu cues)", option.label.c_str(),
                        option.track.cues.size());
        } else {
            ImGui::TextUnformatted("Subtitles: none");
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

void Application::drawLogWindow() {
    if (!show_log_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log", &show_log_);

    if (ImGui::Button("Clear")) {
        log::clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &log_autoscroll_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu messages", log_cache_.size());
    ImGui::Separator();

    const uint64_t version = log::version();
    if (version != log_version_) {
        log_cache_ = log::snapshot();
        log_version_ = version;
    }

    ImGui::BeginChild("##log_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const log::Entry& entry : log_cache_) {
        ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f);
        switch (entry.level) {
            case log::Level::Debug:
                color = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
                break;
            case log::Level::Info:
                color = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                break;
            case log::Level::Warning:
                color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                break;
            case log::Level::Error:
                color = ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                break;
        }
        ImGui::TextDisabled("%7.2f", entry.time_seconds);
        ImGui::SameLine();
        ImGui::TextColored(color, "[%s] %s", log::levelName(entry.level), entry.text.c_str());
    }
    if (log_autoscroll_) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
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
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
        engine_.setVolume(engine_.volume() + 0.05f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
        engine_.setVolume(engine_.volume() - 0.05f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        engine_.setMuted(engine_.muted() == audio::Mute::On ? audio::Mute::Off : audio::Mute::On);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        show_stats_ = !show_stats_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
        show_log_ = !show_log_;
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
    app->pending_is_subtitle_ = false;
}

void Application::openSubtitleDialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"Subtitle files", "srt;ass;ssa;vtt;sub"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(&Application::subtitleDialogCallback, this, window_, filters, 2, nullptr,
                           false);
}

void Application::subtitleDialogCallback(void* userdata, const char* const* filelist, int filter) {
    (void)filter;
    Application* app = static_cast<Application*>(userdata);
    if (!app || !filelist || !filelist[0]) {
        return;
    }
    std::lock_guard<std::mutex> lock(app->pending_mutex_);
    app->pending_path_ = filelist[0];
    app->pending_is_subtitle_ = true;
}

namespace {

std::string toLowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isSubtitleExtension(const std::string& extension) {
    const std::string lower = toLowerCopy(extension);
    return lower == ".srt" || lower == ".ass" || lower == ".ssa" || lower == ".vtt" ||
           lower == ".sub";
}

}  // namespace

void Application::discoverSubtitles(const std::string& video_path) {
    subtitle_options_.clear();
    active_subtitle_ = -1;

    for (const demux::SubtitleTrackInfo& info : engine_.subtitleTracks()) {
        SubtitleOption option;
        option.embedded = true;
        option.track_index = info.index;
        option.bitmap = info.bitmap;
        std::string codec = info.codec;
        const size_t slash = codec.find('/');
        if (slash != std::string::npos) {
            codec = codec.substr(slash + 1);
        }
        option.label = info.name.empty() ? "Track " + std::to_string(info.index + 1) : info.name;
        if (!info.language.empty()) {
            option.label += " [" + info.language + "]";
        }
        option.label += " (" + codec + ")";
        if (info.bitmap) {
            option.label += " - bitmap, unsupported";
        }
        subtitle_options_.push_back(std::move(option));
    }

    // External subtitle files matching the video name, e.g. video.srt,
    // video.eng.srt or video.English.ass.
    const std::filesystem::path path(video_path);
    const std::filesystem::path directory = path.parent_path();
    const std::string stem = path.stem().string();
    if (!directory.empty() && !stem.empty()) {
        std::error_code error_code;
        std::vector<std::filesystem::path> found;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(directory, error_code)) {
            if (!entry.is_regular_file(error_code)) {
                continue;
            }
            if (!isSubtitleExtension(entry.path().extension().string())) {
                continue;
            }
            const std::string entry_stem = entry.path().stem().string();
            if (entry_stem.size() < stem.size()) {
                continue;
            }
            if (toLowerCopy(entry_stem.substr(0, stem.size())) != toLowerCopy(stem)) {
                continue;
            }
            if (entry_stem.size() != stem.size() && entry_stem[stem.size()] != '.') {
                continue;
            }
            found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        for (const std::filesystem::path& file : found) {
            SubtitleOption option;
            option.embedded = false;
            option.path = file;
            option.label = file.filename().string() + " (external)";
            subtitle_options_.push_back(std::move(option));
        }
    }

    log::addf(log::Level::Info, "Subtitles: %zu option(s) found",
              static_cast<int>(subtitle_options_.size()));

    int select = -1;
    for (size_t i = 0; i < subtitle_options_.size(); ++i) {
        if (!subtitle_options_[i].bitmap) {
            select = static_cast<int>(i);
            break;
        }
    }
    if (select >= 0) {
        activateSubtitle(select);
    }
}

bool Application::activateSubtitle(int option) {
    if (option < 0) {
        active_subtitle_ = -1;
        return true;
    }
    if (option >= static_cast<int>(subtitle_options_.size())) {
        return false;
    }
    SubtitleOption& selected = subtitle_options_[static_cast<size_t>(option)];
    if (selected.bitmap) {
        log::addf(log::Level::Warning, "Bitmap subtitles are not supported: %s",
                  selected.label.c_str());
        return false;
    }

    if (!selected.loaded) {
        std::string error;
        if (selected.embedded) {
            std::vector<subtitle::SubtitleBlock> blocks;
            if (!engine_.loadSubtitleTrack(selected.track_index, blocks, error)) {
                log::addf(log::Level::Error, "Failed to read subtitle track: %s", error.c_str());
                return false;
            }
            const std::vector<demux::SubtitleTrackInfo>& infos = engine_.subtitleTracks();
            std::string codec_private;
            std::string codec;
            if (selected.track_index < infos.size()) {
                codec = infos[selected.track_index].codec;
                codec_private.assign(infos[selected.track_index].codec_private.begin(),
                                     infos[selected.track_index].codec_private.end());
            }
            if (!subtitle::parseEmbeddedTrack(codec, codec_private, blocks, selected.track.cues,
                                              &error, &selected.track)) {
                log::addf(log::Level::Error, "Failed to parse subtitle track: %s", error.c_str());
                return false;
            }
            selected.track.embedded = true;
        } else if (!subtitle::loadSubtitleFile(selected.path, selected.track, &error)) {
            log::addf(log::Level::Error, "Failed to load %s: %s", selected.path.string().c_str(),
                      error.c_str());
            return false;
        }
        selected.loaded = true;
    }

    active_subtitle_ = option;
    log::addf(log::Level::Info, "Subtitle track: %s (%zu cues)", selected.label.c_str(),
              selected.track.cues.size());
    return true;
}

void Application::addExternalSubtitle(const std::string& path) {
    if (!engine_.hasFile()) {
        return;
    }
    SubtitleOption option;
    option.embedded = false;
    option.path = path;
    option.label = std::filesystem::path(path).filename().string() + " (external)";
    subtitle_options_.push_back(std::move(option));
    activateSubtitle(static_cast<int>(subtitle_options_.size()) - 1);
}

void Application::drawSubtitles(const ImVec2& video_pos, const ImVec2& video_size) {
    if (active_subtitle_ < 0 || active_subtitle_ >= static_cast<int>(subtitle_options_.size())) {
        return;
    }
    const subtitle::Track& track = subtitle_options_[static_cast<size_t>(active_subtitle_)].track;
    if (track.cues.empty()) {
        return;
    }
    const double now = engine_.position();
    const std::vector<const subtitle::Cue*> active = subtitle::findCues(track.cues, now);
    if (active.empty()) {
        return;
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImFont* font = subtitle_font_ ? subtitle_font_ : ImGui::GetFont();
    // ASS positions and font sizes are in script coordinates; scale them to the
    // video rectangle.
    const float scale = track.play_res_y > 0 ? video_size.y / static_cast<float>(track.play_res_y)
                                             : video_size.y / 1080.0f;
    const float max_width = video_size.x * 0.94f;
    const float outline = 2.0f;

    // One run of text with the colour it is drawn in.
    struct Run {
        std::string text;
        uint32_t color = 0xFFFFFF;
    };

    draw_list->PushClipRect(video_pos, ImVec2(video_pos.x + video_size.x,
                                              video_pos.y + video_size.y), true);
    for (const subtitle::Cue* cue : active) {
        const float font_size =
            std::clamp((cue->font_size > 0.0f ? cue->font_size : 42.0f) * scale, 10.0f, 96.0f);
        const float line_height = font_size * 1.2f;

        // Build the lines, colouring karaoke runs by whether they are sung yet.
        std::vector<std::vector<Run>> lines(1);
        const auto append = [&](const std::string& text, uint32_t color) {
            size_t start = 0;
            while (start <= text.size()) {
                const size_t newline = text.find('\n', start);
                const std::string piece = text.substr(
                    start, newline == std::string::npos ? std::string::npos : newline - start);
                if (!piece.empty()) {
                    lines.back().push_back({piece, color});
                }
                if (newline == std::string::npos) {
                    break;
                }
                lines.emplace_back();
                start = newline + 1;
            }
        };
        if (!cue->karaoke.empty()) {
            for (const subtitle::KaraokeSyllable& syllable : cue->karaoke) {
                const bool sung = now >= cue->start + syllable.start;
                append(syllable.text, sung ? cue->primary_color : cue->secondary_color);
            }
        } else {
            append(cue->text, cue->primary_color);
        }

        // Wrap each line on spaces, keeping the run colours.
        std::vector<std::vector<Run>> wrapped;
        for (const std::vector<Run>& line : lines) {
            std::vector<Run> current;
            float width = 0.0f;
            for (const Run& run : line) {
                std::istringstream words(run.text);
                std::string word;
                bool first_word = true;
                while (words >> word) {
                    const float word_width =
                        font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, word.c_str()).x;
                    const float space = first_word ? 0.0f : font_size * 0.3f;
                    if (width + space + word_width > max_width && !current.empty()) {
                        wrapped.push_back(current);
                        current.clear();
                        width = 0.0f;
                        first_word = true;
                    }
                    if (!first_word) {
                        current.push_back({" ", run.color});
                        width += space;
                    }
                    current.push_back({word, run.color});
                    width += word_width;
                    first_word = false;
                }
            }
            wrapped.push_back(current);
        }
        while (!wrapped.empty() && wrapped.back().empty()) {
            wrapped.pop_back();
        }
        if (wrapped.empty()) {
            continue;
        }

        // Anchor the block. Positioned lines use their own coordinates with the
        // alignment from \an; the rest sit at the bottom of the frame.
        float block_width = 0.0f;
        for (const std::vector<Run>& line : wrapped) {
            float width = 0.0f;
            for (const Run& run : line) {
                width += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, run.text.c_str()).x;
            }
            block_width = std::max(block_width, width);
        }
        const int alignment = cue->alignment >= 1 && cue->alignment <= 9 ? cue->alignment : 2;
        const float anchor_x =
            cue->has_position ? video_pos.x + cue->pos_x * scale : video_pos.x + video_size.x * 0.5f;
        float y = 0.0f;
        if (cue->has_position) {
            const float height = line_height * static_cast<float>(wrapped.size());
            const int vertical = (alignment - 1) / 3;  // 0 bottom, 1 middle, 2 top
            if (vertical == 2) {
                y = video_pos.y + cue->pos_y * scale;
            } else if (vertical == 1) {
                y = video_pos.y + cue->pos_y * scale - height * 0.5f;
            } else {
                y = video_pos.y + cue->pos_y * scale - height;
            }
        } else {
            const float bottom_margin = video_size.y * 0.04f;
            y = video_pos.y + video_size.y - bottom_margin -
                line_height * static_cast<float>(wrapped.size());
        }

        for (const std::vector<Run>& line : wrapped) {
            float width = 0.0f;
            for (const Run& run : line) {
                width += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, run.text.c_str()).x;
            }
            const int horizontal = (alignment - 1) % 3;  // 0 left, 1 center, 2 right
            float x = anchor_x;
            if (horizontal == 1) {
                x -= width * 0.5f;
            } else if (horizontal == 2) {
                x -= width;
            }
            for (const Run& run : line) {
                const ImVec2 size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, run.text.c_str());
                const ImU32 outline_color = IM_COL32((cue->outline_color >> 16) & 0xFF,
                                                     (cue->outline_color >> 8) & 0xFF,
                                                     cue->outline_color & 0xFF, 220);
                for (float dx = -outline; dx <= outline; dx += outline) {
                    for (float dy = -outline; dy <= outline; dy += outline) {
                        if (dx == 0.0f && dy == 0.0f) {
                            continue;
                        }
                        draw_list->AddText(font, font_size, ImVec2(x + dx, y + dy),
                                           outline_color, run.text.c_str());
                    }
                }
                const ImU32 color = IM_COL32((run.color >> 16) & 0xFF, (run.color >> 8) & 0xFF,
                                             run.color & 0xFF, 255);
                draw_list->AddText(font, font_size, ImVec2(x, y), color, run.text.c_str());
                x += size.x;
            }
            y += line_height;
        }
    }
    draw_list->PopClipRect();
}

bool Application::readWindowPixels(std::vector<uint8_t>& rgb, int& width, int& height) {
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    if (width <= 0 || height <= 0) {
        return false;
    }
    rgb.resize(static_cast<size_t>(width) * height * 3);
    // The renderer leaves the default framebuffer bound.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

    // glReadPixels returns the bottom row first; flip to top-down.
    const size_t row_bytes = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> row(row_bytes);
    for (int y = 0; y < height / 2; ++y) {
        uint8_t* top = rgb.data() + static_cast<size_t>(y) * row_bytes;
        uint8_t* bottom = rgb.data() + static_cast<size_t>(height - 1 - y) * row_bytes;
        std::memcpy(row.data(), top, row_bytes);
        std::memcpy(top, bottom, row_bytes);
        std::memcpy(bottom, row.data(), row_bytes);
    }
    return true;
}

bool Application::saveScreenshotTo(const std::string& path) {
    std::vector<uint8_t> rgb;
    int width = 0;
    int height = 0;
    const bool ok = screenshot_window_ ? readWindowPixels(rgb, width, height)
                                       : renderer_.readback(rgb, width, height);
    if (!ok) {
        return false;
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::fwrite(rgb.data(), 1, rgb.size(), file);
    std::fclose(file);
    log::addf(log::Level::Info, "Screenshot written to %s (%dx%d)", path.c_str(), width, height);
    return true;
}

bool Application::saveScreenshot() {
    if (!saveScreenshotTo(screenshot_path_)) {
        return false;
    }
    screenshot_path_.clear();
    return true;
}

void Application::shutdown() {
    engine_.close();
    // Detach before audio_sink_ is destroyed; the engine destructor runs after
    // the sink member and must not touch it.
    engine_.setAudioSink(nullptr);
    audio_sink_.close();
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
