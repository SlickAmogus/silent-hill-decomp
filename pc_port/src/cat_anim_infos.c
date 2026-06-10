/* CAT_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP1_S01.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern).
 * Replaces the earlier cat.h-transcribed table: that version was correct
 * except one rounded duration (Q12(15.8)=64716 vs binary 64880). */

#include "bodyprog/bodyprog.h"

s_AnimInfo CAT_ANIM_INFOS[10];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} CAT_ANIM_INFOS_Seed;

static const CAT_ANIM_INFOS_Seed s_seeds[10] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 262144, -1, 7 },
    { 1, 0x03, 0, 0x08, 64880, 7, 22 },
    { 0, 0x04, 0, 0x05, 262144, -1, 23 },
    { 2, 0x05, 0, 0xFF, 143360, 23, 43 },
    { 0, 0x06, 0, 0x07, 0, -1, 7 },
    { 2, 0x07, 0, 0xFF, 0, 7, 8 },
    { 0, 0x08, 0, 0x09, 0, -1, 22 },
    { 2, 0x09, 0, 0xFF, 0, 22, 23 },
};

void CatAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 10; i++) {
        const CAT_ANIM_INFOS_Seed* s = &s_seeds[i];
        CAT_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        CAT_ANIM_INFOS[i].status              = s->status;
        CAT_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        CAT_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        CAT_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        CAT_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        CAT_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
