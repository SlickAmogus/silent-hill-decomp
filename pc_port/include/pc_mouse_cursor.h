/* SPDX-License-Identifier: GPL-3.0-or-later */
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
 * Puzzles: every free-cursor puzzle (piano, plate, elevator, door panels)
 * integrates the left analog stick into a cursor position, draws through
 * Gfx_CursorDraw, and confirms with the `enter` bind. Each one calls
 * Pc_MouseCursor_PuzzleOverride between its stick integration and its clamp,
 * which puts the cursor exactly under the mouse pointer like the menus do;
 * Gfx_CursorDraw tells us one is on screen so clicks land as enter/cancel.
 *
 * Menu: the title menu is discrete row-selection, so we hit-test the mouse
 * against the row bands, drive the selection on hover, and confirm on click. */

/* Once per frame from MainLoop, after the controller is built and after
 * Pc_ControlStyleUpdate. Polls the mouse; if a cursor puzzle is on screen,
 * injects its click into g_Controller0. */
void Pc_MouseCursor_FrameUpdate(void);

/* Called from Gfx_CursorDraw: a cursor-driven puzzle drew its cursor this frame
 * at (curX, curY) in framebuffer centre-origin pixels. Arms puzzle click
 * injection (and Pc_MouseCursor_PuzzleActive) for the next few frames, and if
 * the puzzle's clamp held the cursor short of the mouse, warps the OS pointer
 * back onto it so the pointer can't drift past the puzzle's bounds. */
void Pc_MouseCursor_NoteCursorDrawn(int curX, int curY);

/* Called by each cursor puzzle after it adds the stick to its cursor and before
 * it clamps it. While the mouse owns the cursor, overwrites both Q12 positions
 * (framebuffer centre-origin px) with the point under the mouse pointer. A mouse
 * move takes ownership; a real stick deflection hands it back to the pad. */
void Pc_MouseCursor_PuzzleOverride(int* cursorX, int* cursorY);

/* Hit-test a vertical list of menu rows drawn at y = baseY + i*stepY (top-left
 * 320x240 coords, as Gfx_StringSetPosition uses). Returns the hovered row index
 * (0..count-1) or -1. visibleFlags gates selectable rows (bit i); pass ~0u when
 * every row is selectable. *outClicked is set to 1 when the left button was
 * pressed this frame while hovering a valid row. */
int Pc_MouseCursor_MenuRowHover(int baseY, int stepY, int count, unsigned int visibleFlags, int* outClicked);

/* 1 if the mouse moved this frame — use to avoid an idle mouse fighting the
 * keyboard/pad for the menu selection. */
int Pc_MouseCursor_Moved(void);

/* Raw pointer position in the text-authoring space (the coordinates
 * Gfx_StringSetPosition takes). Returns 0 (outputs untouched) when the cursor
 * is disabled/captured/outside the viewport — menus with non-uniform layouts
 * (two panes, scrolling lists, buttons) hit-test with this instead of
 * Pc_MouseCursor_MenuRowHover. */
int Pc_MouseCursor_UiPos(int* outX, int* outY);

/* Normalized 0..1 position inside the presented picture. For overlays that draw
 * in the post-capture GL hook and think in viewport pixels rather than in the
 * game's (mode-dependent) text-authoring space. */
int Pc_MouseCursor_ViewportPos(float* outX, float* outY);

/* Left-button press edge / held state / right-button press edge this frame
 * (0 while disabled or outside the viewport). */
int Pc_MouseCursor_LeftClicked(void);
int Pc_MouseCursor_LeftHeld(void);
int Pc_MouseCursor_RightClicked(void);

/* +1/-1 on a new wheel notch this frame (0 otherwise) — menus use it to
 * adjust the hovered value row / scroll lists. */
int Pc_MouseCursor_WheelStep(void);

/* Every wheel notch since the last frame, signed (+ = up). WheelStep merges
 * notches that land inside its 2-frame latch, which makes a fast spin feel
 * dead in a long list. */
int Pc_MouseCursor_WheelNotches(void);

/* The pad bits (ControllerFlag_*) the mouse is producing right now through
 * mouse binds (key_cross = Mouse1 and the like), d-pad bits mirrored onto the
 * left-stick ones. Panels that handle the mouse themselves drop these from the
 * pad flags they read, or one click acts twice. */
unsigned int Pc_MouseCursor_BoundPadBits(void);

/* 1 while a free-cursor puzzle is on screen. control_style.c releases the TPS/OTS
 * mouse capture when this is set, so a cursor puzzle always gets the pointer
 * regardless of the active camera. */
int Pc_MouseCursor_PuzzleActive(void);

/* A screen that wants the pointer calls this during its render, every frame it
 * should show. The GL panels (Quick Options, bind panel, Yes/No box) show it
 * while open without asking. */
void Pc_MouseCursor_Show(void);

/* Draws the pointer, if shown this frame, above the game picture and every PC
 * panel. Once per frame, last, from the post-capture overlay hook. */
void Pc_MouseCursor_GlDraw(void);

#endif /* PC_MOUSE_CURSOR_H */
