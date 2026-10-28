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
3. `TRIANGLE` opens the **Tracks** screen: switch individual video/audio tracks off, and set each track's level
   (video, 0–100 %) or volume (audio, off/−40…+12 dB) with `LEFT/RIGHT`, plus the **master volume** on the first row.
   They start at the project's values; `SELECT` resets the selected one. All of it works live while playing too.
4. `START` prepares (decodes what the timeline uses) and plays.
5. `RIGHT` opens **Export**:
   - audio: the mix as it plays (track toggles, sliders, master volume) to WAV (48 kHz, 16-bit) or MP3 (128–320 kbps,
     the shine encoder), saved in `ux0:data/vegplayer/exports/`. It renders faster than real time.
   - video: MP4 (H.264 + AAC, 29.97 fps) through the system's own recorder (SceMp4Rec, what the system uses for clip
     recording), in all its modes: 640×368, 480×272 (the PSP's screen) and 368×208, each at 2 or 1 Mbps. The picture
     is rendered frame by frame on the GPU (the project letterboxed into the frame), so it can take longer than the
     project; the recorder saves into `ux0:video` and VegPlayer then moves the file to `ux0:data/vegplayer/vexports/`, named after the project. SceMp4Rec is undocumented:
     every step is logged in `ux0:data/vegplayer/log.txt` (lines starting with `vexport:`) — send them if it fails.
6. `LEFT` opens **Project info**: the editor and version that saved the project (e.g. *Vegas Pro 11.0 (Sony)*,
   *VEGAS Pro 20.0*, *Movie Studio Platinum 12.0 (Sony)*, read from the editor's settings folder stored in the project),
   where it was saved on the PC, format, length, track/event/media counts, missing sources, Track Motion, compositing
   modes, Pan/Crop, the effects played and not played, time stretch and fades.

Picture sources can also be MP3 files: their cover art (ID3v2 picture, front cover preferred) is used.

**Official games and apps**: browsing into `ux0:app/TITLEID` (or `gro0:app`, `ux0:patch`) opens it decrypted like
VitaShell's *Open decrypted* (`sceAppMgrGameDataMount`), so its MP4 videos, PNG/JPEG pictures and AT9/MP3/WAV/OGG
sounds can be picked as sources. One title at a time: a project's sources can come from one game or app (plus any
unencrypted files). Game textures in GXT format aren't supported.

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
| | AT3 / AT3plus (`.at3` RIFF, `.oma`/`.aa3` unencrypted) | **hardware** (`SceAudiocodec`) — see below |

**About AT3.** ATRAC3/ATRAC3plus go through `SceAudiocodec` (sysmodule `0x37`, or the internal one). VitaSDK
has no header or stub for it, so both come from DolceSDK (`third_party/sce`); the stub format is VitaSDK's, so it
links unchanged. The decoder's work memory, frame and output buffers live in one physically contiguous block. The
`fact` chunk's encoder delay is skipped; DRM-encrypted OMA files are refused. Not yet tested on hardware: if a file
fails, the log has the SCE error code. Verified on a Vita: ATRAC3plus output is interleaved 16-bit and bit-exact with FFmpeg.

Text and noise generators are skipped (as asked). Solid-colour generators are drawn.

## What is played

- **Video effects** (event and track level, in chain order), all on the GPU (shaders compiled once with vitaShaRK):
  - colour effects folded into one matrix per layer: Invert, Black and White, HSL Adjust, Sepia, Brightness/Contrast,
    Channel Blend (e.g. Red/Green/Blue Only), LAB Adjust (lightness, a/b, Invert Luminosity, desaturate; done in
    YCbCr), Color Corrector (saturation, gain, offset) and Color Balance;
  - Gradient Map (brightness through a gradient texture, mixed by amount);
  - Linear Blur, Gaussian Blur and Radial Blur (17-tap blur pass). Like Vegas, blurs act after Pan/Crop on the
    whole event frame: the amount is relative to the frame and the blur spills past the picture's edges. One Gradient
    Map or blur per layer.
  Without `libshacccg.suprx` the colour effects and Gradient Map run on the CPU; blurs are skipped.
- **Track compositing modes**: Add, Subtract, Cut, Multiply, Screen, Lighten, Darken. Overlay, Hard Light, Dodge, Burn
  and Difference need the picture underneath in the shader, so they draw normally for now.
- **Pan/Crop** keyframes (Linear, Fast, Slow, Smooth, Sharp and Hold interpolation, mirroring, rotation; like Vegas, the window is rescaled to the
  source when the file you pick has another size or shape than the one the project was made with), **Track Motion**
  (keyframes with position, size and 2D rotation, interpolated with the same curve types; when the source has another shape, its moves are scaled with the rescaled layout so moved
  tracks stay on the grid. Vegas doesn't do this; Options can switch it back), generator-track Add/Subtract compositing (guessed, like vegtool).
- **VEGAS 20 effect names** (`com.vegascreativesoftware:*`): Channel Blend (OFX: per-channel weights and offsets,
  Normalize), Invert (`BlendAmount`), and the other OFX effects under their new prefix.
- **Track Motion 2D Shadow and 2D Glow** (first keyframe): a soft copy of each layer's shape in the shadow/glow
  colour, moved and scaled inside the track's frame and drawn just behind the layer (one extra draw per layer, edges
  softened in the shader). Without the shader compiler they draw as flat, hard-edged quads. Options can turn them off.
- **Opacity**: video track Level, event fade-in/fade-out with their curves (Linear, Fast, Slow, Smooth, Sharp),
  crossfades between overlapping events (dissolve).
- **Audio pitch attribute** (Event Properties → Time stretch/pitch shift):
  - *None*: pitch follows the playback rate (plain resampling, cubic interpolation).
  - *élastique*: WSOLA time-stretch/pitch-shift (grains aligned by cross-correlation, to a fraction of a sample;
    attacks are detected and played once from a single grain with a short crossfade, instead of being smeared or
    doubled). zplane's élastique is proprietary, so this is an approximation. Measured with `tests/stretchq`: a pure
    tone comes out about 78 dB clean (was 29–41 dB), clicks stay sharp and undoubled, at the same CPU cost.
  - *Classic*: granular overlap-add with a grain size per Classic mode (A01–A19) and a narrower alignment search.
- **Pitch Shift plug-in**: *resample* (speed and pitch change together) and *Classic* (preserve duration).
- **Vibrato plug-in** (Sony Vibrato): sine-modulated delay line with the project's rate and depth.
- Event gain, audio track volume, fades, crossfades.
- **Reverb** in an audio track's FX chain (Sony Reverb; Options can turn it off): a light per-track reverb (pre-delay, early reflections,
  damped tail) with the project's dry/reverb/early levels, decay time, pre-delay and high cut. Tracks without reverb
  cost nothing; a reverb stops processing once its tail has died away.

