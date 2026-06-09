/* cat_anim_infos.c — Cat (Chara_Cat = 29) animation info table.
 *
 * Replaces the `u8 CAT_ANIM_INFOS[256] = {0}` zero-stub that data_stubs.c
 * carried for this symbol. With the stub, every entry's playbackFunc was NULL,
 * so the cat's animation never advanced: pre-guard it crashed calling NULL,
 * post-guard (cat.c) the locker cutscene waited forever for the cat anim to
 * finish (frozen at state=122 kf=990) and the cat was effectively dead on
 * screen. Values are the authoritative table from include/maps/characters/cat.h
 * (PSX symbol 0x800DC924).
 *
 * Same seed-table + runtime-init pattern as dahlia_anim_infos.c: MinGW won't
 * accept the exe-resident Anim_* function pointers in a static initialiser, so
 * they are wired in CatAnimInfos_Init() (called from main_pc.c at startup). */

#include "bodyprog/bodyprog.h"
#include "bodyprog/anim.h"
#include "maps/characters/cat.h"

s_AnimInfo CAT_ANIM_INFOS[10];

typedef struct {
    u8  kind; /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} CAT_ANIM_INFOS_Seed;

static const CAT_ANIM_INFOS_Seed s_seeds[10] = {
    { 0, NO_VALUE,                               0, ANIM_STATUS(CatAnim_Still, false),     Q12(0.0f),  NO_VALUE, 0  },
    { 3, ANIM_STATUS(CatAnim_Still, false),      0, ANIM_STATUS(CatAnim_Still, false),     Q12(0.0f),  0,        0  },
    { 0, ANIM_STATUS(CatAnim_Jump, false),       0, ANIM_STATUS(CatAnim_Jump, true),       Q12(64.0f), NO_VALUE, 7  },
    { 1, ANIM_STATUS(CatAnim_Jump, true),        0, ANIM_STATUS(CatAnim_JumpToRun, false), Q12(15.8f), 7,        22 },
    { 0, ANIM_STATUS(CatAnim_Run, false),        0, ANIM_STATUS(CatAnim_Run, true),        Q12(64.0f), NO_VALUE, 23 },
    { 2, ANIM_STATUS(CatAnim_Run, true),         0, NO_VALUE,                              Q12(35.0f), 23,       43 },
    { 0, ANIM_STATUS(CatAnim_IdleToJump, false), 0, ANIM_STATUS(CatAnim_IdleToJump, true), Q12(0.0f),  NO_VALUE, 7  },
    { 2, ANIM_STATUS(CatAnim_IdleToJump, true),  0, NO_VALUE,                              Q12(0.0f),  7,        8  },
    { 0, ANIM_STATUS(CatAnim_JumpToRun, false),  0, ANIM_STATUS(CatAnim_JumpToRun, true),  Q12(0.0f),  NO_VALUE, 22 },
    { 2, ANIM_STATUS(CatAnim_JumpToRun, true),   0, NO_VALUE,                              Q12(0.0f),  22,       23 },
};

void CatAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    int i;
    for (i = 0; i < 10; i++) {
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
