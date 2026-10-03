#include "app/Application.hpp"
#include "decode/DecodeDump.hpp"
#include "demux/Demuxer.hpp"
#include "demux/HvcC.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

const char* videoCodecName(player::demux::Codec codec) {
    switch (codec) {
        case player::demux::Codec::Hevc:
            return "HEVC";
        case player::demux::Codec::Mpeg1:
            return "MPEG-1";
        case player::demux::Codec::Mpeg2:
            return "MPEG-2";
        default:
            return "none";
    }
}

// The GUI does not need a console. CLI modes keep it so that output can be
// redirected and shells wait for the process to finish.
void detachConsoleForGui() {
#ifdef _WIN32
    if (const HWND console = GetConsoleWindow()) {
        ShowWindow(console, SW_HIDE);
    }
    FreeConsole();
#endif
}

void printUsage(const char* program) {
    std::printf("usage: %s [options] [file]\n", program);
    std::printf("  --frames N          quit after N rendered frames (smoke test)\n");
    std::printf("  --seek SECONDS      seek to the given position after opening\n");
    std::printf("  --info              print stream information and exit\n");
    std::printf("  --dump-yuv PATH     decode the file to planar YUV and exit\n");
    std::printf("  --dump-frames N     limit --dump-yuv to N frames\n");
    std::printf("  --dump-skip N       skip N frames before --dump-yuv output\n");
    std::printf("  --screenshot PATH   write the rendered frame as PPM when quitting\n");
    std::printf("  --screenshot-every N  with --screenshot, keep writing PPMs every N frames\n");
    std::printf("  --screenshot-ui     capture the whole window instead of just the video\n");
    std::printf("  --subtitle N        select subtitle option N (-1 = none)\n");
    std::printf("  --audio N           select audio track N\n");
    std::printf("  --volume X          set volume 0.0..1.0\n");
    std::printf("  --mute              start muted\n");
    std::printf("  -h, --help          show this help\n");
}

