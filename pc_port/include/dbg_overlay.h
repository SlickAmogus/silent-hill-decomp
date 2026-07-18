#ifndef DBG_OVERLAY_H
#define DBG_OVERLAY_H

#ifdef SH_PC_PORT

void DbgOverlay_Update(void);
void DbgOverlay_Render(void);
void DbgOverlay_PushLine(const char* line);  /* console buffer only (SH_LOG/SH_WARN via g_ShOverlayPushLine) */
void DbgOverlay_ToastLine(const char* line); /* + top-left toast when hidden (SH_DBG_ECHO via g_ShOverlayToastLine) */

/* Snapshot of collState fields at the player's func_8006A4A8 collision pass,
 * filled by collision.c when the visualizer (') is on, shown in the overlay's
 * collState panel. All values are the raw collState fields. */
typedef struct {
    int valid;       /* set each frame the player's func_8006A4A8 ran */
    int radius;      /* field_4.field_28 (collision radius) */
    int respActive;  /* field_44.field_0.field_0 (wall response active) */
    int angMin;      /* field_44.field_0.field_2.vx (angle constraint min) */
    int angMax;      /* field_44.field_0.field_2.vy (angle constraint max) */
    int faceIdx;     /* field_CC.field_4 (last evaluated face index) */
    int dist;        /* field_CC.field_20.field_C (player-to-face distance) */
    int pushX;       /* collResult->offset_0.vx (resolved push X) */
    int pushZ;       /* collResult->offset_0.vz (resolved push Z) */
    int groundFlag;  /* field_90 (1 = no ground found) */
    int groundType;  /* field_94 (ground face type) */
    int corner;      /* func_8006A4A8 corner-collision flag */
    int hold;        /* frames to latch a contact snapshot (anti-flicker) */
} s_CollStateDbg;

extern s_CollStateDbg g_CollStateDbg;

#endif /* SH_PC_PORT */
#endif /* DBG_OVERLAY_H */
