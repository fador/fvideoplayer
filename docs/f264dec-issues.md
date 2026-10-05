# f264dec integration notes

The player includes [f264dec](https://github.com/fador/f264dec) as a submodule
(`external/f264dec`, pinned at `6291599`) for H.264/AVC decoding.

## Verification

- The upstream conformance suite passes 35/35 streams
  (`tests/run_tests.py`).
- The upstream memory-API tests (`tests/test_memory_api.cpp`) decode
  `base_cavlc_slices.264` and `high10.264` in memory and match the reference
  MD5s bit-exactly, checking the reported bit depth (8 and 10) as well.
- An incremental, player-style feed (one access unit pushed at a time with one
  AU of lookahead, pictures consumed as they come out) decodes a generated
  352x288 stream 50/50 frames at PSNR infinite against ffmpeg.
- 10-bit (High 10, yuv420p10le) content decodes bit-exactly through
  `--dump-yuv` and renders correctly: a screenshot of a 1280x720 10-bit MKV
  matches ffmpeg's BT.709 RGB conversion at 41 dB PSNR.

## History

The first API revision (`7d9930d`) could only read a whole file and only
produced pictures when an output file was open, so a demuxer-driven player
could not use it. The issues found then were fixed upstream in `42f0587`
(in-memory push input, picture materialization, consume-once get_picture and a
drainable flush), `2ba13be` (error concealment instead of a fatal frame-number
gap), `cd55c2a` (no `exit()` in the library, instance guard, embed-friendly
CMake, missing config keys) and `119e5b7` (no log files on close, the final
picture returned with `F264_OK` instead of together with `F264_EOS`, empty
picture-list linking).

Two more issues surfaced when playing real 10-bit content, both fixed in
`71092c6` and `aa13a3b`: `f264_picture.bit_depth` reported the on-disk storage
element size (16) instead of the coded depth (10), which made the renderer
treat 10-bit samples as 16-bit and show a near-black, green-tinted image, and
`silent = 1` still printed the profile banner and a per-frame progress line
because one path had the condition inverted and another checked a
never-initialized field.

`6291599` fixed a crash when seeking on streams that use the threaded frame
pipeline (CABAC, one slice per frame): closing the decoder freed the slices and
buffers of jobs that were still running, and each slot's shallow copy of the
`VideoParameters` kept a pointer to the master's macroblock-to-slice-group map,
which the master frees and regenerates for every picture. `CloseDecoder` now
waits for the pipeline, and each slot owns a copy of the map. All of them are
resolved.

## Calling conventions used by the player wrapper

- `f264_decoder_push()` takes Annex B bytes (length-prefixed samples must be
  converted, and the SPS/PPS from an avcC record pushed before the first
  sample). It returns the number of bytes pushed, not `F264_OK`.
- Push at least one access unit ahead of the frame being pulled: the decoder
  needs the next picture's first slice to detect the picture boundary.
- `f264_decoder_decode(dec, nullptr)` decodes one access unit without handing
  out a picture; `f264_decoder_get_picture(dec)` drains every picture that is
  ready. Using the decode call with a `pic_out` argument instead hands out
  pending pictures first and can decode without consuming an access unit, which
  makes the lookahead accounting ambiguous; the wrapper uses the
  decode-then-drain form.
- `f264_decoder_flush(dec, &pic)` must be called repeatedly until it returns
  `F264_EOS`.
- Copy the pixels out of the returned `f264_picture` before the next call; the
  planes alias decoder memory.
- `f264_picture.bit_depth` is the coded bit depth (8, 10, ...) while the plane
  samples always occupy `bit_depth > 8 ? 2 : 1` bytes; consumers scale the
  samples with the coded depth.
- `f264_config.error_cb` is set so decode errors do not print to stderr.

## Timestamps

H.264 elementary streams carry no presentation timestamps and f264dec's
`f264_picture` does not forward one, but the demuxer's samples do. The samples
arrive in decode order while pictures come out in display order, so the
wrapper keeps the pushed timestamps in a min-heap and gives each picture the
smallest one not handed out yet (`decode/DisplayPts.hpp`). This is exact for
constant and variable frame rate alike. An earlier version synthesized the
timeline as `base + index * smallest_positive_step`, which halved the rate on
streams with B-frames because the smallest positive decode-order difference is
two frame intervals.
