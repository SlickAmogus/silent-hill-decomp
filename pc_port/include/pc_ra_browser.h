/*
 * pc_ra_browser.h - the main-menu achievement browser.
 *
 * A tall panel in the middle of the title screen listing the whole set:
 * everything already unlocked first, then the locked ones by ascending point
 * value. Each row is the badge on the left with the name over its description.
 * Scroll by dragging with the mouse, the wheel, or the arrow keys; any other
 * button closes it.
 *
 * Opened with the game's Map button, and ONLY from the main menu -- in game
 * that button belongs to the map screen.
 *
 * Input runs on the game thread (Update); drawing runs from the PsyX
 * post-capture hook (Draw), the same split the unlock toast uses.
 */
#ifndef PC_RA_BROWSER_H
#define PC_RA_BROWSER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One achievement, flattened out of rcheevos so the browser TU needs no
 * rcheevos headers. Sizes follow RA's own limits with room to spare. */
typedef struct PcRaAch
{
    char     title[128];
    char     desc[256];
    char     badge[16];
    unsigned points;
    int      unlocked;
} PcRaAch;

/* Fill `out` with the loaded set. Returns the count written, 0 when no set is
 * loaded. Defined in pc_retroachievements.c, which owns the rcheevos client. */
int  Pc_Ra_SnapshotAchievements(PcRaAch* out, int max);

/* Queue a badge PNG download. No-op for a name already requested this session. */
void Pc_Ra_RequestBadge(const char* badgeName);

/* Open on the title screen. No-op if a set is not loaded. */
void Pc_RaBrowser_Open(void);

/* 1 while the panel owns input. The menu must not act on the pad in that case. */
int  Pc_RaBrowser_IsOpen(void);

/* Per-frame input. Game thread.
 *
 * The caller resolves all three signals against the player's own bindings and
 * passes them in, so this module needs neither game headers nor knowledge of
 * the controller config: `closeRequested` is a press edge on Cancel or Map,
 * `up`/`down` are the held movement directions. */
void Pc_RaBrowser_Update(int closeRequested, int up, int down);

/* Per-frame draw from the post-capture hook. Cheap no-op while closed. */
void Pc_RaBrowser_Draw(void);

/* Raw PNG bytes for a badge that finished downloading. Ignored when the panel
 * has no row waiting on this name. */
void Pc_RaBrowser_ProvideBadge(const char* badgeName,
                               const unsigned char* png, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PC_RA_BROWSER_H */
