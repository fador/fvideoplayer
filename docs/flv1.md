# FLV and Sorenson Spark decoding

The FLV demuxer walks the tag list once at open time, which yields the
duration, the frame rate and a keyframe index for seeking even when the
`onMetaData` script tag is absent or incomplete; when it is present, its AMF0
payload is read for `duration`, `width`, `height` and `framerate`. Video tags
are Sorenson Spark (FLV1) and audio tags are MP3 or AAC. A tag's one-byte FLV
header is stripped before the sample reaches the decoder, and an AAC sequence
header is kept as the track's AudioSpecificConfig rather than played as a
sample. H.264 (AVC), On2 VP6 and Screen Video tracks are named but not
indexed: the audio tags still play, and a file with neither a decodable video
track nor audio is rejected with a message naming the codec.

The Sorenson Spark decoder (`src/decode/flv1/`) is written from scratch. FLV1
is H.263 baseline with a 17-bit picture start code, its own picture header and
its own escape form of the run-level codes, so the H.263 tables, macroblock
syntax and reconstruction are shared with the standard while the header,
escape coding and coefficient indexing follow the FLV variant. Implemented:

- Picture header: the FLV start code, the 8- or 16-bit picture size, the
  standard size codes (CIF, QCIF, 320x240 and the rest), picture type and
  quantiser, and the PEI padding
- GOB headers between macroblock rows, including the stuffing before the start
  code and the GQUANT that changes the quantiser mid-picture
- I and P pictures, all macroblock types: skipped macroblocks, 16x16 and 8x8
  (four-vector) motion, intra macroblocks inside P pictures, and dquant
- H.263 median motion vector prediction with the slice-start rules, half-pel
  bilinear motion compensation with edge replication, and the special chroma
  rounding for four-vector macroblocks
- Intra and inter inverse quantisation, the 8-bit intra DC, the FLV escape
  levels (11-bit and 7-bit forms) and the run that wraps past the end of the
  block as an end-of-block marker

The decode was checked against ffmpeg over the seven sample files used during
development: whole-file luma PSNR is 65-86 dB, which is the sub-LSB difference
between the floating-point inverse DCT here and ffmpeg's integer one, so the
bitstream is consumed identically. Unit tests cover the code tables
(prefix-free, correct markers), the header forms and flat intra and skipped P
pictures built from synthesised bitstreams, and the demuxer (synthetic FLV
files with metadata, MP3 and AAC audio, seeking, and H.264 tracks that play
their audio instead of being rejected).
