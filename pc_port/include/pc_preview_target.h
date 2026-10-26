/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_PREVIEW_TARGET_H
#define PC_PREVIEW_TARGET_H

/* Render-thread-only, same-context offscreen target. Owns no game/world state.
 * A target must be ended before it is resized or destroyed. GL names must be
 * discarded after context loss; they cannot be reused in a replacement context. */
typedef struct {
    unsigned framebuffer, color, depth;
    int width, height, active;
    int drawFramebuffer, readFramebuffer, viewport[4], scissor[4], scissorEnabled;
} PcPreviewTarget;

int Pc_PreviewTarget_Create(PcPreviewTarget* target, int width, int height);
int Pc_PreviewTarget_Begin(PcPreviewTarget* target);
void Pc_PreviewTarget_End(PcPreviewTarget* target);
void Pc_PreviewTarget_Destroy(PcPreviewTarget* target);
/* Debug validation only: no game content is drawn, and no image is presented. */
int Pc_PreviewTarget_SelfTest(void);

#endif
