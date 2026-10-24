/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_TITLE_SCREEN_H
#define PC_TITLE_SCREEN_H

/* The compositor lends drawing primitives, not its atlas or menu state.
 * Coordinates are viewport pixels. GL state is owned/restored by the caller. */
typedef struct {
    float width, height;
    void (*image)(unsigned texture, float x, float y, float w, float h,
                  float vpW, float vpH, float r, float g, float b, float opacity);
    void (*rect)(float x, float y, float w, float h, float vpW, float vpH,
                 float r, float g, float b, float opacity);
    void (*text)(int slot, const char* text, float px, float x, float y,
                 float maxWidth, float vpW, float vpH, float r, float g, float b);
} PcTitlePainter;

int Pc_TitleScreen_Available(void);
void Pc_TitleScreen_Draw(const PcTitlePainter* painter);
/* Shared layout for the original title state machine's mouse input. */
int Pc_TitleMenu_MouseRow(int count);
/* Compositor availability, implemented by the overlay owner. */
int Pc_TitleMenu_Active(void);

#endif