int printFileInfo(const std::string& input) {
    std::string error;
    std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(input, error);
    if (!demuxer) {
        std::fprintf(stderr, "failed to open %s: %s\n", input.c_str(), error.c_str());
        return EXIT_FAILURE;
    }
    const player::demux::VideoTrackInfo& video = demuxer->video();
    const player::demux::AudioTrackInfo& audio = demuxer->audio();

    std::printf("file: %s\n", input.c_str());
    std::printf("video: %s %ux%u\n", videoCodecName(video.codec), video.width, video.height);
    std::printf("fps: %.3f\n", video.fps);
    std::printf("timescale: %u\n", video.timescale);
    std::printf("duration: %.3f s\n", demuxer->duration());
    std::printf("sample format: %s (nal length size %u)\n",
                video.format.annexb ? "annex-b" : "length-prefixed",
                video.format.nal_length_size);
    if (!video.hvcc.empty()) {
        player::demux::HvccInfo hvcc;
        if (player::demux::parseHvcc(video.hvcc.data(), video.hvcc.size(), hvcc, nullptr)) {
            std::printf("hvcC: %s @ %s, %d-bit, chroma idc %d, %u parameter sets\n",
                        player::demux::hvccProfileName(hvcc).c_str(),
                        player::demux::hvccLevelName(hvcc).c_str(),
                        8 + hvcc.bit_depth_luma_minus8, hvcc.chroma_format_idc,
                        static_cast<unsigned>(hvcc.nals.size()));
        }
    }
    const std::vector<player::demux::AudioTrackInfo> audio_tracks = demuxer->audioTracks();
    if (audio_tracks.empty()) {
        std::printf("audio: none\n");
    } else {
        for (const player::demux::AudioTrackInfo& track : audio_tracks) {
            std::printf("audio %zu: '%s' %s (%s), %u channels, %u Hz\n", track.index,
                        track.name.c_str(), track.language.c_str(), track.codec.c_str(),
                        track.channels, track.sample_rate);
        }
    }
    (void)audio;

    const std::vector<player::demux::SubtitleTrackInfo> subtitle_tracks = demuxer->subtitleTracks();
    if (subtitle_tracks.empty()) {
        std::printf("subtitles: none\n");
    } else {
        for (const player::demux::SubtitleTrackInfo& track : subtitle_tracks) {
            std::printf("subtitle %zu: '%s' %s (%s)%s\n", track.index, track.name.c_str(),
                        track.language.c_str(), track.codec.c_str(),
                        track.bitmap ? " [bitmap, unsupported]" : "");
        }
    }
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input;
    std::string dump_yuv;
    int max_frames = -1;
    int dump_frames = -1;
    int dump_skip = 0;
    double seek_seconds = -1.0;
    std::string screenshot;
    int screenshot_every = 0;
    bool screenshot_ui = false;
    bool info_mode = false;
    int subtitle_option = -2;  // -1 disables, >= 0 selects, -2 unset
    int audio_track = -1;      // -1 unset
    double volume = -1.0;      // < 0 unset
    bool mute = false;
    bool help_requested = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (arg == "--dump-yuv" && i + 1 < argc) {
            dump_yuv = argv[++i];
        } else if (arg == "--dump-frames" && i + 1 < argc) {
            dump_frames = std::atoi(argv[++i]);
        } else if (arg == "--dump-skip" && i + 1 < argc) {
            dump_skip = std::atoi(argv[++i]);
        } else if (arg == "--seek" && i + 1 < argc) {
            seek_seconds = std::atof(argv[++i]);
        } else if (arg == "--screenshot" && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (arg == "--screenshot-every" && i + 1 < argc) {
            screenshot_every = std::atoi(argv[++i]);
        } else if (arg == "--screenshot-ui") {
            screenshot_ui = true;
        } else if (arg == "--subtitle" && i + 1 < argc) {
            subtitle_option = std::atoi(argv[++i]);
        } else if (arg == "--audio" && i + 1 < argc) {
            audio_track = std::atoi(argv[++i]);
        } else if (arg == "--volume" && i + 1 < argc) {
            volume = std::atof(argv[++i]);
        } else if (arg == "--mute") {
            mute = true;
        } else if (arg == "--info") {
            info_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            help_requested = true;
        } else if (input.empty()) {
            input = arg;
        }
    }

    const bool cli_mode = help_requested || info_mode || !dump_yuv.empty();
    if (!cli_mode) {
        detachConsoleForGui();
    }

    if (help_requested) {
        printUsage(argv[0]);
        return EXIT_SUCCESS;
    }

    if (info_mode) {
        if (input.empty()) {
            std::fprintf(stderr, "--info requires an input file\n");
            return EXIT_FAILURE;
        }
        return printFileInfo(input);
    }

    if (!dump_yuv.empty()) {
        if (input.empty()) {
            std::fprintf(stderr, "--dump-yuv requires an input file\n");
            return EXIT_FAILURE;
        }
        player::decode::DumpStats stats;
        std::string error;
        if (player::decode::dumpToYuv(input, dump_yuv, dump_frames, dump_skip, &stats, &error) != 0) {
            std::fprintf(stderr, "dump failed: %s\n", error.c_str());
            return EXIT_FAILURE;
        }
        const double fps = stats.seconds > 0.0 ? stats.frames / stats.seconds : 0.0;
        std::printf("Decoded %lld frames (%ux%u) in %.3f s (%.1f fps)\n",
                    static_cast<long long>(stats.frames), stats.width, stats.height, stats.seconds,
                    fps);
        if (stats.damaged > 0) {
            std::printf("%lld pictures had a damaged slice; the last gave up at "
                        "macroblock %d,%d bit %zu (%s)\n",
                        static_cast<long long>(stats.damaged), stats.damage_mb_x,
                        stats.damage_mb_y, stats.damage_bit, stats.damage_reason);
        }
        return EXIT_SUCCESS;
    }

    player::Application app;
    if (!app.init("HEVC Player")) {
        return EXIT_FAILURE;
    }
    if (!screenshot.empty()) {
        app.setScreenshotPath(screenshot);
        app.setScreenshotEvery(screenshot_every);
        app.setScreenshotWindow(screenshot_ui);
    }
    if (volume >= 0.0) {
        app.setVolume(static_cast<float>(volume));
    }
    if (mute) {
        app.setMuted(player::audio::Mute::On);
    }
    if (!input.empty()) {
        app.openFile(input);
        if (subtitle_option >= -1) {
            app.selectSubtitle(subtitle_option);
        }
        if (audio_track >= 0) {
            app.selectAudio(audio_track);
        }
        if (seek_seconds >= 0.0) {
            app.seek(seek_seconds);
        }
        if (seek_seconds >= 0.0 || !screenshot.empty()) {
            app.play();
        }
    }
    return app.run(max_frames);
}
