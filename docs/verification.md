# Verification

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

The from-scratch audio decoders are verified against ffmpeg: the same input is
decoded with the built-in reference path and the PCM is compared by
correlation and maximum sample difference (see the per-codec sections in
[audio.md](audio.md)).

## Test suite

Unit tests cover the hvcC parser, NAL splitting, the container demuxers (built
from minimal in-test MP4/MKV/AVI/FLV/MPEG-PS files), the audio modules (bit
reader, Huffman codebooks, MDCT/windows, volume gain, SDL sink and AAC frame
decoding from embedded ADTS/raw vectors), the filter graphs (spec parsing,
scaling/cropping/flipping/grayscale/eq on synthetic frames, and volume,
resampling, remixing and equalizer response on synthetic PCM) and the
Sorenson Spark decoder (code tables, header forms and synthesised I and P
pictures). The decoders that need a real stream (AC-3, MP2, the audio
reference comparisons) are exercised through environment-gated tests that
point at a file on disk.
