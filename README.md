# MediaEffects

Apply effects to images and videos from the command line, using **ImageMagick** and **ffmpeg** under the hood.

Two ways to run it are provided:

- **`mediaeffects`** — the Bash source script (works on Linux, macOS, MSYS2/Git Bash, WSL).
- **`mediaeffects.exe`** — a self-contained Windows executable build of the same tool.

Both accept identical arguments and produce identical output.

## Requirements

The tool is a thin wrapper around two external programs; they must be installed and available on `PATH`:

- **ImageMagick** (`magick`)
- **ffmpeg** (and `ffprobe`, required for video)

## Usage

```
mediaeffects <input> <output> [effects...] [options]
```

Examples:

```
mediaeffects in.png out.png --invert
mediaeffects in.png out.png --invert --fisheye 1 --hsv 180,0,0
mediaeffects clip.mp4 out.mp4 --fisheye 0.5 --hsv 45,20,-10
```

On Windows, replace `mediaeffects` with `mediaeffects.exe` (or just `mediaeffects` if the `.exe` is on your `PATH`).

## Effects

| Effect | Arguments (default) | Description |
| ------ | ------------------- | ----------- |
| `--invert` | — | Negate colors (ffmpeg `-vf negate` style) |
| `--invertlum` | — | Invert luminosity in LAB colorspace (ImageMagick `-colorspace lab -channel r -negate` style) |
| `--fisheye` | `[strength]` (0.5) | Fisheye distortion (ffmpeg `geq` style) |
| `--hsv` | `H,S,V` (required) | Hue shift in degrees, saturation and value shifts (0 = no change), e.g. `--hsv 180,0,0` (ImageMagick `-modulate` style) |
| `--hue` | `[degrees]` (90) | Rotate hue in degrees (ImageMagick `-colorspace yuv -fx` style) |
| `--explode` | `[strength]` (1.0) | Explode outward (ImageMagick `-implode` style) |
| `--swirl` | `[strength]` (45) | Swirl in degrees (ImageMagick `-swirl` style) |
| `--hflip` | — | Mirror horizontally (ffmpeg `-vf hflip`) |
| `--vflip` | — | Mirror vertically (ffmpeg `-vf vflip`) |
| `--rotate` | `[degrees]` (90) | Rotate by degrees (ffmpeg `-vf rotate`) |
| `--magik` | — | Liquid-rescale to 50%x50%, then resize 200% (ImageMagick `-liquid-rescale`) |
| `--haah` | — | Mirror left half onto right (`-crop +clone -flop +append`) |
| `--waaw` | — | Flop, then mirror left half onto right |
| `--hooh` | — | Rotate 90, mirror half, rotate back |
| `--woow` | — | Rotate 90 + flop, mirror half, rotate back |
| `--stretch` | `[x,y]` (1.1,1.1) | Stretch/squeeze horizontally and vertically (ffmpeg `geq` style); numbers separated by `,` or `;` |
| `--resize` | `WxH` \| `W` \| `N%` (required) | Resize with ImageMagick, e.g. `1280x720`, `500`, `50%` |
| `--derain` | — | Fake derain color trick (ImageMagick colorspace) |
| `--rain` | — | Fake rain color trick (ImageMagick colorspace) |
| `--bgr` | — | Swap color channels to BGR (`-color-matrix`) |
| `--wave` | `[params]` (3.2,3.2,0.05,0.05,0.628,0.628,0,0) | Sinusoidal wave distortion (ffmpeg `geq` style). Params: `xfreq,yfreq,xamp,yamp,xphase,yphase[,xspeed,yspeed]`, separated by `,` or `;` |

## Options

| Option | Description |
| ------ | ----------- |
| `--hidelogs` | Hide progress logs |
| `--help` | Show help and exit |

## Supported formats

- **Images:** `.png`, `.jpg`, `.jpeg`, `.gif`
- **Videos:** `.mp4`, `.mov`, `.mxf`, `.mkv`, `.avi`

## How it works

- **Images** are passed directly to ImageMagick (or ffmpeg for the ffmpeg-style effects).
- **Videos** are processed frame-by-frame: ffmpeg extracts frames → ImageMagick applies the effect → ffmpeg reassembles the frames into the output video. Effects are batched where possible and frame passes run in parallel.

## Installation

### Windows

Put `mediaeffects.exe` anywhere on your `PATH` (or in your project folder) and run it.

### Linux / macOS / Git Bash

Run directly without installing:

```
bash mediaeffects in.png out.png --invert
```

Or self-install into your PATH:

```
bash mediaeffects install
mediaeffects --help
```

Note: self-install only works when the script is saved as a file (not when piped through `curl | bash`). Download it first, then run `install`.

## Effect source generation

Each run writes the generated effect code into the **`effects/`** folder as C source files (`invert.c`, `fisheye.c`, `hsv.c`, `hue.c`, `explode.c`, `swirl.c`, `hflip.c`, `vflip.c`, `rotate.c`, `magik.c`, `haah.c`, `waaw.c`, `hooh.c`, `woow.c`, `stretch.c`, `resize.c`, `derain.c`, `rain.c`, `bgr.c`, `wave.c`). These document the exact ffmpeg/ImageMagick command each effect expands to.
