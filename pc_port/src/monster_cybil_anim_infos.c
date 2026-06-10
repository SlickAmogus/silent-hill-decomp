/* MONSTER_CYBIL_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP6_S04.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo MONSTER_CYBIL_ANIM_INFOS[44];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} MONSTER_CYBIL_ANIM_INFOS_Seed;

static const MONSTER_CYBIL_ANIM_INFOS_Seed s_seeds[44] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 4096, -1, 0 },
    { 2, 0x03, 1, 0xFF, -2146596712, 0, 23 },
    { 0, 0x04, 0, 0x05, 20480, -1, 24 },
    { 1, 0x05, 0, 0x05, 61440, 24, 35 },
    { 0, 0x06, 0, 0x07, 20480, -1, 36 },
    { 1, 0x07, 0, 0x07, 81920, 36, 46 },
    { 0, 0x08, 0, 0x09, 20480, -1, 47 },
    { 1, 0x09, 0, 0x09, 40960, 47, 72 },
    { 0, 0x0A, 0, 0x0B, 61440, -1, 73 },
    { 1, 0x0B, 0, 0x0B, 204800, 73, 90 },
    { 0, 0x0C, 0, 0x0D, 20480, -1, 91 },
    { 2, 0x0D, 0, 0xFF, 40960, 91, 101 },
    { 0, 0x0E, 0, 0x0F, 40960, -1, 102 },
    { 1, 0x0F, 0, 0x0F, 102400, 102, 158 },
    { 0, 0x10, 0, 0x11, 40960, -1, 159 },
    { 1, 0x11, 0, 0x11, 122880, 159, 191 },
    { 0, 0x12, 0, 0x13, 40960, -1, 192 },
    { 1, 0x13, 0, 0x13, 40960, 192, 203 },
    { 0, 0x14, 0, 0x15, 20480, -1, 215 },
    { 1, 0x15, 0, 0x15, -20480, 204, 215 },
    { 0, 0x16, 0, 0x17, 20480, -1, 226 },
    { 1, 0x17, 0, 0x17, -40960, 216, 226 },
    { 0, 0x18, 0, 0x19, 20480, -1, 227 },
    { 2, 0x19, 0, 0xFF, 20480, 227, 233 },
    { 0, 0x1A, 0, 0x1B, 20480, -1, 234 },
    { 2, 0x1B, 0, 0xFF, 20480, 234, 239 },
    { 0, 0x1C, 0, 0x1D, 40960, -1, 320 },
    { 1, 0x1D, 0, 0x1D, 40960, 240, 320 },
    { 0, 0x1E, 0, 0x1F, 20480, -1, 321 },
    { 1, 0x1F, 0, 0x1F, 20480, 321, 343 },
    { 0, 0x20, 0, 0x21, 40960, -1, 345 },
    { 1, 0x21, 0, 0x21, 40960, 344, 345 },
    { 0, 0x22, 0, 0x23, 40960, -1, 357 },
    { 1, 0x23, 0, 0x23, 40960, 346, 357 },
    { 0, 0x24, 0, 0x25, 20480, -1, 24 },
    { 1, 0x25, 0, 0x25, 20480, 24, 35 },
    { 0, 0x26, 0, 0x27, 20480, -1, 36 },
    { 1, 0x27, 0, 0x27, 40960, 36, 46 },
    { 0, 0x28, 0, 0x29, 20480, -1, 0 },
    { 2, 0x29, 0, 0xFF, 32768, 0, 23 },
    { 0, 0x2A, 0, 0x2B, 20480, -1, 358 },
    { 2, 0x2B, 0, 0xFF, 20480, 358, 373 },
};

void MonsterCybilAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 44; i++) {
        const MONSTER_CYBIL_ANIM_INFOS_Seed* s = &s_seeds[i];
        MONSTER_CYBIL_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        MONSTER_CYBIL_ANIM_INFOS[i].status              = s->status;
        MONSTER_CYBIL_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        MONSTER_CYBIL_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        MONSTER_CYBIL_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        MONSTER_CYBIL_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        MONSTER_CYBIL_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
