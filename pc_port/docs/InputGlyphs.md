# Input Glyphs

The Controls screen draws keys and controller buttons as small pictures. The
pictures are the "Input Prompts Pixel" set by Kenney (CC0, www.kenney.nl),
packed into one PNG that you can repaint.

## The Gamepad Icons setting

"Gamepad Icons" on the Controls screen picks which controller the pictures
copy. The values, in order:

| Value      | `gamepad_icons` | Looks like                                  |
|------------|-----------------|---------------------------------------------|
| Auto       | 0               | follows the connected controller            |
| PS Classic | 1               | PS1 pad: slanted Select / Start, 4-button d-pad |
| PS Modern  | 2               | PS4 / PS5 pad: Share or Create, Options     |
| Xbox       | 3               | A/B/X/Y in colour, LB/RB/LT/RT, View / Menu |
| Nintendo   | 4               | A/B/X/Y letters, L/R/ZL/ZR, - / +           |
| Steam      | 5               | Steam Deck style buttons                    |

The value is saved in `config.cfg` as `gamepad_icons`. Auto is the default.
The keyboard columns always show keyboard and mouse pictures, whatever this
setting says.

Face buttons are placed by position: the bottom button is Cross / Xbox A, the
right one Circle / B, the left one Square / X and the top one Triangle / Y.
Nintendo is the exception: it follows the button's name, so the button the
game reads as `a` draws an A.

## Auto

Auto looks at the first controller the game finds and picks a set:

- PS4 and PS5 pads: PS Modern. PS3 pads: PS Classic.
- Xbox 360 and Xbox One pads: Xbox.
- Switch Pro pads and Joy-Cons: Nintendo.
- A Steam Controller or a Steam Deck (Game Mode sets `SteamDeck=1`): Steam.
- A pad that Steam Input presents as its own virtual pad is not counted as
  Valve hardware. The game uses the type SDL reports for it, which is the real
  pad underneath when Steam says so. The one exception is a Steam Deck
  (`SteamDeck=1`): there the virtual pad (0x28DE:0x11FF) counts as Steam when
  SDL reports its type as unknown, Xbox 360, Xbox One or virtual.
- Anything else, or no controller: Xbox.

The row shows just `Auto`; the pictures themselves show what was detected.
Plugging in another controller while the screen is open switches the pictures.
Picking any other value by hand turns detection off until you pick Auto again.

## The atlas

The pictures live in one PNG in the game folder:
`gamedata/icons/input_glyphs.png`. In the source tree it is
`pc_port/assets/gamedata/icons/input_glyphs.png`, and a build copies it.

- Size: **256 x 512** pixels, made of **16 x 16** cells.
- Columns 0 to 7 hold the **light** version of each picture (used for the
  selected cell). Columns 8 to 15 hold the **dark** version (every other cell).
  A dark picture sits exactly 8 columns to the right of its light one.
- The long keys (Enter, Backspace, Shift, Ctrl, Alt, Tab, Caps, Del, Insert, Home,
  End, Page Up, Page Down, Print, Scroll, Pause, Num) take two cells side by side.
  Space and every other picture take one cell.
- Use transparency (alpha 0) for everything that is not part of a picture.

The game registers the atlas as a high-resolution override over a VRAM
rectangle it never writes (VRAM x 448, 32 x 256 texels, texture page 7). The
override is keyed by one CLUT cell no TIM uses, (1008, 511), so the game's own
lookups in that rectangle are left untouched.

## Repainting

Two ways:

1. Edit `input_glyphs.png` in place. Keep the size, the cell grid and the
   light / dark column split.
2. Change which Kenney tiles are used. The picks are listed in
   `pc_port/tools/build_input_glyphs.py`, reading from the vendored
   `pc_port/tools/kenney_input_prompts_pixel/tilemap_packed.png`. Edit the
   picks and run:

       python pc_port/tools/build_input_glyphs.py

   That rewrites `input_glyphs.png` and the generated files
   `pc_port/include/pc_input_glyphs_ids.h` and
   `pc_port/src/pc_input_glyphs_atlas.inc`. Commit all three. The build does
   not run the script.
   `--preview <out.png>` writes a preview sheet to check the picks.

Do not run the script after hand-editing the PNG, or the edits are lost.

## When a picture is missing

- A key with no picture (F13 and up, the Application key) is drawn as text.
- A controller input with no picture in the chosen set (Guide, and any other
  button the set lacks) draws the empty round button.
- If `input_glyphs.png` is missing, is not 256 x 512 or cannot be read, the
  whole screen draws text instead of pictures. Listening (`...`) and unbound
  (`-`) cells are always text.

## Log lines

With debug logging on, `SilentHill_<date>.log` has:

- `[GLYPHS] atlas registered` once, or `[GLYPHS] <reason>, drawing text`
  when it could not be loaded (for example `[GLYPHS] atlas gamedata/icons/input_glyphs.png missing, drawing text`).
- `[GLYPHS] auto: <label>` each time Auto settles on a different set, for
  example `[GLYPHS] auto: PS_Modern`.

## Credit

Input Prompts Pixel by Kenney, www.kenney.nl, released under CC0. The licence
text ships as `gamedata/icons/License_Kenney.txt`.
