# Porting the iOS fixes to Android

Most of this session's work is in shared code (`src/`, `pc_port/src/`), so Android
wants nearly all of it. The quickest route is a merge; the table is here so the
Android session knows what it is getting and which handful of hunks to skip.

```
git merge fork/ios-port
```

`ios-port` is `android-port` plus these commits plus a merge of `pc-port`, so the
merge is mostly fast-forward material. Resolve conflicts by keeping BOTH sides
wherever one side simply added something the other did not touch — that has been
the shape of every conflict between these two branches so far.

## Take these — shared code, Android has the same bug

| Commit | What it fixes | Android relevance |
|---|---|---|
| `cd43e3862` | Black sky, and the cafe culling its own walls | **Both apply.** The sky half gates the cutscene black-clear to `map3_s02` only. The culling half is the bigger one: the interior cell gate decided a neighbour cell's room from its **centre alone**, while the room table it mirrors samples five points and says outright that one is not enough. Small rooms spill into a cell whose centre lands in the dead gap, so the gate rejected the cell holding that room's own walls. Shows at wide aspects — any tablet or 20:9 phone. |
| `ba8240b39` | Fire button, One Button Combat, pause glyph | All shared. `TB_BACK` shares `TB_START`'s position and was being drawn in gameplay too, so the back mark and the pause bars rendered inside one ring — Android draws that same ring. |
| `71e33461d` | Running starts sooner, stops dropping | Pure tuning in `pc_touch.c`. One threshold was doing two jobs; now engage 0.680 / release 0.480. |
| `c50dae299` | Back out of puzzles and brightness | Free-cursor puzzles returned `TC_MODE_OFF`, which correctly suppresses the confirm but also empties the corner escape slot — unescapable with no pad. Now `TC_MODE_BACK`. |
| `172a09599` | Tap skips the boot logos and warning screen | `BootSkip_Pressed` / `Warn_SkipPressed` read raw SDL keyboard and pad only. |
| `a6bc9e27d` (part) | Memory card | **Check Android first.** There is no `.MCD` anywhere in the tree, `MemCardFormat` is an unimplemented stub, and `MemCardExist` just `fopen`s `"<chan>.MCD"` against the CWD — so saving hangs on "checking the memory card" wherever no card happens to exist. The PsyCross half (NULL guards in `MemCardAccept`/`MemCardOpen`, which dereferenced the failed `fopen`) is worth taking regardless. The card-creation half is in `ios_port/src/ios_bootstrap.m`; Android needs its own equivalent in `SilentHillActivity`, writing 128 KB with `'M','C'` in the first two bytes. |

## Skip these — iOS-only

`57b1a11ef` (Info.plist bundle keys), `70b6d31e9` (framebuffer 0 is not the screen
on iOS — Android's EGL surface genuinely **is** FBO 0, so do not apply it),
`427899f25` / `130067afe` (points-vs-pixels; Android has no high-DPI split),
`3c1de6d00` (iOS Options naming), `e73c6d809` (Lua off because `loslib` needs
`system()`).

## Two conflicts to expect

- **`options.c` row budget.** Pages cap at **11 rows including navigation**. The
  mobile layout now is: Controls drops the two mouse rows and gains the touch
  rows; Graphics drops Resolution and Window_Mode and gains Bullet_Decals back.
  Android already compiles out its own set — recount after merging rather than
  assuming, or a page runs off the bottom of the screen.
- **`game_main.c`.** iOS took only the radio hunk from Android's delta, not the
  frame profiler, because the profiler references `g_PsyX_DrawCalls` /
  `MsParse` / `MsSubmit` / `OtNodes`, which live in Android's own uncommitted
  PsyCross. Android keeps its profiler; just do not let the merge delete it.

## One still open

Map and pause screens intermittently clear to fog grey with the text on top,
instead of showing the world underneath. It tracks the pause **freeze-frame
capture** failing: when the blit succeeds you see the world, when it fails the
fog clear becomes the whole image. Android's own commit already made that path
report itself rather than claiming success — so reproduce it and read the log for
`[FREEZE] capture ... FAILED err=`, which names the reason instead of guessing.
