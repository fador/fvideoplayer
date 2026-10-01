#include "app/Application.hpp"
#include "decode/DecodeDump.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void printUsage(const char* program) {
    std::printf("usage: %s [options] [file]\n", program);
    std::printf("  --frames N          quit after N rendered frames (smoke test)\n");
    std::printf("  --seek SECONDS      seek to the given position after opening\n");
    std::printf("  --dump-yuv PATH     decode the file to planar YUV and exit\n");
    std::printf("  --dump-frames N     limit --dump-yuv to N frames\n");
    std::printf("  --dump-skip N       skip N frames before --dump-yuv output\n");
    std::printf("  --screenshot PATH   write the rendered frame as PPM when quitting\n");
    std::printf("  -h, --help          show this help\n");
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
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return EXIT_SUCCESS;
        } else if (input.empty()) {
            input = arg;
        }
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
