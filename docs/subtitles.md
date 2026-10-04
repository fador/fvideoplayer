# Subtitle rendering

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

Embedded text tracks are read from Matroska (SRT, ASS/SSA) and MP4 (`tx3g`,
`wvtt`), and external SRT/ASS/SSA/WebVTT/MicroDVD files next to the video are
discovered automatically.
