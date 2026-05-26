#ifndef DBG_OVERLAY_H
#define DBG_OVERLAY_H

#ifdef SH_PC_PORT

void DbgOverlay_Update(void);
void DbgOverlay_Render(void);
void DbgOverlay_PushLine(const char* line); /* callable from SH_DBG_ECHO via g_ShOverlayPushLine */

#endif /* SH_PC_PORT */
#endif /* DBG_OVERLAY_H */
