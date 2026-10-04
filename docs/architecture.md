# Architecture

```
src/
  app/        SDL3 + ImGui application shell and UI, plus the Win98 theme
              (palette, 3D bevels, gradient captions, classic controls)
  demux/      container demuxers (MP4, MKV, AVI, FLV, MPEG program streams,
              raw HEVC, raw MPEG-1/2, raw MP3, raw FLAC, raw AC-3) + hvcC
              and esds parsers
  subtitle/   SRT/ASS/SSA/WebVTT/MicroDVD parsers and cue lookup
  decode/     codec-neutral Frame, decode backends (KvzdecDecoder,
              mpeg12/ from-scratch MPEG-1 and MPEG-2, flv1/ from-scratch
              Sorenson Spark), NAL handling, decode-to-YUV verification mode
  filters/    post-decode filter graphs (ffmpeg-style spec parsing, video and
              audio filter chains, the built-in filters)
  playback/   pipeline thread, frame queue, media clock
  render/     OpenGL YUV -> RGB renderer (8/10-bit, 4:2:0/4:2:2/4:4:4)
  stats/      thread-safe statistics collector
  audio/      PCM types, volume/mute gain, AVX2 kernels with CPUID dispatch,
              timestamped audio buffer, SDL3 sink and the from-scratch audio
              decoders:
                aac/   bit reader, ADTS + AudioSpecificConfig parsers,
                       Huffman codebooks, ICS/TNS tools, MDCT/filterbank
                opus/  OpusHead, packet framing, range decoder, CELT/SILK
                       modules, packet loss concealment
                mp3/   frame parsing, bit reservoir, Huffman, IMDCT and the
                       synthesis filterbank
                flac/  STREAMINFO, frame headers, constant/verbatim/fixed/LPC
                       subframes, Rice residuals
                ac3/   syncframe syntax, coupling, bit allocation, mantissas,
                       KBD-window IMDCT
```

HEVC is the only codec backed by an external library: `KvzdecDecoder` wraps
the kvzdec C API, and the renderer, queue and stats only ever see
`decode::Frame`. The backend is optional. When the kvzdec submodule is not
checked out, CMake defines `PLAYER_HAS_KVZDEC=0`, the wrapper compiles to a
stub that refuses to open, and the engine treats an HEVC track like any other
codec without a decoder: it is named and the audio plays.

`decode::Frame` is the single currency between a decoder and everything
downstream: three planar sample arrays plus geometry, a conformance window and
the presentation metadata. A frame either owns its sample storage or adopts
storage belonging to a backend decoder, released through a small payload hook.
This is why kvzdec types do not appear in the renderer, the frame queue, the
statistics collector or the UI.

The demux + decode pipeline runs on a dedicated thread and fills a bounded
frame queue (about two seconds, capped by a memory budget). The UI thread
advances a monotonic clock and displays frames as their presentation
timestamps become due, dropping frames that are already late.

Audio follows the same pattern with its own buffer: the pipeline decodes the
selected track (AAC, Opus, MP3, FLAC or AC-3) up to 2 seconds ahead of the
media clock into a timestamped PCM buffer, and a feeder (called by both the
pipeline and the UI thread, so a busy video decode cannot starve it) moves
chunks into the sink to keep the device queue around 400 ms ahead of the video
position. Chunks that fall behind the clock are dropped, which keeps audio
locked to the video after stalls and seeks. Volume and mute are applied as a
software gain when chunks leave the buffer.

The player runs without a console window; messages are collected in a
thread-safe log buffer and can be viewed in the in-app log console
(`View -> Log` or `L`). CLI modes (`--info`, `--dump-yuv`, `--help`) keep the
console so their output can be redirected.
