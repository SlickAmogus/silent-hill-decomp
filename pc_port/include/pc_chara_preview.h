/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CHARA_PREVIEW_H
#define PC_CHARA_PREVIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* From the post-capture GL hook: draws Character Select's model preview into
 * the screen's third column while the screen is open. */
void Pc_CharaPreview_Draw(void);
/* The last frame drew a model, so the screens leave their placeholder off. */
int  Pc_CharaPreview_Showing(void);
/* The highlighted model is not in memory yet and is waiting out the dwell
 * before it is loaded: the screens say so in place of "no preview". */
int  Pc_CharaPreview_Pending(void);

#ifdef __cplusplus
}
#endif

#endif
