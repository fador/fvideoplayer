#include "app/Application.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    std::string input;
    int max_frames = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            max_frames = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: %s [options] [file]\n", argv[0]);
            std::printf("  --frames N   quit after N rendered frames (smoke test)\n");
            return EXIT_SUCCESS;
        } else if (input.empty()) {
            input = arg;
        }
    }

    player::Application app;
    if (!app.init("HEVC Player")) {
        return EXIT_FAILURE;
    }
    return app.run(max_frames);
}
