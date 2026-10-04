# HEVC Player

A video player built around an external HEVC/H.265 decoder plus from-scratch
decoders for MPEG-1/2, Sorenson Spark, AAC-LC, Opus, MP3, FLAC and AC-3. It
demuxes MP4, Matroska, AVI, FLV, MPEG program streams and raw elementary
streams without libavformat, renders through SDL3 + Dear ImGui with a custom
OpenGL YUV -> RGB path, and shows live decode statistics.

## Supported codecs

### Video

| Codec | Decoder | Notes |
| --- | --- | --- |
| HEVC / H.265 | kvzdec (external, git submodule) | 8/10-bit, 4:2:0/4:2:2/4:4:4, tiles/WPP; bit-exact against the HM reference |
| MPEG-1 | from scratch | [details](docs/mpeg12.md) |
| MPEG-2 | from scratch | Main Profile at Main Level, field pictures, [details](docs/mpeg12.md) |
| Sorenson Spark (FLV1) | from scratch | H.263 baseline variant, [details](docs/flv1.md) |

A video track whose codec has no decoder is named and skipped: the file's
audio still plays, with the video area showing the codec name. A file with no
decodable video and no audio is rejected with a message naming the codec. See
[containers](docs/mpeg12.md#containers) for how each demuxer handles this.

### Audio

| Codec | Decoder | Notes |
| --- | --- | --- |
| AAC-LC | from scratch | mono to 5.1, ADTS and raw access units, [details](docs/audio.md#aac-lc) |
| Opus | from scratch | CELT, SILK and hybrid, packet loss concealment, [details](docs/audio.md#opus) |
| MP3 | from scratch | MPEG-1/2/2.5 Layer III (minimp3 scalar port), [details](docs/audio.md#mp3) |
| FLAC | from scratch | RFC 9639, sample-accurate seeking, [details](docs/audio.md#flac) |
| AC-3 | from scratch | Dolby Digital (ATSC A/52), mono to 5.1, coupling, [details](docs/audio.md#ac-3) |
| MP2 | from scratch | MPEG-1/2 Layer II, used for MPEG program streams |

PCM tracks are listed but have no decoder. All audio plays through an SDL3
float sink with software volume and mute.

### Containers

| Container | Extensions | Audio and subtitles |
| --- | --- | --- |
| MP4 / MOV | `.mp4`, `.m4a`, `.mov` | AAC/Opus/AC-3 tracks, `tx3g`/`wvtt` text |
| Matroska / WebM | `.mkv`, `.mka`, `.webm` | AAC/Opus/FLAC/AC-3 tracks, SRT/ASS/SSA text |
| AVI | `.avi` | MP3/AAC/AC-3/Opus tracks |
| Flash Video | `.flv` | MP3 or AAC |
| MPEG program stream | `.mpg`, `.mpeg`, `.vob`, `.evo` | MP2 or MP3 |
| Raw elementary stream | `.hevc`, `.m1v`, `.m2v`, `.mp3`, `.flac`, `.ac3`, ... | audio-only where applicable |

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

The suite covers the demuxers, the decoders, the filter graphs and the
playback engine, mostly from synthetic in-test files; the decoders are also
verified against ffmpeg as described in [verification](docs/verification.md).

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
  --vf SPEC          post-decode video filter graph, e.g.
                     "scale=1280:720,flip=h,eq=contrast=1.2"
  --af SPEC          post-decode audio filter graph, e.g.
                     "volume=1.5,equalizer=bass=6,resample=48000"
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

## Documentation

- [Architecture](docs/architecture.md) - module layout, playback pipeline, threading
- [Filter graphs](docs/filters.md) - `--vf` / `--af` syntax and built-in filters
- [MPEG-1 and MPEG-2](docs/mpeg12.md) - from-scratch MPEG video decoder
- [Sorenson Spark](docs/flv1.md) - from-scratch FLV1 decoder and FLV demuxing
- [Audio decoding](docs/audio.md) - AAC, Opus, MP3, FLAC, AC-3 and container audio
- [Subtitles](docs/subtitles.md) - ASS/SSA rendering and text track support
- [Verification](docs/verification.md) - how the decoders are tested

## License

The player sources are provided as-is. Third-party components keep their own
licenses: kvzdec (3-clause BSD), SDL3 (zlib), Dear ImGui and ImPlot (MIT).
