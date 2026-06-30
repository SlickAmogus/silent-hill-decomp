#ifndef CONTROL_STYLE_H
#define CONTROL_STYLE_H

/* Camera / control-style system.
 *
 * The game owns a small registry of control styles (Classic, Thirdperson
 * Shooter, ...). The active style is persisted in config.cfg (control_style)
 * and the whole registry is published to config.cfg (control_styles) on boot
 * so the launcher's dropdown always reflects whatever the installed build
 * supports. Adding a style = one entry in g_ControlStyles[] (control_style.c)
 * + its camera/input behaviour; it then appears in the launcher automatically.
 *
 * This replaces the old debug-only Numpad-2 TPS toggle: the style is selected
 * in the launcher and toggled in-game by the rebindable Change-Camera action
 * (key_change_cam / pad_change_cam), with no allow_debug_controls gating. */

typedef enum {
    ControlStyle_Classic = 0,
    ControlStyle_Tps     = 1,
    ControlStyle_Ots     = 2,  /* Over the Shoulder — TPS offset to one side */
    ControlStyle_Fps     = 3,  /* First Person — eye between Harry's arms, forced aim pose */
} e_ControlStyle;

extern int g_ControlStyle; /* active e_ControlStyle */
extern int g_OtsSide;      /* OTS shoulder side: +1 = right, -1 = left */

/* Boot: apply control_style from config + publish the registry to config. */
void Pc_ControlStyleInit(void);

/* Per-frame (in-game, not debug-gated): poll the Change-Camera binding. */
void Pc_ControlStyleUpdate(void);

/* Set the active style, mirror the runtime flags, and persist to config. */
void Pc_ControlStyleSet(int style);

int         Pc_ControlStyleCount(void);
const char* Pc_ControlStyleId(int idx);
const char* Pc_ControlStyleLabel(int idx);

#endif /* CONTROL_STYLE_H */
