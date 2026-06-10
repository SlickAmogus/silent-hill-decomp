/* UNKKOWN_23_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP7_S03.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo UNKKOWN_23_ANIM_INFOS[12];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} UNKKOWN_23_ANIM_INFOS_Seed;

static const UNKKOWN_23_ANIM_INFOS_Seed s_seeds[12] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 81920, -1, 0 },
    { 1, 0x03, 0, 0x0A, 81920, 0, 74 },
    { 0, 0x04, 0, 0x05, 81920, -1, 75 },
    { 1, 0x05, 0, 0x08, 81920, 75, 115 },
    { 0, 0x06, 0, 0x07, 81920, -1, 116 },
    { 2, 0x07, 0, 0xFF, 81920, 116, 132 },
    { 0, 0x08, 0, 0x09, 245760, -1, 115 },
    { 2, 0x09, 0, 0xFF, 0, 115, 115 },
    { 0, 0x0A, 0, 0x0B, 245760, -1, 74 },
    { 2, 0x0B, 0, 0xFF, 0, 74, 74 },
};

void Unkkown23AnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 12; i++) {
        const UNKKOWN_23_ANIM_INFOS_Seed* s = &s_seeds[i];
        UNKKOWN_23_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        UNKKOWN_23_ANIM_INFOS[i].status              = s->status;
        UNKKOWN_23_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        UNKKOWN_23_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        UNKKOWN_23_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        UNKKOWN_23_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        UNKKOWN_23_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
