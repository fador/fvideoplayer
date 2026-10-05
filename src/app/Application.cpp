#include "app/Application.hpp"

#include "app/Win98Theme.hpp"

#include <SDL3/SDL_opengl.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cctype>
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

// ASS styles name a font family and typeset the line assuming its metrics, so
// substituting a wider font makes the syllables collide. Map the families that
// matter onto the files Windows ships them in, bold and italic included.
struct SubtitleFontFile {
    const char* family;
    const char* regular;
    const char* bold;
    const char* italic;
    const char* bold_italic;
};

constexpr SubtitleFontFile kSubtitleFontFiles[] = {
    {"monotype corsiva", "MTCORSVA.TTF", nullptr, nullptr, nullptr},
    {"arial", "arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"},
    {"arial black", "ariblk.ttf", nullptr, nullptr, nullptr},
    {"calibri", "calibri.ttf", "calibrib.ttf", "calibrii.ttf", "calibriz.ttf"},
    {"cambria", "cambria.ttc", "cambriab.ttf", "cambriai.ttf", "cambriaz.ttf"},
    {"comic sans ms", "comic.ttf", "comicbd.ttf", "comici.ttf", "comicz.ttf"},
    {"consolas", "consola.ttf", "consolab.ttf", "consolai.ttf", "consolaz.ttf"},
    {"courier new", "cour.ttf", "courbd.ttf", "couri.ttf", "courbi.ttf"},
    {"gabriola", "gabriola.ttf", nullptr, nullptr, nullptr},
    {"georgia", "georgia.ttf", "georgiab.ttf", "georgiai.ttf", "georgiaz.ttf"},
    {"impact", "impact.ttf", nullptr, nullptr, nullptr},
    {"segoe ui", "segoeui.ttf", "segoeuib.ttf", "segoeuii.ttf", "segoeuiz.ttf"},
    {"tahoma", "tahoma.ttf", "tahomabd.ttf", nullptr, nullptr},
    {"times new roman", "times.ttf", "timesbd.ttf", "timesi.ttf", "timesbi.ttf"},
    {"trebuchet ms", "trebuc.ttf", "trebucbd.ttf", "trebucit.ttf", "trebucbi.ttf"},
    {"verdana", "verdana.ttf", "verdanab.ttf", "verdanai.ttf", "verdanaz.ttf"},
    {"dfkai-sb", "kaiu.ttf", nullptr, nullptr, nullptr},
    {"ms gothic", "msgothic.ttc", nullptr, nullptr, nullptr},
    {"yu gothic", "YuGothM.ttc", "YuGothB.ttc", nullptr, nullptr},
};

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

const char* sliceName(decode::SliceType slice_type) {
    switch (slice_type) {
        case decode::SliceType::B:
            return "B";
        case decode::SliceType::P:
            return "P";
        case decode::SliceType::I:
            return "I";
        default:
            return "?";
    }
}

