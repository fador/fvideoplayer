# MPEG-1 and MPEG-2 video decoding

The decoder (`src/decode/mpeg12/`) covers ISO/IEC 11172-2 (MPEG-1) and
ISO/IEC 13818-2 / H.262 (MPEG-2, Main Profile at Main Level). It is written from
scratch: no external codec library is involved, and no new dependency was added
to the build. The two standards share one implementation because MPEG-2 is a
strict superset; a sequence extension or a picture coding extension switches it
into MPEG-2 mode, and `Decoder::standard()` reports which was seen.

## What is implemented

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

## What is not implemented

- **Dual prime prediction** (`motion_type == 11`). A picture that uses it is
  skipped with a message rather than decoded with the wrong vectors.
- **Profiles above Main**: `chroma_format` other than 4:2:0, and the SNR,
  spatial and high profiles. These are rejected explicitly.
- **Scalability**: the sequence scalability extension and its layers are not
  parsed.
- **MPEG-2 features deliberately absent**: there are no flat/non-flat quantiser
  matrix tests and no `q_scale_type` interaction with the DC predictor beyond
  what the standard specifies.
- **Program stream audio formats**: the first audio PES stream is reassembled
  and split into MPEG-1/2 Layer II or Layer III frames, so `.vob` and `.mpg`
  files play with MP2 or MP3 audio. Other audio codecs (AC-3, DTS) would each
  need their own elementary stream framing.

## Containers

MPEG-1 and MPEG-2 are reachable from every container the player can already
open. The container reports which of the two it is, and the demuxer decides for
a raw elementary stream by looking for a sequence extension:

| Container | MPEG-1 marker | MPEG-2 marker |
| --- | --- | --- |
| Raw elementary stream | `.mpg`, `.mpeg`, `.m1v`, `.mpe`, `.vcd` | `.m2v`, `.m2p`, `.mpv`, or a sequence extension in the stream |
| Program stream | `.vob`, `.evo`, or a leading pack or system header | same |
| AVI | `mpg1`, `MPG1`, `PIM1`, `BW10`, `XMPG` | `mpg2`, `MPG2`, `MPEG`, `PIM2`, `VCR2`, `DVR `, `MMES`, `LMP2`, `slif`, `EM2V`, `M701`..`M705`, `mpgv` |
| Matroska | `V_MPEG1` | `V_MPEG2` |
| MP4 / MOV | `mp4v` with an `esds` object type of `0x01` | `mp4v` with an `esds` object type of `0x60`..`0x65` |

A raw stream is split into pictures at picture headers, and the sequence and
group headers that precede a picture are folded into that picture's sample,
because the decoder needs them and there is no separate sample for them. A
sequence or group header therefore ends the picture before it, which is what
makes every sample independently decodable from a keyframe.

A **program stream** (`MPEG-PS`) is walked pack by pack: pack headers, system
headers and PES packets are skipped by their declared length, and the video
PES payloads are reassembled into one elementary stream before being split into
pictures exactly like a raw stream. PES packets that split a picture, and video
PES packets whose declared length is zero, are both handled. Each picture takes
the PTS of the PES packet it started in, and the frame rate is derived from the
PTS spacing when the stream provides one. Both the MPEG-1 (`01` marker with a
STD buffer field) and MPEG-2 (`10` marker with flags and a header length) PES
header forms are parsed.

`mp4v` with an object type of `0x20` (MPEG-4 Visual) or `0x21` (H.264) is kept
as an unsupported video track: it is named in the UI and the log, no video
samples are indexed, and the audio plays. A video codec with no decoder is
never fed to a decoder that cannot handle it.

The same fallback applies to every container. When the first video track is not
one of HEVC, MPEG-1, MPEG-2 or Sorenson Spark, the demuxer keeps the track
description (its codec id, size and, when the container has one, the codec
name), indexes no video samples, and exposes the audio tracks as usual. The
engine then logs `Video codec X has no decoder; playing audio only`, the
stats show the codec name, and the transport and seeking follow the audio
timeline. A file with an unsupported video track and no audio is rejected with
a message naming the codec. Verified with ffmpeg-produced H.264 files in
Matroska, MP4, AVI, FLV and MPEG-PS: all five open, report the codec, and play
their AAC, MP3 or MP2 audio end to end with no dropped or late packets.

## Verification

The tables were transcribed by hand from ISO/IEC 11172-2 and then
cross-checked programmatically against an independent implementation, which
caught two transcription errors: a misaligned run/level magnitude table and six
transposed `macroblock_type` codewords. `tests/test_mpeg1.cpp` additionally
checks that every code table is prefix free and free of duplicate codewords,
that the zig-zag table is a permutation, and that the decoder reproduces a
hand-computed flat-grey intra picture pixel for pixel - including when the
stream is fed one byte at a time, which splits every header.

Real MPEG-2 streams are verified against ffmpeg (about 47-49 dB luma PSNR on
a 640x480 encode and a 240 frame capture, the float-versus-integer IDCT
rounding); see [verification.md](verification.md) for the block syntax bugs
that comparison uncovered.
