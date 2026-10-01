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
  menu. AAC-LC is decoded by a from-scratch decoder (no external codec
  libraries) and played through an SDL3 float sink with software volume and
  mute; see [Audio decoding](#audio-decoding) for details

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

Unit tests cover the hvcC parser, NAL splitting, all three container
demuxers (built from minimal in-test MP4/MKV/AVI files) and the audio
modules (bit reader, Huffman codebooks, MDCT/windows, volume gain, SDL sink
and AAC frame decoding from embedded ADTS/raw vectors).

## Usage

```
hevc_player [options] [file]

  --frames N         quit after N rendered frames (smoke test)
  --seek SECONDS     seek to the given position after opening
  --info             print stream information and exit
  --subtitle N       select subtitle option N (-1 = none)
  --audio N          select audio track N
  --volume X         set volume 0.0..1.0
  --mute             start muted
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
| Up / Down | volume +/- 5% |
| M | mute / unmute |
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
  audio/      PCM types, volume/mute gain, SDL3 sink and the AAC decoder
              (aac/: bit reader, ADTS + AudioSpecificConfig parsers, Huffman
              codebooks, ICS/TNS tools, MDCT/filterbank)
```

The demux + decode pipeline runs on a dedicated thread and fills a bounded
frame queue (about two seconds, capped by a memory budget). The UI thread
advances a monotonic clock and displays frames as their presentation
timestamps become due, dropping frames that are already late.

Audio follows the same pattern: the pipeline decodes packets and pushes float
PCM to the sink, which keeps only a short queue (250 ms) so audio stays close
to the video clock. Volume and mute are applied as a software gain before the
samples reach the device.

## Audio decoding

The AAC-LC decoder is written from scratch, with no external codec library.
Implemented:

- MPEG-4 AudioSpecificConfig parsing (MP4/Matroska extradata), including
  SBR/PS detection and explicit sample rates, plus ADTS framing for
  raw/AVI streams
- Huffman codebooks 0..11 with escape sequences, section data, differential
  scale factors, pulse data and TNS (Levinson-Durbin LPC)
- M/S stereo, intensity stereo, perceptual noise substitution (PNS) and
  the channel maps for configurations 1..6 (mono to 5.1)
- Inverse MDCT (FFT based, verified against the direct transform), sine and
  KBD windows, all window sequences and the inter-frame overlap-add
- PCM gain, mute and the SDL3 output path with a bounded queue

Not supported: SBR/PS (HE-AAC), AAC Main/LTP/SSR, 960-sample frames,
coupling elements (CCE) and program config elements (PCE). Such tracks are
reported once in the log and skipped. Audio access units are read from
Matroska; MP4 and AVI audio tracks are listed but their samples are not read
yet.

The decoder was verified sample-exact against a reference decoder for mono,
stereo, 5.1, short-window transients, PNS/intensity and all 12 sampling
frequencies; unit tests cover the codebook tables, the KBD window values and
full frame decoding from both ADTS and raw access units.

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
