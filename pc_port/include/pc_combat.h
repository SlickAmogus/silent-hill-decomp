#pragma once
#ifdef SH_PC_PORT

bool PC_PlayerManualReloadRequested(void);

/* Rising-edge detection for a raw keyboard scancode. Returns true on the
 * frame the key transitions 0→1. Each scancode tracked separately. Used
 * for PC convenience hotkeys (M=map, I=inventory, R=reload) that live
 * outside the PSX controller mapping. */
bool PC_KeyboardKeyClicked(int sdlScancode);

#endif /* SH_PC_PORT */