## What is read from the .veg (and what is guessed)

The format is reverse-engineered; these offsets came from comparing three Sparta-remix projects (VEGAS 11, 14, 15).

| Data | Where | Confidence |
|---|---|---|
| Media pool | `b28f2d5d`: id @0x3c, UTF-16 path @0x60 (full paths, so `Vidéo` folders survive) | solid |
| Event | `168d206a`: start/length @0x10/0x18 (100 ns), rate @0x28, kind @0x47, level @0x6c, audio fades @0x88/0x90 | level and fades inferred |
| Take | `168d206d`: in-point @0x10, media id @0x24, name @0x2a | solid |
| Audio stretch/pitch | `d54ad82a`: method @0x0c, attribute @0x10, semitones (double) @0x14 | semitones solid; method numbering **guessed** (0 None, 1 Classic, 2 élastique: all sample events use 2) |
| Pitch Shift plug-in | in `b66a95ce`: semitones, cents, ratio (double) | values solid; *preserve duration* flag position **guessed** |
| Vibrato plug-in | Sony Vibrato `d6802ba0`: i32 params; rate (centi-Hz) @0x1c, depth (cents) @0x20 | rate varies per instance as a rate would (9.99, 3.02, 4.63 Hz); depth **guessed** |
| OFX effects | `1af0e4e6` with the OFX wrapper GUID: named parameters | solid for HSL Adjust |
| Legacy Invert / B&W | `1af0e4e6`, GUIDs `c3062be7…` / `e87674d5…` | solid |
| Track compositing mode | `69d46c29` @0x64: Vegas's CompositeMode order (3D Source Alpha 0, Add 1, Subtract 2, Cut 3, Source Alpha 4, Multiply 5, Overlay 6, Screen 7, Hard Light 8, Dodge 9, Burn 10, Darken 11, Lighten 12, Difference 13, Difference Squared 14; Custom -1) | order from the scripting API; 4 is the default everywhere, which fits |
| Pan/Crop keyframe type | keyframe @16: 0 Linear, 1 Fast, 2 Slow, 3 Smooth, 4 Sharp, 5 Hold | Smooth (3) seen in the Porta Mix project |
| Media path | media chunk: the first word is the path's offset (0x50 Vegas 8, 0x60, 0x80) | solid |
| Vegas 8 differences | stretch chunk 24 bytes (semitones @0x10, no attribute); DXT Linear Blur `8dbe8301`; Solid Color floats after the generator GUID | from scan.veg (Vegas 8 and its Vegas 17 re-save) |
| DXT effect values | the first keyframe's copy of the parameter block (the static block can be stale) | solid for Channel Blend, Gradient Map |
| Channel Blend | 2 x 20 doubles; block 1: per output channel its weights from R,G,B,A (+1); rows normalised | Red Only = the picture's brightness in red |
| Gradient Map | amount, stop count, 32-byte stops (B,G,R,A floats, position double) | Sunset decodes to black-orange-yellow-white |
| Audio track FX | `f79298f4`: same records as the event plug-in chain; Sony Reverb i32 params | levels/decay/pre-delay/high cut **guessed** from one project |
| Audio track volume | `69d46c27` (audio track settings): float @0x0c, linear gain | solid (values land on round dB steps) |
| Video track Level | `69d46c29` (compositing): float @0x68 | solid (Pinkiety: 0 %, 32.8 %, 35 %) |
| Track Motion shadow/glow | `cd6915d8`: u32 @0x04 = enabled parts (1 position, 8 2D Shadow, 16 2D Glow); from @0x24 three keyframe lists (count, then keyframes with a size field at s): x/y @s+0x30/0x38, w/h @s+0x50/0x58, colour B,G,R,A floats @s+0xb0, blur @s+0xc0 | flags and colours solid (CTR Custom vs the Veg Custom it came from, scan.veg); how blur and glow intensity map to pixels **guessed** |
| Track Motion keyframes | position list in `cd6915d8`: per keyframe interpolation type @s+4 (as Pan/Crop), time @s+8 (u64, 100 ns, project timeline), rotation as a quaternion (w @s+0x90, z @s+0xa8: angle = 2 atan2(z, w)) | from Northern Pacer (16 keyframes rocking between -18.8 and +16 degrees on the beat) |
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
- Optional (Options > "Small-layer frame copies", off by default): every cached frame also gets a quarter-size copy
  that layers drawn small sample instead. It can stop small tiles shimmering, but costs about 6 % more cache memory.
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
