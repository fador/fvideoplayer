# HEVC Player

A focused HEVC/H.265 video player built around the custom `kvzdec` decoder.

- **Decoder**: [kvzdec](https://gitlab.tuni.fi/cs/ultravideo/hevc/kvzdec) (git submodule)
- **Containers**: native demuxers for MP4, Matroska (MKV) and AVI — no libavformat
- **GUI**: Dear ImGui + SDL3, custom YUV -> RGB video rendering
- **Playback**: correct-FPS pacing with a ~2 second decode-ahead buffer, keyframe seeking
- **Statistics**: live decode performance, buffer levels, per-frame info
- **Audio**: dummy sink interface only (real audio planned)

## Building

Prerequisites: CMake 3.25+, a C++17 toolchain, Ninja or Visual Studio.

```sh
git clone --recursive <repo-url> hevc_player
cd hevc_player
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows with Visual Studio, run from a developer command prompt (vcvars64).

## Usage

```
hevc_player [file]
```
