# VegPlayer (PS Vita)

Plays a VEGAS Pro `.veg` project in real time on the PS Vita: pick the project, point it at the source files it
needs, and it composites the timeline on the GPU while mixing the audio events live.

The `.veg` reading started from the parsers in `vegtool.c` and `tved.c` (chunk walking, GUIDs, Pan/Crop keyframes,
generators, project settings). Only the `.veg` part was taken: nothing of the `tved` format, ffmpeg or shell code.
That parser was then rewritten to walk the chunk tree structurally and extended (see *What is read from the .veg*).

## Building

You need VitaSDK with these packages:

```
vdpm libvita2d vitaShaRK SceShaccCgExt taihen freetype libpng libjpeg-turbo zlib
cmake -B build && cmake --build build -j4      # -> build/VegPlayer.vpk
```

`.github/workflows/build.yml` builds the VPK on GitHub Actions (`vitasdk/vitasdk` container) and uploads it as an
artifact, in case you want builds without the Codespace.

On the Vita, the first start compiles the effect shaders with vitaShaRK, which needs `ur0:data/libshacccg.suprx`
(the same file vitaGL ports ask for). The compiled `.gxp` files are saved in `ux0:data/vegplayer/shaders/`, so later
starts don't need it. Without it the player still runs: transforms and opacity stay on the GPU through vita2d's own
shaders, and the colour effects fall back to a CPU pass on the scene thread.

## Using it

1. Copy the `.veg` and its sources anywhere on the memory card. Files with the same name as in the project are
   found automatically next to the `.veg` (one sub-folder deep) or in `ux0:data/vegplayer/media/`.
2. Open the project. The **Sources** screen lists every source the timeline uses, separately for its picture and its
   sound, so you can take the picture from the `.mp4` and the sound from an `.at9`. `X` picks a file, `SQUARE` skips a
   source. Choices are saved next to the project in `<project>.veg.vegplayer`.
3. `TRIANGLE` opens the **Tracks** screen: switch individual video/audio tracks off (also live while playing).
4. `START` prepares (decodes what the timeline uses) and plays.

Player: `X` play/pause, `LEFT/RIGHT` ±1 s, `L/R` ±5 s, `UP/DOWN` ±10 s, `SELECT` restart, `TRIANGLE` track panel,
`SQUARE` hide the bar, `O` back. `SELECT` on the Sources screen opens **Options**. The FPS counter is in the top-left.

Log: `ux0:data/vegplayer/log.txt`.

## Formats

| Kind | Formats | Decoder |
|---|---|---|
| Video | H.264 in MP4 / MOV / M4V (up to 1920×1088) | **hardware** (`sceAvcdec`) |
| Pictures | PNG, JPEG, BMP, GIF (first frame), TGA, PSD, PNM, HDR | stb_image |
| Audio | AT9 (`.at9`, RIFF) | **hardware** (`SceAtrac`) |
| | AAC in MP4 / M4A | **hardware** (`sceAudiodec`) |
| | MP3 | **hardware** (`sceAudiodec`); Layer I/II streams fall back to dr_mp3 |
| | WAV (8/16/24/32-bit PCM, float), Ogg Vorbis | in code (stb_vorbis) |
| | AT3 / AT3plus (`.at3`, `.oma`) | **detected and refused** — see below |

**About AT3.** The Vita does decode ATRAC3/ATRAC3plus in hardware, but through `SceAudiocodec`
(`sceAudiocodecInit/Decode`, sysmodule `0x37`). VitaSDK has no header and no stubs for that library, and its NIDs and control-structure layout
aren't documented anywhere VitaSDK-based code can use. Rather than add a software decoder, or guess a struct and
risk crashing, AT3 files are recognised and reported as unsupported; converting them to AT9 works today. If you
have the `SceAudiocodec` NIDs and `SceAudiocodecCtrl` layout (DolceSDK has them), `adec.c` is where an
`AF_AT3` decoder would go: the RIFF/EA3 detection is already there.

Text and noise generators are skipped (as asked). Solid-colour generators are drawn.

## What is played

- **Video effects** (event and track level, in chain order): Invert, Black and White (grayscale), HSL Adjust (hue,
  saturation, luminance), Sepia and Brightness/Contrast. Each is an affine colour transform, so the whole chain is
  folded into one 3×4 matrix and applied by one GPU pass per layer. Unknown effects are listed on the Sources screen.
