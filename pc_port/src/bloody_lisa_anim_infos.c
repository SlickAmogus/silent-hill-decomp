/* bloody_lisa_anim_infos.c — Bloody Lisa animation info table.
 *
 * Extracted from disc_extract/VIN/MAP7_S01.BIN at PSX
 * BLOODY_LISA_ANIM_INFOS symbol (see configs/USA/maps/sym.map7_s01.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo BLOODY_LISA_ANIM_INFOS[4];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} BLOODY_LISA_ANIM_INFOS_Seed;

static const BLOODY_LISA_ANIM_INFOS_Seed s_seeds[4] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 1, 0x03, 0, 0x03, 40960, 0, 100 },
};

void BloodyLisaAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 4; i++) {
        const BLOODY_LISA_ANIM_INFOS_Seed* s = &s_seeds[i];
        BLOODY_LISA_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        BLOODY_LISA_ANIM_INFOS[i].status              = s->status;
        BLOODY_LISA_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        BLOODY_LISA_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        BLOODY_LISA_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        BLOODY_LISA_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        BLOODY_LISA_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
