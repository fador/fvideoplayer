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
    std::printf("video: HEVC %ux%u\n", video.width, video.height);
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
    if (audio.present) {
        std::printf("audio: fourcc 0x%08X, %u channels, %u Hz\n", audio.codec_fourcc,
                    audio.channels, audio.sample_rate);
    } else {
        std::printf("audio: none\n");
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
    bool info_mode = false;
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
        return EXIT_SUCCESS;
    }

    player::Application app;
    if (!app.init("HEVC Player")) {
        return EXIT_FAILURE;
    }
    if (!screenshot.empty()) {
        app.setScreenshotPath(screenshot);
        app.setScreenshotEvery(screenshot_every);
    }
    if (!input.empty()) {
        app.openFile(input);
        if (seek_seconds >= 0.0) {
            app.seek(seek_seconds);
        }
        if (seek_seconds >= 0.0 || !screenshot.empty()) {
            app.play();
        }
    }
    return app.run(max_frames);
}
