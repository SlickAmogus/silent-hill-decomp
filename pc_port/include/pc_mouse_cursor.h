#ifndef PC_MOUSE_CURSOR_H
#define PC_MOUSE_CURSOR_H

/* PC port: mouse control for cursor-driven puzzles + a clickable main menu.
 *
 * QoL enhancement (not 1:1 with the PSX original). Gated by config
 * `mouse_cursor` (default on) and only active when the OS cursor is free —
 * i.e. relative mouse mode is OFF, which is true in menus, puzzles and the
 * classic camera, but NOT in TPS/OTS/FPS where the pointer is captured for
 * look. See control_style.c for the capture logic.
 *
 * Puzzles: every free-cursor puzzle (piano, plate, door panels, alert door,
 * map pan) integrates the left analog stick, draws through Gfx_CursorDraw, and
 * confirms with the `enter` bind. We detect one is on screen via Gfx_CursorDraw
 * and inject mouse motion into the stick + a click into the controller, so the
 * puzzle's own cursor follows the mouse with no per-puzzle code.
 *
 * Menu: the title menu is discrete row-selection, so we hit-test the mouse
 * against the row bands, drive the selection on hover, and confirm on click. */

/* Once per frame from MainLoop, after the controller is built and after
 * Pc_ControlStyleUpdate. Polls the mouse; if a cursor puzzle is on screen,
 * injects its motion + click into g_Controller0. */
void Pc_MouseCursor_FrameUpdate(void);

/* Called from Gfx_CursorDraw: a cursor-driven puzzle drew its cursor this
 * frame, so arm the puzzle mouse injection for the next few frames. */
void Pc_MouseCursor_NoteCursorDrawn(void);

/* Hit-test a vertical list of menu rows drawn at y = baseY + i*stepY (top-left
 * 320x240 coords, as Gfx_StringSetPosition uses). Returns the hovered row index
 * (0..count-1) or -1. visibleFlags gates selectable rows (bit i); pass ~0u when
 * every row is selectable. *outClicked is set to 1 when the left button was
 * pressed this frame while hovering a valid row. */
int Pc_MouseCursor_MenuRowHover(int baseY, int stepY, int count, unsigned int visibleFlags, int* outClicked);

/* 1 if the mouse moved this frame — use to avoid an idle mouse fighting the
 * keyboard/pad for the menu selection. */
int Pc_MouseCursor_Moved(void);

/* 1 while a free-cursor puzzle is on screen. control_style.c releases the TPS/OTS
 * mouse capture when this is set, so a cursor puzzle always gets the pointer
 * regardless of the active camera. */
int Pc_MouseCursor_PuzzleActive(void);

/* Draw the mouse cursor sprite at the current pointer position. Call during a
 * menu's render (needs the 2D packet buffer + ordering table set up). */
void Pc_MouseCursor_Draw(void);

#endif /* PC_MOUSE_CURSOR_H */
