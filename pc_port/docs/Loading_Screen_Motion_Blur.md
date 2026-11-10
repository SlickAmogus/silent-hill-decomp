# Loading-screen motion blur (framebuffer feedback)

The Harry-running loading screen, the door out-fade and the item-screen load all
draw through `Screen_BackgroundMotionBlur` (`src/bodyprog/screen/background_draw.c`).
This note records what the PS1 does there, why the port looked different, and
how PsyCross reproduces it now. The same store feeds the per-map dream and
ghosting overlays and the scene-scratch soft focus; the last section covers
those.

## What the PS1 does

- Two display buffers: `GsDefDispBuff2(0, 32, 0, 256)` puts buffer 0 at VRAM rows
  32..255 and buffer 1 at rows 256..479, 320x224 each (`Screen_Init`).
- `g_ActiveBufferIdx = GsGetActiveBuff()` is the buffer being DISPLAYED, i.e. the
  frame just shown. `water.c` proves it: its 1x1 TILE goes to the draw buffer and
  the DrawMove that reads it back adds `+32` when the index is 1 and `+256` when
  it is 0, so the draw buffer is the other one.
- Every frame `MainLoop` adds `GsSortClear` to the far end of OT0, so the draw
  buffer is cleared first. `Screen_BackgroundMotionBlur` then adds up to three
  opaque `SPRT`s (code 0x64, no semi-transparency bit) into the same far bucket,
  textured from `getTPage(2, 0, j<<8, g_ActiveBufferIdx<<8)` with
  `v0 = 32` on buffer 0 and `0` on buffer 1: that is the displayed buffer,
  copied onto the same position of the draw buffer. Harry draws over it, then
  OT2 (the screen fade).
- The copy modulates with 128 (identity) or, when
  `VSync(-1) % vBlanks == 0`, with 127. Rectangles are not dithered on the GPU
  (`DuckStation gpu.cpp FillDrawCommand`: "always disabled for sprites"), so a
  127 frame is exactly `t5 -> (t5*127)>>7 = t5-1` on every non-black channel and
  a texel whose 16-bit word is 0 is skipped. Nothing else fades the picture.

So frame N = Harry_N drawn over M(frame N-1), where M is identity or "one 5-bit
level off", and a pose stays on screen until it has lost 31 levels. At 60 fps
with `SyncMode_Wait2` that is a decay every other frame, about a second of
trail; when a load stretches the frames the parity is effectively random and the
average is the same per frame. That is the heavy smear a console and DuckStation
show.

## What the port did

PsyCross has no VRAM double-buffer. It captured the composed frame each present,
downscaled it to 320x224, packed it into both display-buffer rects of the VRAM
texture with a gain of 0.7 (`g_PsxFeedbackDamp`) and let the SPRT sample that.
Three things were wrong against the above:

1. The gain. A pass that keeps 0.7 of the level makes a pose vanish in three or
   four frames; the console keeps every pose for ~31 decay frames.
2. The resolution. Harry is drawn at window resolution, his trail came back as
   320x224 blocks. DuckStation at higher internal resolution keeps the whole loop
   at that resolution.
3. The decay cadence. The port steps the loading screen every
   `g_PcLoadScreenPaceVblanks` (2) vblanks and holds the loop between steps, so
   the vblank count had the same parity on every step and the game's 127/128
   choice was constant: decay on every step or never.

## What it does now

- `g_PsxFeedbackDamp` is 1.0.
- For an opaque reader that copies a display buffer onto the same buffer-relative
  position (`GR_FeedbackScreenTexture`), the store captures the display area 1:1
  at its own pixel size and the SPRT samples it by `gl_FragCoord` through the
  hi-res override sampler (`u_fbScreen`), doing the GPU's arithmetic in the
  shader: level `L = round(v*31.875)`, `L = (L*mod)>>7` with `mod` recovered from
  the vertex colour, output `L*8`, word 0 discarded. Nothing is resampled, so the
  loop cannot drift, and the round trip through the 8-bit framebuffer is
  lossless. The VRAM rects are still packed from the same capture for every
  other reader and as the fallback (console `FBSCREEN 0`).
- While the loading screen is paced, `Screen_BackgroundMotionBlur` decides 127
  vs 128 by the step counter (`g_PcLoadScreenStep`), decaying on every other
  step; with `LOADPACE 1` it falls back to the game's vblank rule, which at 60 fps
  is the same thing.

## The blending readers

The cutscene overlays (map4_s01 Lisa, map1_s05, map6_s00, map7_s03 and the
rest, plus the lighthouse glow) draw semi-transparent SPRTs (`RECT_BLEND`,
mostly average mode, modulation 128) that copy the displayed buffer onto the
same position in OT2, with `DR_STP(1)` placed at the far end of OT0 so every
pixel the world draws carries the STP bit and blends. Each frame is therefore
half the live frame plus half the previous one: an exponential ghost whose
decay is the blend itself, which is why their loop gain was already unity.
The Alessa and Lisa soft focus (map3_s02, map4_s04, map7_s02) is the same idea
through a scratch rect: the scene is redirected into offscreen VRAM and eight
strips composite it back at one- or two-pixel offsets with different blends.

Those readers were still served by the 320x224 VRAM store, a filtered
downscale blended over a world drawn at window resolution. They now sample the
same 1:1 capture as the loading trail: a copy onto its own position by screen
position, an offset strip through its own UVs over the rect's PSX size
(`GR_FeedbackSampleTexture` mode 2), with the sampler giving each texel the
alpha the VRAM path's LUT would have (0.5, STP set) so average, additive and
`dream_blur_strength` behave as before. The scratch rect has its own capture
target because it is taken mid-frame, while the per-present capture must stay
for the display-buffer readers of the same frame.

Console: `FBSCREEN`, `FBDAMP`, `FBEXACT`, `LOADPACE`, `LOADMIN`.

## Applying the PsyCross side

The renderer half lives in PsyCross. Until it is merged there, the commit is
kept as `pc_port/patches/PsyCross-feedback-blur.patch`; from the PsyCross
checkout:

```bash
git am path/to/pc_port/patches/PsyCross-feedback-blur.patch
```
