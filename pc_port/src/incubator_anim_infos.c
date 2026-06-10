/* INCUBATOR_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP7_S03.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo INCUBATOR_ANIM_INFOS[8];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} INCUBATOR_ANIM_INFOS_Seed;

static const INCUBATOR_ANIM_INFOS_Seed s_seeds[8] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 1, 0x03, 0, 0x03, 24985, 0, 74 },
    { 0, 0x04, 0, 0x05, 40960, -1, 75 },
    { 1, 0x05, 0, 0x05, 40960, 75, 115 },
    { 0, 0x06, 0, 0x07, 40960, -1, 116 },
    { 2, 0x07, 0, 0xFF, 40960, 116, 132 },
};

void IncubatorAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 8; i++) {
        const INCUBATOR_ANIM_INFOS_Seed* s = &s_seeds[i];
        INCUBATOR_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        INCUBATOR_ANIM_INFOS[i].status              = s->status;
        INCUBATOR_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        INCUBATOR_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        INCUBATOR_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        INCUBATOR_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        INCUBATOR_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
