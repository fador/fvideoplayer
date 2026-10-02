# HEVC Player

A focused HEVC/H.265 video player built around the custom
[kvzdec](https://gitlab.tuni.fi/cs/ultravideo/hevc/kvzdec) decoder.

- **Decoder**: kvzdec (git submodule), wrapped through its public C API
- **Containers**: native demuxers for MP4, Matroska (MKV) and AVI — no
libavformat; raw Annex B `.hevc` streams and raw `.mp3` / `.flac` files
  (audio-only playback) are supported as well
- **GUI**: Dear ImGui + SDL3 with a custom OpenGL YUV -> RGB render path
- **Playback**: correct-FPS pacing with a ~2 second decode-ahead buffer and
  keyframe seeking
- **Statistics**: live decode performance, buffer level, bitrate and
  per-frame decoder information
- **Subtitles**: embedded Matroska text tracks (SRT, ASS/SSA) and MP4 timed
    text (tx3g), plus external
    SRT/ASS/SSA/WebVTT/MicroDVD files, selectable from the Subtitles menu and
  switchable on the fly; external files next to the video are discovered
  automatically
- **Audio**: container audio tracks are listed and selectable from the Audio
menu. AAC-LC, Opus (SILK/CELT/hybrid), MP3 and FLAC are decoded by
  from-scratch decoders (no external codec libraries) and played through an
  SDL3 float sink with software volume and mute; audio-only MP3 and FLAC files
  play directly.
  See [Audio decoding](#audio-decoding) for details

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
  demux/      container demuxers (MP4, MKV, AVI, raw HEVC, raw MP3, raw FLAC)
            + hvcC parser
              parser
  subtitle/   SRT/ASS/SSA/WebVTT/MicroDVD parsers and cue lookup
  decode/     kvzdec wrapper, NAL handling, decode-to-YUV verification mode
  playback/   pipeline thread, frame queue, media clock
  render/     OpenGL YUV -> RGB renderer (8/10-bit, 4:2:0/4:2:2/4:4:4)
  stats/      thread-safe statistics collector
  audio/      PCM types, volume/mute gain, AVX2 kernels with CPUID dispatch,
              timestamped audio buffer, SDL3 sink and the from-scratch audio
              decoders (aac/: bit reader, ADTS + AudioSpecificConfig parsers,
              Huffman codebooks, ICS/TNS tools, MDCT/filterbank; opus/:
              OpusHead, packet framing, range decoder, CELT/SILK modules,
packet loss concealment; mp3/: frame parsing, bit reservoir,
            Huffman, IMDCT and the synthesis filterbank; flac/: STREAMINFO,
            frame headers, constant/verbatim/fixed/LPC subframes, Rice
            residuals)
```

The demux + decode pipeline runs on a dedicated thread and fills a bounded
frame queue (about two seconds, capped by a memory budget). The UI thread
advances a monotonic clock and displays frames as their presentation
timestamps become due, dropping frames that are already late.

Audio follows the same pattern with its own buffer: the pipeline decodes the
selected track (AAC, Opus, MP3 or FLAC) up to 2 seconds ahead of the media clock
into a timestamped PCM buffer, and a
feeder (called by both the pipeline and the UI thread, so a busy video decode
cannot starve it) moves chunks into the sink to keep the device queue around
400 ms ahead of the video position. Chunks that fall behind the clock are
dropped, which keeps audio locked to the video after stalls and seeks. Volume
and mute are applied as a software gain when chunks leave the buffer.

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
- AVX2 kernels (MDCT/DFT, dequantization table lookup, windowing, M/S,
  intensity and gain) with CPUID dispatch; the scalar path stays as fallback
  and both are cross-checked by unit tests

Not supported: SBR/PS (HE-AAC), AAC Main/LTP/SSR, 960-sample frames,
coupling elements (CCE) and program config elements (PCE). Such tracks are
reported once in the log and skipped. Audio access units are read from
Matroska, MP4 and AVI; PCM audio is listed but has no decoder yet.

The decoder was verified sample-exact against a reference decoder for mono,
stereo, 5.1, short-window transients, PNS/intensity and all 12 sampling
frequencies; unit tests cover the codebook tables, the KBD window values and
full frame decoding from both ADTS and raw access units.

### Opus

The Opus decoder is also written from scratch. Implemented:

- OpusHead parsing (channel mapping families 0 and 1, pre-skip, output gain)
- Packet framing: TOC parsing (SILK/hybrid/CELT, bandwidth, frame size),
  codes 0..3 with CBR, VBR and padding
- The range decoder from RFC 6716 section 4.1, verified symbol by symbol
  against vectors produced by the reference encoder
- CELT constant tables (energy bands, allocation, probability model, MDCT
  window, logN, ICDFs) generated from the RFC tables
- CELT PVQ (pyramid vector quantization) decoding, verified against pulse
  vectors produced by the reference encoder
- CELT band energy decoding (coarse with prediction, fine and final bits),
  verified against reference-encoded energy vectors
- CELT bit allocation (dynalloc boosts, skip/intensity/dual decisions and
  the fine-energy versus PVQ split), verified against reference-encoded
  allocations for mono/stereo and several frame sizes
- CELT spreading rotation and vector normalization, verified against
  reference outputs
- CELT band decoding (PVQ integration, recursive band splitting, theta
  angles, folding and noise fill, stereo mid/side, time/frequency
  resolution changes), verified against reference round trips for mono
  and stereo, long and transient frames, all spread levels and intensity
  stereo
- CELT denormalisation and anti-collapse, verified against reference
  outputs
- CELT inverse MDCT (kiss_fft radix-2/3/4/5 transforms with shared
  twiddle tables, pre/post rotation and the integrated overlap window),
  verified against reference outputs for every frame size and stride
  combination
- CELT pitch comb filter (with crossfade and tap presets) and
  de-emphasis, verified against reference outputs
- CELT frame decoding: header parsing (silence, post-filter, transient,
  intra, spread, dynalloc, trim), band energy history, PVQ content,
  synthesis and the pitch post-filter, with inter-frame state. CELT-only
  Opus packets now decode to PCM, verified against reference-encoded and
  reference-decoded frame sequences (mono/stereo, several frame sizes)
- SILK NLSF decoding (codebook unpacking, predictive residual
  dequantization, stabilization), verified bit-exactly against the
  reference for both narrowband/mediumband and wideband codebooks
- SILK side information decoding (signal type, gains, NLSF indices,
  interpolation, pitch lags and contours, LTP gains and scaling, seed),
  verified bit-exactly against the reference for all sampling rates,
  frame sizes and conditional coding modes
- SILK gains, pitch lags and excitation (rate levels, shell coding, LSB
  extensions, signs), verified bit-exactly against the reference for all
  signal types, quantizer offsets and frame lengths
- SILK NLSF to LPC conversion (cosine approximation table, polynomial
  expansion, coefficient fitting with bandwidth expansion, stability
  check and interpolation), verified bit-exactly against the reference
- SILK parameter decoding: gain dequantization, interpolated LPC sets,
  LTP coefficients and scaling, verified bit-exactly against the
  reference for all rates, frame sizes, signal types and loss states
- SILK core synthesis: excitation offsets and sign randomization, LTP
  rewhitening and prediction, LPC synthesis with gain adjustment and the
  voiced-to-unvoiced loss transition, verified bit-exactly against the
  reference for voiced and unvoiced frames at all rates
- SILK stereo (predictor decoding and interpolated mid/side to
  left/right) and the decoder resampler (allpass 2x upsampling plus FIR
  interpolation from 8/12/16 kHz to 48 kHz), verified bit-exactly
  against the reference including resampler state across calls
- SILK frame decoding and Opus wiring: payload parsing (VAD and LBRR
  flags, LBRR skipping), per-frame parameter and excitation decoding,
  stereo unmixing, resampling and interleaving. SILK-only Opus packets
  now decode to PCM, verified sample-exactly against reference-encoded
  and reference-decoded packets (mono/stereo, 10/20/40 ms, NB/WB)
- Hybrid Opus packets: the wideband SILK layer plus the CELT high band
  (with the bandwidth-dependent end band), the redundancy flag parsing
  and the SILK + CELT sum. SILK-only packets are exact; hybrid output is
  within 1% energy of the reference because the reference's static mode
  uses precomputed tables that differ by a few ULPs from the
  runtime-computed ones
- OpusHead pre-skip trimming (the decoder ramp-up samples are dropped,
  spanning packets when a frame is shorter than the pre-skip) and the
  output gain applied to the decoded samples
- CELT packet loss concealment: pitch-based extrapolation of the last
  frame for short bursts, noise shaped by the decaying band energies for
  longer losses
- SILK packet loss concealment and comfort noise, including the hybrid
  case where the CELT layer conceals the band above 8 kHz with noise

CELT-only, SILK-only and hybrid Opus tracks play, with packet loss
concealed in all three modes.

### MP3

The MP3 (MPEG-1/2/2.5 Layer III) decoder is a port of the public-domain
minimp3 scalar path. Implemented:

- Frame headers, the bit reservoir, side info and scalefactor decoding
  (MPEG-1 and MPEG-2/2.5 forms)
- Huffman decoding with linbits, requantization, M/S and intensity stereo,
  alias reduction
- Long/short/mixed block IMDCT and the polyphase synthesis filterbank
- MPEG-1, MPEG-2 and MPEG-2.5 at all sample rates, mono and stereo

Raw `.mp3` files play directly (ID3v2/v1 tags skipped, Xing/Info and VBRI
headers for the duration, frame-accurate seeking). Test vectors decoded by
the reference match bit-exactly across eight configurations, and an
end-to-end test opens a tagged file, demuxes every frame and plays it
through the engine.

### FLAC

The FLAC decoder follows RFC 9639 and is written from scratch. Implemented:

- STREAMINFO parsing and frame indexing with byte-exact frame lengths taken
  from the frame footer CRC-16
- Frame headers: fixed block sizes, coded frame numbers, all sample rate and
  bit depth codes, uncommon block sizes and rates
- Constant, verbatim, fixed predictor (orders 0-4) and LPC subframes
  (orders 1-32), wasted bits per sample
- Residual coding: Rice partitions with 4-bit and 5-bit parameters, escape
  partitions, mid/side, left/side and right/side decorrelation

Raw `.flac` files play directly; the demuxer indexes every frame for
sample-accurate seeking. Seven reference-encoded streams (16/24-bit, mono to
6 channels, 32-96 kHz, noise and stereo decorrelation) decode bit-exactly,
and an end-to-end test plays a file through the engine.

### MP4 audio and subtitles

The MP4 demuxer reads every track the player can act on, not just the video:

- One audio track per `soun` handler, with the codec fourcc, channel count,
  sample rate and language from `mdhd`. Access units come from the same sample
  tables as the video, and seeking moves each audio cursor to the packet that
  covers the playhead so playback follows a seek in either direction.
- Decoder configuration is taken from the sample entry: the
  AudioSpecificConfig out of the `esds` descriptor chain for `mp4a`, and for
  Opus the `dOps` box, which stores the OpusHead fields big endian, so they are
  swapped while the header is rebuilt. Reading them verbatim turned a pre-skip
  of 312 into 14337 and dropped the first quarter second of audio.
- Text tracks with a `tx3g` sample entry are reported as subtitles and their
  samples are read as timed text blocks, which the subtitle layer turns into
  cues.

Verified against ffmpeg-produced files (HEVC with AAC, with Opus, with two
audio tracks and with `mov_text` subtitles): the audio decodes without errors
and the cues match the source, next to unit tests built from minimal in-test
MP4 files.

### AVI audio

AVI audio streams are read from the same chunk tables as the video, using
`idx1` when the file has one and scanning the `movi` list otherwise:

- Codec, channel count and sample rate come from the WAVEFORMATEX in `strf`.
  MPEG audio and AAC map to the decoders that already exist; Opus is recognised
  by its format tag and takes the WAVEFORMATEX trailing bytes as its
  OpusHead. PCM is listed with its raw tag and reported as having no decoder.
- AVI stores no timestamp per chunk, so presentation times accumulate from the
  `strh` scale and rate, which describe one chunk.
- Each audio track keeps its own cursor and `seek()` moves them along with the
  video one, so audio follows a seek in either direction.

Verified against ffmpeg-produced AVI files with MP3 and with AAC audio: the
audio decodes without errors, and PCM is reported as unsupported rather than
mistaken for silence. Unit tests cover the track description, packet reading
with and without `idx1`, and the seek behaviour.

### Subtitle rendering

ASS/SSA files are more than timed text, and the renderer follows the parts that
carry meaning:

- The style table and the script resolution are read from the file or from the
  Matroska CodecPrivate, so each line gets its font size, its three colours and
  its alignment. The `\an` and `\pos` overrides place a line in script
  coordinates, which is what typeset karaoke relies on. A line without `\pos`
  sits inside its margins at the edge its alignment names.
- Karaoke runs (`\k`, `\K`, `\kf`, `\ko`) become timed syllables. A run is
  drawn in the style's secondary colour until it is sung and in the primary
  colour after, so the highlight follows the music.
- The animation tags are evaluated per frame: `\t` transitions interpolate
  size, scale, colours and alphas between two times, `\fad` and `\fade` ramp
  the opacity, `\move` slides the line, and `\alpha`/`\1a`-`\4a` set
  transparency. `\bord` and `\shad` size the outline and the drop shadow, and
  `\blur` is drawn as a soft halo, since a text draw list cannot blur.
- Every line that is active is drawn, not just the newest one: karaoke
  typesetting stacks a fade-in line, a beat flash and a fade-out line on the
  same spot.
- Comment lines are skipped, since Aegisub keeps its karaoke templates there.
- A CJK font is loaded from the system (Yu Gothic, MS Gothic, Meiryo or
  YaHei) because the built-in font has no Japanese glyphs. It is used for
  subtitles only, so the interface keeps its normal size.
- The font a style names is used when the system has it (Monotype Corsiva,
  Arial, Times New Roman and friends). This matters for typeset karaoke: the
  syllables are positioned for the metrics of that font, and a wider
  substitute makes them collide. Anything missing falls back to the CJK face.

Not implemented: `\clip`, `\iclip`, `\org` and vector drawing (`\p`), and the
bold, italic, underline and strikeout flags are ignored because the renderer
draws with plain glyphs. A non-uniform `\fscx`/`\fscy` pair is approximated by
the vertical factor, since the text draw list can only scale uniformly.



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





