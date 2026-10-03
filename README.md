# HEVC Player

A video player built around an external HEVC/H.265 decoder plus a from-scratch
MPEG-1 decoder.

- **Decoders**: kvzdec (git submodule) for HEVC through its public C API, and a
custom MPEG-1 video decoder written from scratch. Both produce the same
codec-neutral decoded frame, so the renderer, queue and statistics do not know
  which one ran.
  See [MPEG-1 video decoding](#mpeg-1-video-decoding) for details
- **Containers**: native demuxers for MP4, Matroska (MKV) and AVI - no
libavformat; raw `.hevc` and `.mpg` / `.mpeg` / `.m1v` elementary streams, and
  raw `.mp3` / `.flac` files
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
  demux/      container demuxers (MP4, MKV, AVI, raw HEVC, raw MPEG-1, raw
              MP3, raw FLAC) + hvcC and esds parsers
  subtitle/   SRT/ASS/SSA/WebVTT/MicroDVD parsers and cue lookup
  decode/     codec-neutral Frame, decode backends (KvzdecDecoder,
              mpeg12/ from-scratch MPEG-1 and MPEG-2), NAL handling, decode-to-YUV
              verification mode
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
selected track (AAC, Opus, MP3 or FLAC) up to 2 seconds ahead of the media clock
into a timestamped PCM buffer, and a
feeder (called by both the pipeline and the UI thread, so a busy video decode
cannot starve it) moves chunks into the sink to keep the device queue around
400 ms ahead of the video position. Chunks that fall behind the clock are
dropped, which keeps audio locked to the video after stalls and seeks. Volume
and mute are applied as a software gain when chunks leave the buffer.

## MPEG-1 and MPEG-2 video decoding

The decoder (`src/decode/mpeg12/`) covers ISO/IEC 11172-2 (MPEG-1) and
ISO/IEC 13818-2 / H.262 (MPEG-2, Main Profile at Main Level). It is written from
scratch: no external codec library is involved, and no new dependency was added
to the build. The two standards share one implementation because MPEG-2 is a
strict superset; a sequence extension or a picture coding extension switches it
into MPEG-2 mode, and `Decoder::standard()` reports which was seen.

### What is implemented

- **Headers**: sequence header including the optional quantisation matrices and
  VBV size, group of pictures, picture header and slice header
- **Pictures**: all four coding types - I, P, B and the reserved D - decoded
  in 4:2:0 at 8 bits
- **Slice layer**: multiple slices per picture, slices spanning macroblock
  rows, `macroblock_skip_run` with escape and stuffing, and the slice
  terminator. `macroblock_address_increment` and `macroblock_skip_run` share
  the same code table, so a single counter drives both
- **Macroblock layer**: `macroblock_type` for I, P and B pictures, including
  the P-picture types that imply an all-zero motion vector
- **Block layer**: all variable-length code tables (Tables B-1 to B-14),
  matched longest codeword first because the `macroblock_type` tables are not
  prefix codes
- **Inverse quantisation**: the MPEG-1 rules, including the intra / non-intra
  split (`/16` versus `(2 * level + 1) / 32`) and the requirement that every
  reconstructed coefficient is odd
- **Transform**: separable 8x8 floating-point inverse DCT
- **Motion compensation**: full-sample and half-sample prediction, half-sample
  rounding always upwards as the standard requires, and chroma vectors derived
  by halving the luma vector
- **Reference handling**: a two-slot reference set where B pictures never
  become references, with I and P pictures emitted only once the following
  picture has been decoded, and B pictures held until the I or P picture that
  precedes them can be displayed

MPEG-2 adds the following, all of which are implemented:

- **Sequence extension**: profile/level, `progressive_sequence`, `chroma_format`
  and the size and frame-rate extensions
- **Picture coding extension**: `f_code`, `intra_dc_precision`,
  `picture_structure`, `frame_pred_frame_dct`, `concealment_motion_vectors`,
  `q_scale_type`, `intra_vlc_format`, `alternate_scan`, `repeat_first_field`
  and `progressive_frame`
- **Field pictures**: top and bottom fields are decoded into one full-height
  buffer as their own parity lines and emitted as a single frame once both
  fields are present, so interlaced material displays correctly
- **Motion types**: frame prediction, field prediction (per 16x8 half, with
  `field_select`), 16x8 prediction in field pictures, and field DCT
  (`dct_type`), which writes the two luma halves to the two parities of the
  same eight-line span
- **Coding tools**: the non-linear quantiser scale (Table 7-6), the alternate
  (vertical) scan, `intra_vlc_format` with the Table B-14 coefficient codes,
  12-bit escape levels, the precision-scaled DC predictor, and IDCT mismatch
  control
- **Concealment motion vectors** are parsed (and discarded, since the block is
  intra anyway)

### What is not implemented

- **Dual prime prediction** (`motion_type == 11`). A picture that uses it is
  skipped with a message rather than decoded with the wrong vectors.
- **Profiles above Main**: `chroma_format` other than 4:2:0, and the SNR,
  spatial and high profiles. These are rejected explicitly.
- **Scalability**: the sequence scalability extension and its layers are not
  parsed.
- MPEG-2 features deliberately absent: there are no flat/non-flat quantiser
  matrix tests and no `q_scale_type` interaction with the DC predictor beyond
  what the standard specifies.
- **MPEG program streams**: `.mpg` files from DVD rippers are program streams
  rather than elementary streams, and the program stream demuxer is not written
  yet. A program stream is detected as an elementary stream and reports
  "no MPEG-1 pictures found".

### Containers

MPEG-1 and MPEG-2 are reachable from every container the player can already
open. The container reports which of the two it is, and the demuxer decides for
a raw elementary stream by looking for a sequence extension:

| Container | MPEG-1 marker | MPEG-2 marker |
| --- | --- | --- |
| Raw elementary stream | `.mpg`, `.mpeg`, `.m1v`, `.mpe`, `.vcd` | `.m2v`, `.m2p`, `.mpv`, or a sequence extension in the stream |
| AVI | `mpg1`, `MPG1`, `PIM1`, `BW10`, `XMPG` | `mpg2`, `MPG2`, `MPEG`, `PIM2`, `VCR2`, `DVR `, `MMES`, `LMP2`, `slif`, `EM2V`, `M701`..`M705`, `mpgv` |
| Matroska | `V_MPEG1` | `V_MPEG2` |
| MP4 / MOV | `mp4v` with an `esds` object type of `0x01` | `mp4v` with an `esds` object type of `0x60`..`0x65` |

A raw stream is split into pictures at picture headers, and the sequence and
group headers that precede a picture are folded into that picture's sample,
because the decoder needs them and there is no separate sample for them.
`mp4v` with an object type of `0x20` (MPEG-4 Visual) or `0x21` (H.264) is
rejected with a clear message rather than decoded incorrectly.

### Verification

The tables were transcribed by hand from ISO/IEC 11172-2 and then
cross-checked programmatically against an independent implementation, which
caught two transcription errors: a misaligned run/level magnitude table and six
transposed `macroblock_type` codewords. `tests/test_mpeg1.cpp` additionally
checks that every code table is prefix free and free of duplicate codewords,
that the zig-zag table is a permutation, and that the decoder reproduces a
hand-computed flat-grey intra picture pixel for pixel - including when the
stream is fed one byte at a time, which splits every header.

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

Not implemented: the 3D rotations `\frx` and `\fry` are ignored, and a vector
`\clip` uses the box of its points, because the draw list only clips to
rectangles. `\t` animates the state fields (size, scale, colours, alphas,
rotation, shear, spacing) but not clips or drawings.



The player runs without a console window; messages are collected in a
thread-safe log buffer and can be viewed in the in-app log console
(`View -> Log` or `L`). CLI modes (`--info`, `--dump-yuv`, `--help`) keep
the console so their output can be redirected.

## Verification

The HEVC decode path was checked bit-exact against the HM reference outputs of
the kvzdec conformance suite for raw, MP4, MKV and AVI inputs (8/10-bit,
4:2:0/4:4:4, tiles/WPP). `--dump-yuv` performs a synchronous decode of the
whole file for this purpose.

The MPEG-1/MPEG-2 decoder has no reference bitstream in the repository, so it
is covered by unit tests instead: the VLC tables, inverse quantisation, IDCT
normalisation and motion compensation are checked against hand-computed values,
and whole pictures are decoded from synthesised bitstreams and compared pixel
for pixel. Both code table sets were generated from, or cross-checked against,
an independent implementation rather than transcribed by hand. This is a weaker
guarantee than the HEVC path, and the gap is not uniform:

- MPEG-1 I pictures are verified end to end, including byte-at-a-time input.
  P and B pictures are verified only against synthetic streams.
- MPEG-2 frame pictures are verified end to end for the intra path, including
  all four `intra_dc_precision` values and the alternate scan, `intra_vlc_format`
  and `q_scale_type` paths.
- MPEG-2 field pictures, field and 16x8 prediction, field DCT and B pictures
  are implemented but have **no end-to-end test at all**, because a synthetic
  stream exercising them would be a decoder of its own. Treat interlaced
  MPEG-2 as unproven.
- Dual prime is not implemented, and a picture that needs it is skipped.

`--dump-yuv` reports a decode as damaged if a slice does not parse cleanly, so
a bad result is at least visible.


## License

The player sources are provided as-is. Third-party components keep their own
licenses: kvzdec (3-clause BSD), SDL3 (zlib), Dear ImGui and ImPlot (MIT).





