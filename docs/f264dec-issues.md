# f264dec integration notes

The player includes [f264dec](https://github.com/fador/f264dec) as a submodule
(`external/f264dec`, pinned at `7d9930d`) for H.264/AVC decoding. In its
standalone, file-based mode the decoder is correct: `tests/streams/
base_cavlc_slices.264` decodes bit-exactly against ffmpeg (17/17 frames,
PSNR infinite), and a generated 50 frame 352x288 stream also matches
bit-exactly.

The public API, however, cannot be driven by a container demuxer that feeds
one access unit at a time. The issues below were found while integrating it;
they are ordered by how much they block the integration. A work-in-progress
patch that addressed issues 1-5 was used for verification and is kept outside
the repository; it is not part of the submodule.

## Blockers

### 1. No in-memory input path

`f264_decoder_open()` calls `OpenDecoder()`, which always calls
`open_annex_b(cfg->infile, ...)` and reads NAL units with `read()` on the file
descriptor (`src/f264dec.cpp:1157`, `src/annexb.cpp:295`). `f264_config` has
only `infile` (`src/f264dec.h:90`). There is no way to push Annex B bytes or
NAL units, so a player that demuxes samples from MP4/Matroska/AVI/FLV cannot
use the library without round-tripping through temporary files.

Suggested API, mirroring kvzdec's `decoder_push_nal`: a `memory_input` config
flag plus `f264_decoder_push(dec, data, size)` that appends Annex B bytes to a
growable buffer, and a reader that consumes it. Note that the read cursor must
not be cached across pushes: any `realloc` of the buffer invalidates a pointer
held from a previous call (this was hit during development).

### 2. Decoded pictures are not produced without an output file

`write_out_picture()` returns immediately when no output file is open
(`src/output.cpp:356`: `if (p_out == -1) return;`). With `cfg->outfile` empty,
no picture is ever copied into `pDecOuputPic`, so `f264_decoder_decode()` fills
`*pic_out` only with NULL. The CLI is unaffected because the decoder writes YUV
internally, but an API consumer gets no pixels at all. The `write()` calls
inside the function are already guarded by `p_out >= 0`, so the fix is to
always materialize the picture in the output list and guard only the file
writes. (The YUV400 branch has two unguarded `write()` calls that need the same
guard.)

### 3. No way to consume each output picture exactly once

`pDecPic->bValid` is cleared only when writing to a file
(`src/output.cpp:492`: `if (p_out >= 0) pDecPic->bValid = 0;`). Without an
output file the nodes stay valid forever, so `get_one_avail_dec_pic_from_list()`
never reuses them (the list grows without bound) and `f264_decoder_decode()`
keeps returning the same stale head picture - the first frame - on every call.
There is no signal that a returned picture is new. A `f264_decoder_get_picture()`
that returns the first valid node and marks it consumed would fix both the
reuse and the "is this new" problem.

### 4. Flush can only return one of the remaining pictures

`f264_decoder_flush()` calls `FinitDecoder()` -> `flush_dpb()`, which outputs
every picture left in the DPB into the `pDecOuputPic` list, but the API fills
`*pic_out` from the head only (`src/f264dec.cpp:1559`). A caller cannot drain
the rest; the CLI gets away with it because it writes internally. The same
consume API as issue 3 solves this.

### 5. One access unit of lookahead is needed to finish a frame

`decode_one_frame()` reads slices until it sees the first VCL NAL of the
*next* picture (`is_new_picture()`, `src/image.cpp:817`); only then does it
decode and emit the current frame. Access unit delimiters are ignored
(`src/image.cpp:1850`). A demuxer-driven caller therefore has to push at least
one access unit ahead of the frame it is pulling, or the decoder must grow a
suspend/resume "need more data" state. The lookahead works (verified
incrementally on a stream with AUDs) but must be documented, because pushing a
single access unit and decoding it immediately runs the reader dry and the
decoder treats that as end of stream.

## Robustness

### 6. `error()` calls `exit()`

Many decode paths call `error()`, which prints and calls `exit(code)`
(`src/f264dec.cpp:97`). Examples: a failed `get_annex_b_NALU` on a truncated
stream (`src/nalu.cpp:55`), a frame-number gap (`src/image.cpp:181`), an
invalid start code emulation (`src/nalu.cpp:71`). A library called from a
player must not terminate the process; these should return error codes or call
a configurable error callback.

### 7. Frame-number gaps are fatal by default

With `conceal_mode == 0` and `gaps_in_frame_num_value_allowed_flag == 0`, a
`frame_num` that is not the previous one plus one calls
`error("An unintentional loss of pictures occurs! Exit", 100)`
(`src/image.cpp:150-182`). A player that seeks, drops packets, or opens a
stream mid-GOP hits this and dies. Decoding should conceal or resync instead.

### 8. Single global decoder instance

`p_Dec` is a global (`src/f264dec.cpp:76`); `f264_decoder_open()` returns that
global and `f264_decoder_close()` frees it. Two open decoders cannot coexist
and a second open overwrites the first. The player uses one video decoder, so
this is tolerable, but the per-instance handle type suggests otherwise.

### 9. Output pictures alias decoder-owned memory

`fill_f264_picture_from_dec_pic()` points the wrapper at the `DecodedPicList`
node's planes. The pixels are only valid until the next `decoder_decode()` or
`decoder_flush()`, and with a file output open the node is reusable
immediately. This needs to be documented, or the API should copy into caller
storage.

### 10. `get_one_avail_dec_pic_from_list` assumes a non-empty list

When the list is empty, `pPrior` stays NULL and `pPrior->pNext = pPic`
dereferences it (`src/f264dec.cpp:1062-1088`). `pDecOuputPic` is pre-allocated
so this is latent today, but a consume API that frees or empties nodes can
reach it.

## Integration warts

### 11. `add_subdirectory` builds an executable and wires CTest/Python

The submodule's `CMakeLists.txt` unconditionally defines the `f264dec`
executable (`:29`) and, when `BUILD_TESTING` is on and `tests/streams` exists,
requires a Python 3 interpreter for the regression test (`:42`). Embedding it
should be possible with library-only and no-test options.

### 12. `config_parse` misses fields that exist in `f264_config`

`f264_config_parse()` handles `threads`, `cpuid`, `frames`, `silent`, `input`,
`output` and `ref`, but not `deblock_enable` or `file_format`, which are
config fields.

### 13. `decoder_close` writes files into the current working directory

`CloseDecoder()` always calls `Report()`, which opens `log.dec` and appends to
`dataDec.txt` in the process working directory (`src/f264dec.cpp:500-616`),
even with `silent = 1`. Running the tests left both files next to the player
binary. A library should not create files as a side effect of closing.

### 14. The CLI counts every `F264_OK` as a decoded frame

`main.cpp:124-137` increments `frames_decoded` for every successful
`decoder_decode()` even though the call can return `F264_OK` with no picture
(early frames and reordering). Harmless for a benchmark, but it makes the
reported frame count depend on the DPB output timing rather than on pictures.

## Suggested minimum for the player

Issues 1-4 are required before the player can use the library: a memory input,
pictures materialized without an output file, a consume-once picture API, and
draining at flush. Issue 5 is a calling convention that follows from the
current design. Issues 6-7 matter for robustness on real streams and seeks.
