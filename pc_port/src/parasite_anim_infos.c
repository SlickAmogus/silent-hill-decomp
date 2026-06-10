/* PARASITE_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP6_S04.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo PARASITE_ANIM_INFOS[6];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} PARASITE_ANIM_INFOS_Seed;

static const PARASITE_ANIM_INFOS_Seed s_seeds[6] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 2, 0x03, 0, 0xFF, 40960, 0, 19 },
    { 0, 0x04, 0, 0x05, 40960, -1, 20 },
    { 2, 0x05, 0, 0xFF, 40960, 20, 70 },
};

void ParasiteAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 6; i++) {
        const PARASITE_ANIM_INFOS_Seed* s = &s_seeds[i];
        PARASITE_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        PARASITE_ANIM_INFOS[i].status              = s->status;
        PARASITE_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        PARASITE_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        PARASITE_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        PARASITE_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        PARASITE_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
