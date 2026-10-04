# Filter graphs

Between a decoder and the renderer (or the audio sink) the pipeline runs a
linear filter graph, the same position an ffmpeg filtergraph or a DirectShow
transform filter chain occupies in MPC-HC. Every decoded picture and every
decoded PCM chunk passes through the graph on the pipeline thread:

```
demux -> video decoder -> video filter chain -> frame queue -> renderer
     -> audio decoder -> audio filter chain -> PCM buffer -> sink
```

The graph is described in ffmpeg syntax: a comma separated list of filters,
each `name` or `name=key=value:key=value`, with positional arguments and named
arguments mixed as needed.

```
--vf "scale=1280:720,flip=h,eq=brightness=0.05:contrast=1.2:saturation=0.8"
--af "volume=1.5,resample=48000,channels=2,equalizer=bass=6:mid=-2:treble=3"
```

Built-in filters:

| Graph | Filter | Arguments |
| --- | --- | --- |
| Video | `scale` | `WIDTH:HEIGHT`; `-1` derives a dimension from the aspect ratio, `-2` also rounds it to an even number (`scale=1280:-2`) |
| Video | `crop` | `WIDTH:HEIGHT[:X:Y]`; a negative width/height is relative to the frame edge, a negative x/y centers the rectangle; explicit odd offsets are rejected for subsampled chroma |
| Video | `flip` | `h`, `v` or `hv` (`hflip` / `vflip` also work) |
| Video | `grayscale` | none |
| Video | `eq` | `brightness`, `contrast`, `saturation` (brightness in [-1, 1]; contrast and saturation are multipliers) |
| Audio | `volume` | linear gain, or decibels with a `dB` suffix (`volume=1.5`, `volume=-6dB`) |
| Audio | `resample` | output rate in Hz, linear interpolation with state across chunks |
| Audio | `channels` | `1` or `2`: stereo to mono averages the pair, mono to stereo duplicates |
| Audio | `equalizer` | `bass`, `mid`, `treble` in dB (200 Hz low shelf, 1 kHz bell, 6 kHz high shelf) |

A filter that changes the audio format (a resampler or a remix) makes the engine
reconfigure the sink and clear the PCM buffer, so chunks of different formats
never mix. The video chain reconfigures itself whenever the decoded geometry
changes, and `reset()` clears filter state on seek and track changes. The
Filters menu and the Filters window show the active graph, accept a new spec,
and report parse errors; `--vf` and `--af` set the initial graph from the
command line.