- **Pan/Crop** keyframes (linear between keys, mirroring, rotation; like Vegas, the window is rescaled to the
  source when the file you pick has another size or shape than the one the project was made with), **Track Motion**
  (first keyframe; when the source has another shape, its moves are scaled with the rescaled layout so moved
  tracks stay on the grid. Vegas doesn't do this; Options can switch it back), generator-track Add/Subtract compositing (guessed, like vegtool).
- **Opacity**: video track Level, event fade-in/fade-out with their curves (Linear, Fast, Slow, Smooth, Sharp),
  crossfades between overlapping events (dissolve).
- **Audio pitch attribute** (Event Properties → Time stretch/pitch shift):
  - *None*: pitch follows the playback rate (plain resampling, cubic interpolation).
  - *élastique*: WSOLA time-stretch/pitch-shift (grains aligned by cross-correlation). zplane's élastique is
    proprietary, so this is an approximation.
  - *Classic*: granular overlap-add with a grain size per Classic mode (A01–A19) and a narrower alignment search.
- **Pitch Shift plug-in**: *resample* (speed and pitch change together) and *Classic* (preserve duration).
- **Vibrato plug-in**: sine-modulated delay line.
- Event gain, audio track volume, fades, crossfades.

## What is read from the .veg (and what is guessed)

The format is reverse-engineered; these offsets came from comparing three Sparta-remix projects (VEGAS 11, 14, 15).

| Data | Where | Confidence |
|---|---|---|
| Media pool | `b28f2d5d`: id @0x3c, UTF-16 path @0x60 (full paths, so `Vidéo` folders survive) | solid |
| Event | `168d206a`: start/length @0x10/0x18 (100 ns), rate @0x28, kind @0x47, level @0x6c, audio fades @0x88/0x90 | level and fades inferred |
| Take | `168d206d`: in-point @0x10, media id @0x24, name @0x2a | solid |
| Audio stretch/pitch | `d54ad82a`: method @0x0c, attribute @0x10, semitones (double) @0x14 | semitones solid; method numbering **guessed** (0 None, 1 Classic, 2 élastique: all sample events use 2) |
| Pitch Shift plug-in | in `b66a95ce`: semitones, cents, ratio (double) | values solid; *preserve duration* flag position **guessed** |
| Vibrato plug-in | name match; parameter layout unknown | **guessed** (defaults: 5 Hz, 0.5 st) |
| OFX effects | `1af0e4e6` with the OFX wrapper GUID: named parameters | solid for HSL Adjust |
| Legacy Invert / B&W | `1af0e4e6`, GUIDs `c3062be7…` / `e87674d5…` | solid |
| Audio track volume | `69d46c27` (audio track settings): float @0x0c, linear gain | solid (values land on round dB steps) |
| Video track Level | `69d46c29` (compositing): float @0x68 | solid (Pinkiety: 0 %, 32.8 %, 35 %) |
| Fade curves | event @0x74 (in) / @0x78 (out): 1 Linear, 2 Fast, −2 Slow, 4 Smooth, −4 Sharp | codes match Vegas's defaults (audio 2/−2, video 4); which word is in/out **guessed** |

Options can override the guessed parts (stretch method, Pitch Shift mode, event level) if a project sounds wrong.

## Text

The UI draws text with `src/sharptext.c`, a drop-in replacement for `vita2d_pgf/pvf_draw_text`. vita2d renders each
glyph once at its base size and scales the quads (point-sampled, pen truncated to whole pixels), which looks crusty
below 1.0 and soft above it. sharptext makes the glyph image for the size it is drawn at and draws it 1:1 on whole
pixels: PVF fonts (vector) are rasterised at that exact size; PGF fonts (bitmaps) are resampled once per size (area
average when shrinking, Catmull-Rom when enlarging). Same arguments as vita2d, so it can be reused in other projects.

Button hints use the console's own button glyphs from `sa0:data/font/pvf/psexchar.pvf` (character map:
[vita_Symbols](https://github.com/nobodywasishere/vita_Symbols)); without that font the button names are written out.

## How it stays fast

Sparta remixes reuse a few seconds of a source thousands of times (the sample projects use 0.1–6.5 s of each video),
so preparing decodes only the frames the timeline shows, with the hardware H.264 decoder, into a frame cache in
GPU memory (CDRAM first, then mapped RAM; RGBA or RGB565, sized to fit the budget). Playback never seeks a decoder.

- **main thread**: UI and sceGxm submission (vita2d context), vsync'd at 60 Hz.
- **vp_scene** (core 2): builds the layer list for the audio clock's current time: frame lookup, Pan/Crop and Track
  Motion maths, effect matrices. Scenes are triple-buffered between this thread and the render thread.
- **vp_audio** (core 1, top priority): mixer and `sceAudioOut`. Its clock drives the picture.
- **vp_vdec / vp_adec0 / vp_adec1**: decoders, only while preparing.

Each frame is a handful of textured quads (the sample projects peak at 6–13 layers), so the GPU is nowhere near its limit
and the target is the display's 60 fps rather than the 15 fps minimum. The CPU clock is raised to 444 MHz.

## Testing status

This was written without a Vita or a full VitaSDK toolchain at hand:

- All sources pass a strict compile check (clang, ARM target) against `vita-headers` and the exact `libvita2d`
  commit vdpm ships, and every `sce*` function used was checked against the stub libraries that CMake links.
- The portable parts are tested on a PC (`tests/`): the `.veg` reader on the sample projects, the MP4 demuxer
  against ffmpeg-made files (Annex-B output re-decodes frame-exact), the audio engine (pitch/stretch accuracy measured
  by FFT), and the compositor and mixer over all three timelines.
- Not yet exercised: everything that only runs on the console (hardware decoders, GXM shader compile, audio port),
  so expect a first round of fixes on real hardware. `log.txt` records what each stage did.

Third-party code: stb_image and stb_vorbis (Sean Barrett), dr_mp3 (David Reid), both public domain / MIT-0.
