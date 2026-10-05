# f264dec integration notes

The player includes [f264dec](https://github.com/fador/f264dec) as a submodule
(`external/f264dec`, pinned at `119e5b7`) for H.264/AVC decoding.

## Verification

- The upstream conformance suite passes 35/35 streams
  (`tests/run_tests.py`).
- The upstream memory-API test (`tests/test_memory_api.cpp`) decodes
  `base_cavlc_slices.264` in memory and matches the reference MD5 bit-exactly.
- An incremental, player-style feed (one access unit pushed at a time with one
  AU of lookahead, pictures consumed as they come out) decodes a generated
  352x288 stream 50/50 frames at PSNR infinite against ffmpeg.

## History

The first API revision (`7d9930d`) could only read a whole file and only
produced pictures when an output file was open, so a demuxer-driven player
could not use it. The issues found then were fixed upstream in `42f0587`
(in-memory push input, picture materialization, consume-once get_picture and a
drainable flush), `2ba13be` (error concealment instead of a fatal frame-number
gap), `cd55c2a` (no `exit()` in the library, instance guard, embed-friendly
CMake, missing config keys) and `119e5b7` (no log files on close, the final
picture returned with `F264_OK` instead of together with `F264_EOS`, empty
picture-list linking). All of them are resolved.

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
- `f264_config.error_cb` is set so decode errors do not print to stderr.

## Timestamps

H.264 elementary streams carry no presentation timestamps and f264dec's
`f264_picture` does not forward one, so the wrapper derives the output
timeline: the first pushed sample's PTS is the base and each following
display-order picture advances by the smallest positive PTS step seen on the
input. Constant-frame-rate streams are exact; variable-frame-rate streams are
approximated.