const char* hashResultName(decode::HashResult result) {
    switch (result) {
        case decode::HashResult::NotPresent:
            return "n/a";
        case decode::HashResult::Match:
            return "match";
        case decode::HashResult::Mismatch:
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
    win98::applyStyle();

    // The first font added becomes the UI font, so add the Win98 UI font first
    // and keep the CJK font for subtitles only.
    const char* ui_font_file = nullptr;
    io.FontDefault = win98::loadUiFont(&ui_font_file);
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
    win98::stylePlots();
    imgui_ready_ = true;

    std::string renderer_error;
    if (!renderer_.init(&renderer_error)) {
        std::fprintf(stderr, "video renderer init failed: %s\n", renderer_error.c_str());
        return false;
    }
    engine_.setAudioSink(&audio_sink_);

    log::addf(log::Level::Info, "UI font: %s", ui_font_file != nullptr ? ui_font_file : "?");
    std::string backends;
#if PLAYER_HAS_KVZDEC
    backends += std::string("kvzdec ") + decode::hevcBackendVersion();
#endif
#if PLAYER_HAS_F264DEC
    if (!backends.empty()) {
        backends += ", ";
    }
    backends += std::string("f264dec ") + decode::h264BackendVersion();
#endif
    if (backends.empty()) {
        backends = "no external decoders";
    }
    log::addf(log::Level::Info, "HEVC Player started (%s)", backends.c_str());
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
    if (current && current->image.valid() && engine_.currentSerial() != uploaded_serial_) {
        renderer_.upload(&current->image);
            uploaded_serial_ = engine_.currentSerial();
        }
        video_texture_ = renderer_.render(color_);
        // A UI capture does not need a decoded frame, a video capture does.
        if (!screenshot_path_.empty() && (video_texture_ != 0 || screenshot_window_)) {
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
    drawFiltersWindow();
    // Menus are popup windows of ImGui, which only exist once everything else
    // has been submitted.
    win98::decoratePopups();
}

void Application::drawMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (win98::beginMenu("File")) {
            if (win98::menuItem("Open...", "Ctrl+O")) {
                openFileDialog();
            }
            win98::separator();
            if (win98::menuItem("Close")) {
                engine_.close();
                uploaded_serial_ = 0;
                video_texture_ = 0;
                subtitle_options_.clear();
                active_subtitle_ = -1;
                status_.clear();
            }
            win98::separator();
            if (win98::menuItem("Quit", "Alt+F4")) {
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
            win98::endMenu();
        }
        if (win98::beginMenu("Playback")) {
            const bool has_file = engine_.hasFile();
            if (win98::menuItem(engine_.state() == playback::State::Playing ? "Pause" : "Play",
                                "Space", false, has_file)) {
                engine_.togglePlayPause();
            }
            if (win98::menuItem("Stop", nullptr, false, has_file)) {
                engine_.stop();
            }
            win98::endMenu();
        }
        if (win98::beginMenu("Subtitles")) {
            const bool has_file = engine_.hasFile();
            if (win98::menuItem("None", nullptr, active_subtitle_ < 0, has_file)) {
                activateSubtitle(-1);
            }
            win98::separator();
            for (size_t i = 0; i < subtitle_options_.size(); ++i) {
                const SubtitleOption& option = subtitle_options_[i];
                const bool selected = active_subtitle_ == static_cast<int>(i);
                if (win98::menuItem(option.label.c_str(), nullptr, selected, !option.bitmap)) {
                    activateSubtitle(static_cast<int>(i));
                }
            }
            if (subtitle_options_.empty()) {
                win98::menuItem("(no subtitle tracks found)", nullptr, false, false);
            }
            win98::separator();
            if (win98::menuItem("Load from file...", nullptr, false, has_file)) {
                openSubtitleDialog();
            }
            win98::endMenu();
        }
        if (win98::beginMenu("Audio")) {
            const bool has_file = engine_.hasFile();
            const bool muted = engine_.muted() == audio::Mute::On;
            if (win98::menuItem("Mute", "M", muted, has_file)) {
                engine_.setMuted(muted ? audio::Mute::Off : audio::Mute::On);
            }
            int volume_percent = static_cast<int>(engine_.volume() * 100.0f + 0.5f);
            ImGui::SetNextItemWidth(160.0f);
            if (win98::sliderInt("Volume", &volume_percent, 0, 100,
                                 ImGuiSliderFlags_AlwaysClamp)) {
                engine_.setVolume(static_cast<float>(volume_percent) / 100.0f);
            }
            ImGui::SameLine();
            const std::string volume_text = std::to_string(volume_percent) + "%";
            ImGui::TextUnformatted(volume_text.c_str());
            win98::separator();
            if (win98::menuItem("None", nullptr, !engine_.audioEnabled(), has_file)) {
                engine_.selectAudioTrack(playback::PlaybackEngine::kNoAudioTrack);
            }
            win98::separator();
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
                if (win98::menuItem(label.c_str(), nullptr, selected, has_file)) {
                    engine_.selectAudioTrack(track.index);
                }
            }
            if (tracks.empty()) {
                win98::menuItem("(no audio tracks found)", nullptr, false, false);
            }
            win98::endMenu();
        }
        if (win98::beginMenu("Filters")) {
            if (win98::menuItem("Video filters...", nullptr, false, true)) {
                show_filters_ = true;
            }
            if (win98::menuItem("Audio filters...", nullptr, false, true)) {
                show_filters_ = true;
            }
            win98::separator();
            const std::vector<std::string> video_names = engine_.videoFilterNames();
            const std::vector<std::string> audio_names = engine_.audioFilterNames();
            if (video_names.empty() && audio_names.empty()) {
                win98::menuItem("(no active filters)", nullptr, false, false);
            } else {
                for (const std::string& name : video_names) {
                    win98::menuItem(("Video: " + name).c_str(), nullptr, false, false);
                }
                for (const std::string& name : audio_names) {
                    win98::menuItem(("Audio: " + name).c_str(), nullptr, false, false);
                }
            }
            win98::separator();
            std::string clear_error;
            if (win98::menuItem("Clear video filters", nullptr, false, !video_names.empty())) {
                engine_.setVideoFilters("", clear_error);
                video_filter_input_[0] = '\0';
            }
            if (win98::menuItem("Clear audio filters", nullptr, false, !audio_names.empty())) {
                engine_.setAudioFilters("", clear_error);
                audio_filter_input_[0] = '\0';
            }
            win98::endMenu();
        }
        if (win98::beginMenu("View")) {
            win98::menuItemToggle("Statistics", "S", &show_stats_);
            win98::menuItemToggle("Log", "L", &show_log_);
            win98::separator();
            if (win98::menuItemRadio("BT.601 color", nullptr, !color_.bt709)) {
                color_.bt709 = false;
            }
            if (win98::menuItemRadio("BT.709 color", nullptr, color_.bt709)) {
                color_.bt709 = true;
            }
            win98::separator();
            if (win98::menuItemRadio("Limited range", nullptr, !color_.full_range)) {
                color_.full_range = false;
            }
            if (win98::menuItemRadio("Full range", nullptr, color_.full_range)) {
                color_.full_range = true;
            }
            win98::endMenu();
        }
        if (win98::beginMenu("Help")) {
            win98::menuItem("Space: play/pause  Left/Right: seek 5s", nullptr, false, false);
            win98::menuItem("Up/Down: volume  M: mute", nullptr, false, false);
            win98::menuItem("F: fullscreen  S: statistics  L: log", nullptr, false, false);
            win98::endMenu();
        }
    }
    win98::endMainMenuBar();
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
    // The video area is a black well in the grey chrome, as in Media Player
    // Classic; the transport row below stays on the window face.
    ImGui::GetWindowDrawList()->AddRectFilled(
        cursor, ImVec2(cursor.x + video_size.x, cursor.y + video_size.y),
        IM_COL32(0x00, 0x00, 0x00, 0xFF));
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
        std::string message = "No file open";
        if (engine_.hasFile()) {
            const player::demux::VideoTrackInfo& video = engine_.videoInfo();
            if (video.codec == player::demux::Codec::Unknown) {
                message = video.codec_name.empty()
                              ? "Audio only"
                              : "Audio only (" + video.codec_name + " video not supported)";
            } else {
                message = engine_.buffering() ? "Buffering..." : "No frame";
            }
        }
        const ImVec2 text_size = ImGui::CalcTextSize(message.c_str());
        ImGui::SetCursorScreenPos(ImVec2(cursor.x + (video_size.x - text_size.x) * 0.5f,
                                         cursor.y + (video_size.y - text_size.y) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
        ImGui::TextUnformatted(message.c_str());
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(cursor);
    }

    ImGui::SetCursorScreenPos(ImVec2(cursor.x, cursor.y + video_size.y + 4.0f));
    drawTransport();
    ImGui::End();
}

void Application::drawTransport() {
    const bool has_file = engine_.hasFile();
    const double duration = engine_.duration();

    if (win98::button(engine_.state() == playback::State::Playing ? "Pause" : "Play")) {
        engine_.togglePlayPause();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!has_file);
    if (win98::button("Stop")) {
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
    win98::sliderFloat("##seek", &drag_position_, 0.0f,
                       static_cast<float>(duration > 0.0 ? duration : 1.0));
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
    if (win98::button(muted ? "Unmute" : "Mute", ImVec2(64.0f, 0.0f))) {
        engine_.setMuted(muted ? audio::Mute::Off : audio::Mute::On);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("M toggles mute, Up/Down adjust volume");
    }

    ImGui::SameLine();
    int volume_percent = static_cast<int>(engine_.volume() * 100.0f + 0.5f);
    ImGui::SetNextItemWidth(120.0f);
    if (win98::sliderInt("##volume", &volume_percent, 0, 100, ImGuiSliderFlags_AlwaysClamp)) {
        engine_.setVolume(static_cast<float>(volume_percent) / 100.0f);
    }
    ImGui::SameLine();
    const std::string volume_text = std::to_string(volume_percent) + "%";
    ImGui::TextUnformatted(volume_text.c_str());
}

void Application::drawStatsWindow() {
    if (!show_stats_) {
        return;
    }
    stats::Snapshot snapshot = engine_.stats().snapshot();

    ImGui::Begin("Statistics", nullptr);
    if (win98::collapsingHeader("Stream", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (win98::collapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (win98::collapsingHeader("Decode performance", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (win98::collapsingHeader("Current frame", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (snapshot.current.valid) {
            ImGui::Text("PTS: %.3f s   POC: %d", snapshot.current.pts / 1000.0,
                        snapshot.current.poc);
            ImGui::Text("QP: %d   Slice: %s", snapshot.current.qp,
                        sliceName(snapshot.current.slice_type));
            ImGui::Text("Decode: %.2f ms   Sample: %zu bytes", snapshot.current.decode_ms,
                        snapshot.current.sample_bytes);
            ImGui::Text("Hash check: %s", hashResultName(snapshot.current.hash_result));
        } else {
            ImGui::TextUnformatted("No frame decoded yet");
        }
    }

    if (win98::collapsingHeader("Buffer level (s)")) {
        if (!snapshot.queue_history.empty() && ImPlot::BeginPlot("##buffer", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("sample", "seconds");
            ImPlot::PlotLine("buffered", snapshot.queue_history.data(),
                             static_cast<int>(snapshot.queue_history.size()));
            ImPlot::EndPlot();
        }
    }

    if (win98::collapsingHeader("Bitrate (Mbps)")) {
        if (!snapshot.bitrate_history_mbps.empty() &&
            ImPlot::BeginPlot("##bitrate", ImVec2(-1, 120))) {
            ImPlot::SetupAxes("sample", "Mbps");
            ImPlot::PlotLine("bitrate", snapshot.bitrate_history_mbps.data(),
                             static_cast<int>(snapshot.bitrate_history_mbps.size()));
            ImPlot::EndPlot();
        }
    }
    ImGui::End();
    win98::windowFrame("Statistics", &show_stats_);
}

void Application::drawLogWindow() {
    if (!show_log_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(720.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Log", nullptr);

    if (win98::button("Clear")) {
        log::clear();
    }
    ImGui::SameLine();
    win98::checkbox("Auto-scroll", &log_autoscroll_);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu messages", log_cache_.size());
    win98::separator();

    const uint64_t version = log::version();
    if (version != log_version_) {
        log_cache_ = log::snapshot();
        log_version_ = version;
    }

    // The log reads like a console window: black text on a sunken white field.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, win98::kWindow);
    ImGui::BeginChild("##log_scroll", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const log::Entry& entry : log_cache_) {
        ImVec4 color(0.0f, 0.0f, 0.0f, 1.0f);
        switch (entry.level) {
            case log::Level::Debug:
                color = ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
                break;
            case log::Level::Info:
                color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
                break;
            case log::Level::Warning:
                color = ImVec4(0.55f, 0.25f, 0.0f, 1.0f);
                break;
            case log::Level::Error:
                color = ImVec4(0.65f, 0.0f, 0.0f, 1.0f);
                break;
        }
        ImGui::TextDisabled("%7.2f", entry.time_seconds);
        ImGui::SameLine();
        ImGui::TextColored(color, "[%s] %s", log::levelName(entry.level), entry.text.c_str());
    }
    if (log_autoscroll_) {
        ImGui::SetScrollHereY(1.0f);
    }
    const ImVec2 log_bounds_min = ImGui::GetWindowPos();
    const ImVec2 log_bounds_size = ImGui::GetWindowSize();
    ImGui::EndChild();
    win98::sunkenBorder(log_bounds_min,
                        ImVec2(log_bounds_min.x + log_bounds_size.x,
                               log_bounds_min.y + log_bounds_size.y));
    ImGui::PopStyleColor();
    ImGui::End();
    win98::windowFrame("Log", &show_log_);
}

bool Application::setVideoFilters(const std::string& spec, std::string& error) {
    if (!engine_.setVideoFilters(spec, error)) {
        return false;
    }
    std::snprintf(video_filter_input_.data(), video_filter_input_.size(), "%s", spec.c_str());
    return true;
}

bool Application::setAudioFilters(const std::string& spec, std::string& error) {
    if (!engine_.setAudioFilters(spec, error)) {
        return false;
    }
    std::snprintf(audio_filter_input_.data(), audio_filter_input_.size(), "%s", spec.c_str());
    return true;
}

void Application::drawFiltersWindow() {
    if (!show_filters_) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640.0f, 300.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Filters", nullptr);

    ImGui::TextUnformatted("Post-decode filter graphs in ffmpeg syntax, applied on the pipeline thread.");
    ImGui::TextDisabled(
        "video: scale=W:H  crop=W:H:X:Y  flip=h|v|hv  grayscale  eq=brightness:contrast:saturation");
    ImGui::TextDisabled("audio: volume=GAIN|NDB  resample=RATE  channels=1|2  equalizer=bass:mid:treble");
    win98::separator();

    const auto activeNames = [](const std::vector<std::string>& names) {
        std::string text;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += names[i];
        }
        return text.empty() ? std::string("(none)") : text;
    };

    ImGui::TextUnformatted("Video graph");
    ImGui::SetNextItemWidth(-190.0f);
    const bool video_enter = win98::inputText("##video_filters", video_filter_input_.data(),
                                              video_filter_input_.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (win98::button("Apply##video") || video_enter) {
        std::string error;
        if (engine_.setVideoFilters(video_filter_input_.data(), error)) {
            filter_error_.clear();
        } else {
            filter_error_ = "Video: " + error;
        }
    }
    ImGui::SameLine();
    if (win98::button("Clear##video")) {
        std::string error;
        if (engine_.setVideoFilters("", error)) {
            video_filter_input_[0] = '\0';
            filter_error_.clear();
        } else {
            filter_error_ = "Video: " + error;
        }
    }
    ImGui::Text("Active: %s", activeNames(engine_.videoFilterNames()).c_str());

    win98::separator();
    ImGui::TextUnformatted("Audio graph");
    ImGui::SetNextItemWidth(-190.0f);
    const bool audio_enter = win98::inputText("##audio_filters", audio_filter_input_.data(),
                                              audio_filter_input_.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (win98::button("Apply##audio") || audio_enter) {
        std::string error;
        if (engine_.setAudioFilters(audio_filter_input_.data(), error)) {
            filter_error_.clear();
        } else {
            filter_error_ = "Audio: " + error;
        }
    }
    ImGui::SameLine();
    if (win98::button("Clear##audio")) {
        std::string error;
        if (engine_.setAudioFilters("", error)) {
            audio_filter_input_[0] = '\0';
            filter_error_.clear();
        } else {
            filter_error_ = "Audio: " + error;
        }
    }
    ImGui::Text("Active: %s", activeNames(engine_.audioFilterNames()).c_str());

    if (!filter_error_.empty()) {
        win98::separator();
        ImGui::TextColored(ImVec4(0.65f, 0.0f, 0.0f, 1.0f), "%s", filter_error_.c_str());
    }

    ImGui::End();
    win98::windowFrame("Filters", &show_filters_);
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
        {"Video files", "mp4;mkv;avi;hevc;h265;265;mov;mpg;mpeg;m1v"},
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

Application::SubtitleFont Application::subtitleFont(const std::string& family, bool bold, bool italic) {
    // Lowercase and collapse whitespace: "Monotype  Corsiva" is the same family
    // as "Monotype Corsiva".
    std::string key;
    bool pending_space = false;
    for (char c : family) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (std::isspace(byte)) {
            pending_space = !key.empty();
            continue;
        }
        if (pending_space) {
            key += ' ';
            pending_space = false;
        }
        key += static_cast<char>(std::tolower(byte));
    }
    if (bold) {
        key += "|b";
    }
    if (italic) {
        key += "|i";
    }
    for (const auto& entry : subtitle_fonts_) {
        if (entry.first == key) {
            return entry.second;
        }
    }

    SubtitleFont result;
    result.font = subtitle_font_;
    std::string family_key = key.substr(0, key.find('|'));
    for (const SubtitleFontFile& candidate : kSubtitleFontFiles) {
        if (family_key != candidate.family) {
            continue;
        }
        const char* file = candidate.regular;
        if (bold && italic && candidate.bold_italic) {
            file = candidate.bold_italic;
        } else if (bold && candidate.bold) {
            file = candidate.bold;
        } else if (italic && candidate.italic) {
            file = candidate.italic;
        }
        if (file != nullptr) {
            const std::filesystem::path path = std::filesystem::path("C:/Windows/Fonts") / file;
            if (std::filesystem::exists(path)) {
                result.font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str(), 32.0f);
            }
        }
        // The system may not ship the variant, so fake it while drawing.
        result.synthetic_bold = bold && file != candidate.bold && file != candidate.bold_italic;
        result.synthetic_italic = italic && file != candidate.italic && file != candidate.bold_italic;
        break;
    }
    subtitle_fonts_.emplace_back(key, result);
    return result;
}

// Decodes one UTF-8 codepoint and returns how many bytes it used.
size_t decodeUtf8(const char* text, size_t size, ImWchar& codepoint) {
    const unsigned char first = static_cast<unsigned char>(text[0]);
    size_t length = 1;
    ImWchar value = first;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        value = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        value = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        value = first & 0x07;
    }
    length = std::min(length, size);
    for (size_t i = 1; i < length; ++i) {
        value = static_cast<ImWchar>((value << 6) | (static_cast<unsigned char>(text[i]) & 0x3F));
    }
    codepoint = value;
    return length;
}

// Ear clipping for a simple polygon. ASS drawings can be concave, and the draw
// list only fills convex shapes.
void triangulatePolygon(const std::vector<ImVec2>& points, std::vector<int>& indices) {
    const int count = static_cast<int>(points.size());
    if (count < 3) {
        return;
    }
    std::vector<int> remaining(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        remaining[static_cast<size_t>(i)] = i;
    }
    float area = 0.0f;
    for (int i = 0; i < count; ++i) {
        const ImVec2& a = points[static_cast<size_t>(i)];
        const ImVec2& b = points[static_cast<size_t>((i + 1) % count)];
        area += a.x * b.y - b.x * a.y;
    }
    const float orientation = area < 0.0f ? -1.0f : 1.0f;
    const auto inside = [](const ImVec2& p, const ImVec2& a, const ImVec2& b, const ImVec2& c) {
        const float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
        const float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
        const float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);
        const bool negative = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
        const bool positive = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
        return !(negative && positive);
    };
    int guard = 0;
    while (remaining.size() > 3 && guard++ < count * count) {
        bool clipped = false;
        for (size_t i = 0; i < remaining.size(); ++i) {
            const int previous = remaining[(i + remaining.size() - 1) % remaining.size()];
            const int current = remaining[i];
            const int next = remaining[(i + 1) % remaining.size()];
            const ImVec2& a = points[static_cast<size_t>(previous)];
            const ImVec2& b = points[static_cast<size_t>(current)];
            const ImVec2& c = points[static_cast<size_t>(next)];
            const float cross = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (cross * orientation <= 0.0f) {
                continue;  // reflex corner
            }
            bool occupied = false;
            for (int other : remaining) {
                if (other != previous && other != current && other != next &&
                    inside(points[static_cast<size_t>(other)], a, b, c)) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) {
                continue;
            }
            indices.push_back(previous);
            indices.push_back(current);
            indices.push_back(next);
            remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            break;
        }
    }
    if (remaining.size() == 3) {
        indices.push_back(remaining[0]);
        indices.push_back(remaining[1]);
        indices.push_back(remaining[2]);
    }
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

    // One run of text with the colour it is drawn in.
    struct Run {
        std::string text;
        uint32_t color = 0xFFFFFF;
    };

    const auto channel = [](uint32_t color, int shift) {
        return static_cast<int>((color >> shift) & 0xFF);
    };
    const auto withAlpha = [&](uint32_t color, float opacity) {
        return IM_COL32(channel(color, 16), channel(color, 8), channel(color, 0),
                        static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f));
    };

    draw_list->PushClipRect(video_pos, ImVec2(video_pos.x + video_size.x,
                                              video_pos.y + video_size.y), true);
    for (const subtitle::Cue* cue : active) {
        const subtitle::AssRenderState rs = subtitle::evaluateAssCue(*cue, now);
        const SubtitleFont face = subtitleFont(cue->font_name, rs.bold, rs.italic);
        ImFont* cue_font = face.font ? face.font : font;
        // \fscx and \fscy stretch the glyphs; the draw list can only scale the
        // text uniformly, so the vertical factor picks the size.
        const float font_size = std::clamp(
            (rs.font_size > 0.0f ? rs.font_size : 42.0f) * scale * rs.scale_y / 100.0f, 8.0f,
            128.0f);
        const float line_height = font_size * 1.2f;
        const float outline = std::max(rs.border * scale, 0.0f);
        const float blur = std::max(rs.blur * scale, 0.0f);
        const float shadow = rs.shadow * scale;
        const float spacing = rs.spacing * scale;
        const ImU32 text_color = withAlpha(rs.primary, rs.text_opacity);
        const ImU32 outline_color = withAlpha(rs.outline, rs.outline_opacity);
        const ImU32 soft_color = withAlpha(rs.outline, rs.outline_opacity * 0.35f);
        const ImU32 shadow_color = withAlpha(rs.back, rs.shadow_opacity * 0.6f);

        // \frz, \fax, \fay and a faked italic shear are applied to every glyph
        // corner. The corners are built by hand rather than by AddText, which
        // also lets \fscx and \fscy stretch the glyphs independently.
        const bool italic_shear = face.synthetic_italic;
        const bool transformed =
            rs.angle != 0.0f || rs.shear_x != 0.0f || rs.shear_y != 0.0f || italic_shear;

        // Glyph metrics come from the face baked at its load size.
        constexpr float kFontLoadSize = 32.0f;  // size the faces are added at
        ImFontBaked* baked = cue_font->GetFontBaked(kFontLoadSize);
        const float glyph_scale = baked->Size > 0.0f ? font_size / baked->Size : 1.0f;
        const float stretch =
            rs.scale_y > 0.0f ? rs.scale_x / rs.scale_y : 1.0f;  // \fscx over \fscy
        const bool fast_text = !transformed && spacing <= 0.0f && stretch == 1.0f;
        const ImTextureRef font_texture = ImGui::GetIO().Fonts->TexRef;

        // Width of a run, letter spacing and stretching included.
        const auto run_width = [&](const std::string& text) {
            if (fast_text) {
                return cue_font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str()).x;
            }
            float width = 0.0f;
            size_t codepoints = 0;
            for (size_t i = 0; i < text.size();) {
                const unsigned char byte = static_cast<unsigned char>(text[i]);
                size_t length = 1;
                if ((byte & 0xE0) == 0xC0) {
                    length = 2;
                } else if ((byte & 0xF0) == 0xE0) {
                    length = 3;
                } else if ((byte & 0xF8) == 0xF0) {
                    length = 4;
                }
                ImWchar codepoint = 0;
                length = decodeUtf8(text.data() + i, text.size() - i, codepoint);
                const ImFontGlyph* glyph = baked->FindGlyph(codepoint);
                width += (glyph ? glyph->AdvanceX * glyph_scale * stretch : 0.0f) +
                         (codepoints > 0 ? spacing : 0.0f);
                ++codepoints;
                i += length;
            }
            return width;
        };

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
                append(syllable.text, sung ? rs.primary : rs.secondary);
            }
        } else {
            append(cue->text, rs.primary);
        }

        // Wrap each line on spaces, keeping the run colours. \q 1 and 2 turn
        // wrapping off.
        const bool wrap = cue->wrap_style != 1 && cue->wrap_style != 2;
        std::vector<std::vector<Run>> wrapped;
        for (const std::vector<Run>& line : lines) {
            std::vector<Run> current;
            float width = 0.0f;
            for (const Run& run : line) {
                std::istringstream words(run.text);
                std::string word;
                bool first_word = true;
                while (words >> word) {
                    const float word_width = run_width(word);
                    const float space = first_word ? 0.0f : font_size * 0.3f;
                    if (wrap && width + space + word_width > max_width && !current.empty()) {
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

        // The \p drawings of this cue as screen offsets, plus the box they
        // occupy, which is what the alignment positions.
        struct DrawingShape {
            std::vector<std::vector<ImVec2>> contours;
            float min_x = 0.0f;
            float min_y = 0.0f;
            float max_x = 0.0f;
            float max_y = 0.0f;
        };
        std::vector<DrawingShape> shapes;
        for (const subtitle::AssDrawing& drawing : cue->drawings) {
            DrawingShape shape;
            const float divisor = static_cast<float>(1 << (drawing.scale - 1));
            bool first = true;
            for (const auto& contour : drawing.contours) {
                std::vector<ImVec2> points;
                for (const auto& point : contour) {
                    const float x = point.first / divisor * scale;
                    const float y = point.second / divisor * scale;
                    if (first) {
                        shape.min_x = shape.max_x = x;
                        shape.min_y = shape.max_y = y;
                        first = false;
                    }
                    shape.min_x = std::min(shape.min_x, x);
                    shape.min_y = std::min(shape.min_y, y);
                    shape.max_x = std::max(shape.max_x, x);
                    shape.max_y = std::max(shape.max_y, y);
                    points.push_back(ImVec2(x, y));
                }
                if (points.size() >= 3) {
                    shape.contours.push_back(std::move(points));
                }
            }
            if (!shape.contours.empty()) {
                shapes.push_back(std::move(shape));
            }
        }

        // Anchor the block. Positioned lines use their own coordinates with the
        // alignment from \an; the rest sit inside their margins at the edge the
        // alignment names, which is how ASS places a line without \pos.
        float block_width = 0.0f;
        for (const std::vector<Run>& line : wrapped) {
            float width = 0.0f;
            for (const Run& run : line) {
                width += run_width(run.text);
            }
            block_width = std::max(block_width, width);
        }
        float block_height = line_height * static_cast<float>(wrapped.size());
        for (const DrawingShape& shape : shapes) {
            block_width = std::max(block_width, shape.max_x - shape.min_x);
            block_height = std::max(block_height, shape.max_y - shape.min_y);
        }
        if (wrapped.empty() && shapes.empty()) {
            continue;
        }
        const int alignment = rs.alignment >= 1 && rs.alignment <= 9 ? rs.alignment : 2;
        const int horizontal = (alignment - 1) % 3;  // 0 left, 1 center, 2 right
        const int vertical = (alignment - 1) / 3;    // 0 bottom, 1 middle, 2 top
        float anchor_x = 0.0f;
        if (rs.has_position) {
            anchor_x = video_pos.x + rs.pos_x * scale;
        } else if (horizontal == 0) {
            anchor_x = video_pos.x + cue->margin_l * scale;
        } else if (horizontal == 2) {
            anchor_x = video_pos.x + video_size.x - cue->margin_r * scale;
        } else {
            anchor_x = video_pos.x + video_size.x * 0.5f;
        }
        float top = 0.0f;
        if (rs.has_position) {
            if (vertical == 2) {
                top = video_pos.y + rs.pos_y * scale;
            } else if (vertical == 1) {
                top = video_pos.y + rs.pos_y * scale - block_height * 0.5f;
            } else {
                top = video_pos.y + rs.pos_y * scale - block_height;
            }
        } else {
            const float margin = (cue->margin_v > 0.0f ? cue->margin_v : 30.0f) * scale;
            if (vertical == 2) {
                top = video_pos.y + margin;
            } else if (vertical == 1) {
                top = video_pos.y + video_size.y * 0.5f - block_height * 0.5f;
            } else {
                top = video_pos.y + video_size.y - margin - block_height;
            }
        }
        const auto line_left = [&](float width) {
            if (horizontal == 1) {
                return anchor_x - width * 0.5f;
            }
            if (horizontal == 2) {
                return anchor_x - width;
            }
            return anchor_x;
        };

        // \frz, \fax, \fay and a faked italic shear move every glyph corner.
        // \org sets the origin; without it the alignment anchor is used.
        const ImVec2 pivot = cue->has_origin
                                 ? ImVec2(video_pos.x + cue->origin_x * scale,
                                          video_pos.y + cue->origin_y * scale)
                                 : ImVec2(anchor_x, top);
        const float radians = (rs.angle + (italic_shear ? -12.0f : 0.0f)) * 3.14159265f / 180.0f;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        const auto place = [&](float x, float y) {
            if (!transformed) {
                return ImVec2(x, y);
            }
            const float rx = x - pivot.x;
            const float ry = y - pivot.y;
            const float sheared_x = rx + rs.shear_x * ry;
            const float sheared_y = ry + rs.shear_y * rx;
            return ImVec2(pivot.x + sheared_x * cosine - sheared_y * sine,
                          pivot.y + sheared_x * sine + sheared_y * cosine);
        };
        // Draws a run glyph by glyph when the transform or the spacing needs it.
        const auto draw_text = [&](const std::string& text, float x, float y, ImU32 color) {
            if (fast_text) {
                draw_list->AddText(cue_font, font_size, ImVec2(x, y), color, text.c_str());
                return;
            }
            float pen = x;
            size_t i = 0;
            while (i < text.size()) {
                const unsigned char byte = static_cast<unsigned char>(text[i]);
                size_t length = 1;
                if ((byte & 0xE0) == 0xC0) {
                    length = 2;
                } else if ((byte & 0xF0) == 0xE0) {
                    length = 3;
                } else if ((byte & 0xF8) == 0xF0) {
                    length = 4;
                }
                ImWchar codepoint = 0;
                length = decodeUtf8(text.data() + i, text.size() - i, codepoint);
                const ImFontGlyph* glyph = baked->FindGlyph(codepoint);
                i += length;
                if (glyph == nullptr) {
                    continue;
                }
                const float x0 = pen + glyph->X0 * glyph_scale * stretch;
                const float x1 = pen + glyph->X1 * glyph_scale * stretch;
                const float y0 = y + glyph->Y0 * glyph_scale;
                const float y1 = y + glyph->Y1 * glyph_scale;
                draw_list->AddImageQuad(font_texture, place(x0, y0), place(x1, y0), place(x1, y1),
                                        place(x0, y1), ImVec2(glyph->U0, glyph->V0),
                                        ImVec2(glyph->U1, glyph->V0), ImVec2(glyph->U1, glyph->V1),
                                        ImVec2(glyph->U0, glyph->V1), color);
                pen += glyph->AdvanceX * glyph_scale * stretch + spacing;
            }
        };

        // Everything the cue draws.
        const auto draw_cue = [&] {
            float y = top;
            for (const std::vector<Run>& line : wrapped) {
                float width = 0.0f;
                for (const Run& run : line) {
                    width += run_width(run.text);
                }
                float x = line_left(width);
                const auto pass = [&](float dx, float dy, ImU32 color) {
                    float pen = x;
                    for (const Run& run : line) {
                        draw_text(run.text, pen + dx, y + dy, color);
                        pen += run_width(run.text);
                    }
                };
                // Outline the whole line first: outlining run by run lets a
                // run's outline eat into the glyphs of the one before it.
                if (shadow > 0.0f) {
                    pass(shadow, shadow, shadow_color);
                }
                const float soft_radius = outline + blur;
                if (soft_radius > 0.0f) {
                    const float step = std::max(soft_radius / 2.0f, 1.0f);
                    for (float dx = -soft_radius; dx <= soft_radius; dx += step) {
                        for (float dy = -soft_radius; dy <= soft_radius; dy += step) {
                            if (dx * dx + dy * dy > soft_radius * soft_radius) {
                                continue;
                            }
                            pass(dx, dy, soft_color);
                        }
                    }
                }
                if (outline > 0.0f) {
                    for (float dx = -outline; dx <= outline; dx += outline) {
                        for (float dy = -outline; dy <= outline; dy += outline) {
                            if (dx == 0.0f && dy == 0.0f) {
                                continue;
                            }
                            pass(dx, dy, outline_color);
                        }
                    }
                }
                if (face.synthetic_bold) {
                    pass(font_size * 0.03f, 0.0f, text_color);
                }
                float pen = x;
                for (const Run& run : line) {
                    const float run_w = run_width(run.text);
                    const ImU32 color = withAlpha(run.color, rs.text_opacity);
                    draw_text(run.text, pen, y, color);
                    // Underline and strikeout are thin bars under and through
                    // the run.
                    if (rs.underline || rs.strikeout) {
                        const float thickness = std::max(font_size * 0.06f, 1.0f);
                        if (rs.underline) {
                            draw_list->AddRectFilled(
                                ImVec2(pen, y + font_size * 0.92f),
                                ImVec2(pen + run_w, y + font_size * 0.92f + thickness), color);
                        }
                        if (rs.strikeout) {
                            draw_list->AddRectFilled(
                                ImVec2(pen, y + font_size * 0.45f),
                                ImVec2(pen + run_w, y + font_size * 0.45f + thickness), color);
                        }
                    }
                    pen += run_w;
                }
                y += line_height;
            }

            // Vector drawings: filled with the primary colour and outlined with
            // the border width.
            for (const DrawingShape& shape : shapes) {
                const float left = line_left(shape.max_x - shape.min_x) - shape.min_x;
                for (const std::vector<ImVec2>& contour : shape.contours) {
                    std::vector<ImVec2> points;
                    points.reserve(contour.size());
                    for (const ImVec2& point : contour) {
                        points.push_back(ImVec2(left + point.x, top + point.y - shape.min_y));
                    }
                    std::vector<int> indices;
                    triangulatePolygon(points, indices);
                    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
                        draw_list->AddTriangleFilled(
                            points[static_cast<size_t>(indices[i])],
                            points[static_cast<size_t>(indices[i + 1])],
                            points[static_cast<size_t>(indices[i + 2])], text_color);
                    }
                    if (outline > 0.0f) {
                        draw_list->AddPolyline(points.data(), static_cast<int>(points.size()),
                                               outline_color, ImDrawFlags_Closed,
                                               std::max(outline, 1.0f));
                    }
                }
            }
        };

        // Clips: rectangles shrink the video rectangle, and an inverse clip is
        // drawn as the four bands around its hole. A vector clip uses the box
        // of its points, since the draw list clips to rectangles only.
        ImVec4 clip_rect(video_pos.x, video_pos.y, video_pos.x + video_size.x,
                         video_pos.y + video_size.y);
        const subtitle::AssClip* inverse = nullptr;
        for (const subtitle::AssClip& clip : cue->clips) {
            float x1 = clip.rect ? clip.x1 : 0.0f;
            float y1 = clip.rect ? clip.y1 : 0.0f;
            float x2 = clip.rect ? clip.x2 : 0.0f;
            float y2 = clip.rect ? clip.y2 : 0.0f;
            if (!clip.rect) {
                for (const auto& point : clip.points) {
                    x1 = std::min(x1, point.first);
                    y1 = std::min(y1, point.second);
                    x2 = std::max(x2, point.first);
                    y2 = std::max(y2, point.second);
                }
            }
            if (clip.inverse) {
                if (inverse == nullptr) {
                    inverse = &clip;
                }
                continue;
            }
            clip_rect.x = std::max(clip_rect.x, video_pos.x + x1 * scale);
            clip_rect.y = std::max(clip_rect.y, video_pos.y + y1 * scale);
            clip_rect.z = std::min(clip_rect.z, video_pos.x + x2 * scale);
            clip_rect.w = std::min(clip_rect.w, video_pos.y + y2 * scale);
        }

        // Clips: the transformed text already carries its own corners, so only
        // the clip rectangles have to be applied here.
        if (inverse == nullptr) {
            draw_list->PushClipRect(ImVec2(clip_rect.x, clip_rect.y),
                                    ImVec2(clip_rect.z, clip_rect.w), true);
            draw_cue();
            draw_list->PopClipRect();
        } else {
            float hx1 = inverse->rect ? inverse->x1 : 0.0f;
            float hy1 = inverse->rect ? inverse->y1 : 0.0f;
            float hx2 = inverse->rect ? inverse->x2 : 0.0f;
            float hy2 = inverse->rect ? inverse->y2 : 0.0f;
            if (!inverse->rect) {
                for (const auto& point : inverse->points) {
                    hx1 = std::min(hx1, point.first);
                    hy1 = std::min(hy1, point.second);
                    hx2 = std::max(hx2, point.first);
                    hy2 = std::max(hy2, point.second);
                }
            }
            const float hole_x1 = video_pos.x + hx1 * scale;
            const float hole_y1 = video_pos.y + hy1 * scale;
            const float hole_x2 = video_pos.x + hx2 * scale;
            const float hole_y2 = video_pos.y + hy2 * scale;
            const ImVec4 bands[4] = {
                ImVec4(clip_rect.x, clip_rect.y, clip_rect.z, hole_y1),
                ImVec4(clip_rect.x, hole_y2, clip_rect.z, clip_rect.w),
                ImVec4(clip_rect.x, hole_y1, hole_x1, hole_y2),
                ImVec4(hole_x2, hole_y1, clip_rect.z, hole_y2),
            };
            for (const ImVec4& band : bands) {
                if (band.x >= band.z || band.y >= band.w) {
                    continue;
                }
                draw_list->PushClipRect(ImVec2(band.x, band.y), ImVec2(band.z, band.w), true);
                draw_cue();
                draw_list->PopClipRect();
            }
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
