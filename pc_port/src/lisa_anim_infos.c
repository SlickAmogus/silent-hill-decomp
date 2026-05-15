/* lisa_anim_infos.c — Lisa animation info table.
 *
 * Extracted from disc_extract/VIN/MAP3_S04.BIN at PSX
 * LISA_ANIM_INFOS symbol (see configs/USA/maps/sym.map3_s04.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo LISA_ANIM_INFOS[36];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} LISA_ANIM_INFOS_Seed;

static const LISA_ANIM_INFOS_Seed s_seeds[36] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 8192, -1, 0 },
    { 2, 0x03, 0, 0xFF, 20480, 0, 9 },
    { 0, 0x04, 0, 0x05, 20480, -1, 10 },
    { 2, 0x05, 0, 0xFF, 81920, 10, 37 },
    { 0, 0x06, 0, 0x07, 40960, -1, 38 },
    { 1, 0x07, 0, 0x07, 6144, 38, 58 },
    { 0, 0x08, 0, 0x09, 40960, -1, 59 },
    { 1, 0x09, 0, 0x09, 20480, 59, 72 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 73 },
    { 1, 0x0B, 0, 0x0B, 36864, 73, 189 },
    { 0, 0x0C, 0, 0x0D, 20480, -1, 190 },
    { 2, 0x0D, 0, 0xFF, 20480, 190, 205 },
    { 0, 0x0E, 0, 0x0F, 40960, -1, 206 },
    { 1, 0x0F, 0, 0x0F, 40960, 206, 227 },
    { 0, 0x10, 0, 0x11, 40960, -1, 228 },
    { 1, 0x11, 0, 0x11, 40960, 228, 248 },
    { 0, 0x12, 0, 0x13, 40960, -1, 249 },
    { 1, 0x13, 0, 0x13, 32768, 249, 283 },
    { 0, 0x14, 0, 0x15, 40960, -1, 284 },
    { 1, 0x15, 0, 0x15, 40960, 284, 353 },
    { 0, 0x16, 0, 0x17, 40960, -1, 354 },
    { 1, 0x17, 0, 0x17, 14336, 354, 403 },
    { 0, 0x18, 0, 0x19, 40960, -1, 404 },
    { 1, 0x19, 0, 0x19, 32768, 404, 415 },
    { 0, 0x1A, 0, 0x1B, 32768, -1, 416 },
    { 1, 0x1B, 0, 0x1B, 30720, 416, 436 },
    { 0, 0x1C, 0, 0x1D, 32768, -1, 437 },
    { 1, 0x1D, 0, 0x1D, 40960, 437, 547 },
    { 0, 0x1E, 0, 0x1F, 40960, -1, 548 },
    { 1, 0x1F, 0, 0x1F, 40960, 548, 572 },
    { 0, 0x20, 0, 0x21, 40960, -1, 573 },
    { 1, 0x21, 0, 0x21, 40960, 573, 602 },
    { 0, 0x22, 0, 0x23, 8192, -1, 603 },
    { 2, 0x23, 0, 0xFF, 20480, 603, 618 },
};

void LisaAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 36; i++) {
        const LISA_ANIM_INFOS_Seed* s = &s_seeds[i];
        LISA_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        LISA_ANIM_INFOS[i].status              = s->status;
        LISA_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        LISA_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        LISA_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        LISA_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        LISA_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
