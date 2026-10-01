# HEVC Player

A focused HEVC/H.265 video player built around the custom
[kvzdec](https://gitlab.tuni.fi/cs/ultravideo/hevc/kvzdec) decoder.

- **Decoder**: kvzdec (git submodule), wrapped through its public C API
- **Containers**: native demuxers for MP4, Matroska (MKV) and AVI — no
  libavformat; raw Annex B `.hevc` streams are supported as well
- **GUI**: Dear ImGui + SDL3 with a custom OpenGL YUV -> RGB render path
- **Playback**: correct-FPS pacing with a ~2 second decode-ahead buffer and
  keyframe seeking
- **Statistics**: live decode performance, buffer level, bitrate and
  per-frame decoder information
- **Subtitles**: embedded Matroska text tracks (SRT, ASS/SSA) and external
  SRT/ASS/SSA/WebVTT/MicroDVD files, selectable from the Subtitles menu and
  switchable on the fly; external files next to the video are discovered
  automatically
- **Audio**: container audio tracks are listed and selectable from the Audio
  menu; playback uses a dummy sink (packets are counted but not decoded yet)

## Building

Prerequisites: CMake 3.25+, a C++17 toolchain, Ninja or Visual Studio, and
Git submodules initialized.

```sh
git clone --recursive <repo-url> hevc_player
cd hevc_player
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows with Visual Studio, run the commands from a developer command
prompt (`vcvars64.bat`).

### Tests

```sh
ctest --test-dir build --output-on-failure
```

Unit tests cover the hvcC parser, NAL splitting and all three container
demuxers (built from minimal in-test MP4/MKV/AVI files).

## Usage

```
hevc_player [options] [file]

  --frames N         quit after N rendered frames (smoke test)
  --seek SECONDS     seek to the given position after opening
  --info             print stream information and exit
  --subtitle N       select subtitle option N (-1 = none)
  --audio N          select audio track N
  --screenshot PATH  write the first rendered frame as a PPM image and quit
  --screenshot-every N  with --screenshot, keep writing PPMs every N frames
  --dump-yuv PATH    decode the file to planar YUV and exit
  --dump-frames N    limit --dump-yuv to N frames
  --dump-skip N      skip N frames before --dump-yuv output
  -h, --help         show help
```

### Controls

| Input | Action |
| --- | --- |
| Space | play / pause |
| Left / Right | seek 5 seconds |
| S | toggle statistics window |
| L | toggle log console |
| F | toggle fullscreen |
| Drag & drop | open a file |
| File -> Open | native file dialog |

## Architecture

```
src/
  app/        SDL3 + ImGui application shell and UI
  demux/      container demuxers (MP4, MKV, AVI, raw HEVC) + hvcC parser
  subtitle/   SRT/ASS/SSA/WebVTT/MicroDVD parsers and cue lookup
  decode/     kvzdec wrapper, NAL handling, decode-to-YUV verification mode
  playback/   pipeline thread, frame queue, media clock
  render/     OpenGL YUV -> RGB renderer (8/10-bit, 4:2:0/4:2:2/4:4:4)
  stats/      thread-safe statistics collector
  audio/      audio sink interface + null sink
```

The demux + decode pipeline runs on a dedicated thread and fills a bounded
frame queue (about two seconds, capped by a memory budget). The UI thread
advances a monotonic clock and displays frames as their presentation
timestamps become due, dropping frames that are already late.

The player runs without a console window; messages are collected in a
thread-safe log buffer and can be viewed in the in-app log console
(`View -> Log` or `L`). CLI modes (`--info`, `--dump-yuv`, `--help`) keep
the console so their output can be redirected.

## Verification

The decode path was checked bit-exact against the HM reference outputs of
the kvzdec conformance suite for raw, MP4, MKV and AVI inputs (8/10-bit,
4:2:0/4:4:4, tiles/WPP). `--dump-yuv` performs a synchronous decode of the
whole file for this purpose.

## License

The player sources are provided as-is. Third-party components keep their own
licenses: kvzdec (3-clause BSD), SDL3 (zlib), Dear ImGui and ImPlot (MIT).
