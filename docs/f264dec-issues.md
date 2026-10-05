# f264dec integration notes

The player includes [f264dec](https://github.com/fador/f264dec) as a submodule
(`external/f264dec`, now pinned at `cd55c2a`) for H.264/AVC decoding.

The decoder is correct in every mode that was checked:

- The upstream conformance suite passes 35/35 streams (`tests/run_tests.py`).
- The upstream memory-API test (`tests/test_memory_api.cpp`) decodes
  `base_cavlc_slices.264` in memory and matches the reference MD5 bit-exactly.
- An incremental, player-style feed (one access unit pushed at a time with one
  AU of lookahead, pictures consumed as they come out) decodes a generated
  352x288 stream 50/50 frames with PSNR infinite against ffmpeg.

The API issues that originally blocked the integration were fixed upstream in
`42f0587`, `2ba13be` and `cd55c2a`. What remains is listed at the end.

## Resolved by `cd55c2a`

| # | Issue | Resolution |
| --- | --- | --- |
| 1 | No in-memory input | `f264_config.memory_input` plus `f264_decoder_push()`; the reader consumes the pushed Annex B buffer |
| 2 | No pictures without an output file | `write_out_picture()` now always materializes the picture; only the `write()` calls are guarded by `p_out >= 0` |
| 3 | No consume-once picture API | `f264_decoder_get_picture()` hands out the first valid node and clears `bValid` |
| 4 | Flush returned only one picture | `f264_decoder_flush()` drains one picture per call and returns `F264_EOS` when empty; call it in a loop |
| 5 | One AU of lookahead needed | Documented in `f264dec.h` |
| 6 | `error()` called `exit()` | `error()` now calls an optional `f264_error_callback` or prints, then returns |
| 7 | Frame-number gaps were fatal | The gap path conceals or calls `fill_frame_num_gap()` instead of exiting |
| 8 | Single global decoder instance | Documented, and a second `decoder_open()` returns NULL while one is active |
| 9 | Output pictures alias decoder memory | Documented in `f264dec.h`: pixels are valid until the next decode/get_picture/flush call |
| 11 | `add_subdirectory` built the CLI and wired CTest | `F264DEC_BUILD_CLI` / `F264DEC_BUILD_TESTS` default to top-level only; `f264dec::f264dec` alias target |
| 12 | `config_parse` missed fields | `memory_input`, `deblock_enable` and `file_format` are parsed |
| 14 | CLI counted every `F264_OK` as a frame | It only counts when a picture is returned, and drains the flush loop |

## Open issues

### 1. `decoder_close` still writes files into the current working directory

`CloseDecoder()` calls `Report()` (`src/f264dec.cpp:1269`), which opens
`log.dec` and appends to `dataDec.txt` in the process working directory
(`src/f264dec.cpp:467-616`), even with `silent = 1`. Running the tests leaves
both files next to the binary. A library should not create files as a side
effect of closing; this should be behind an opt-in (or only in the CLI).

### 2. `decoder_decode` can return the last picture together with `F264_EOS`

When the final frame is decoded at end of input, `f264_decoder_decode()`
returns `F264_EOS` and still fills `*pic_out` with that picture. A caller that
stops at EOS (as the upstream CLI and `test_memory_api.cpp` do) loses the last
frame; a caller must process `pic_out` before checking the return code. This is
easy to miss and should either be documented or the last picture should be
returned with `F264_OK` and the following call with `F264_EOS`. Verified while
testing the incremental feed: dropping the EOS picture produced 49/50 frames,
processing it produced 50/50.

### 3. `get_one_avail_dec_pic_from_list` does not link a node into an empty list

The NULL `pPrior` dereference is fixed (`if (pPrior) pPrior->pNext = pPic;`),
but when the list is empty the freshly allocated node is not linked anywhere
and the caller keeps using its own pointer. `pDecOuputPic` is pre-allocated so
this is latent, but a consume API that empties the list can reach it.

## Calling conventions for the player wrapper

- Feed `f264_decoder_push()` Annex B bytes (length-prefixed samples must be
  converted, and the SPS/PPS from an avcC record pushed before the first
  sample). It returns the number of bytes pushed, not `F264_OK`.
- Push at least one access unit ahead of the frame being pulled: the decoder
  needs the next picture's first slice to detect the boundary.
- `f264_decoder_decode(dec, &pic)` first hands out an unconsumed picture and
  otherwise decodes one access unit; `pic` can be NULL while reordering.
- `f264_decoder_get_picture(dec)` drains additional pictures without decoding.
- `f264_decoder_flush(dec, &pic)` must be called repeatedly until it returns
  `F264_EOS`.
- Copy the pixels out of the returned `f264_picture` before the next call; the
  planes alias decoder memory.
- Set `f264_config.error_cb` so decode errors do not print to stderr, and
  expect `decoder_close()` to create `log.dec` / `dataDec.txt` in the working
  directory until open issue 1 is fixed.
