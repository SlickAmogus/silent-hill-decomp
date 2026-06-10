/* INCUBUS_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP7_S03.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo INCUBUS_ANIM_INFOS[12];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} INCUBUS_ANIM_INFOS_Seed;

static const INCUBUS_ANIM_INFOS_Seed s_seeds[12] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 81920, -1, 0 },
    { 1, 0x03, 0, 0x06, 40960, 0, 104 },
    { 0, 0x04, 0, 0x05, 40960, -1, 105 },
    { 1, 0x05, 0, 0x08, 40960, 105, 313 },
    { 0, 0x06, 0, 0x07, 40960, -1, 314 },
    { 2, 0x07, 0, 0xFF, 81920, 314, 338 },
    { 0, 0x08, 0, 0x09, 245760, -1, 313 },
    { 2, 0x09, 0, 0xFF, 0, 313, 313 },
    { 0, 0x0A, 0, 0x0B, 245760, -1, 338 },
    { 2, 0x0B, 0, 0xFF, 0, 338, 338 },
};

void IncubusAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 12; i++) {
        const INCUBUS_ANIM_INFOS_Seed* s = &s_seeds[i];
        INCUBUS_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        INCUBUS_ANIM_INFOS[i].status              = s->status;
        INCUBUS_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        INCUBUS_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        INCUBUS_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        INCUBUS_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        INCUBUS_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
