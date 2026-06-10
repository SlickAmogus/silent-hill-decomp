/* BLOODY_INCUBATOR_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP7_S03.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo BLOODY_INCUBATOR_ANIM_INFOS[24];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} BLOODY_INCUBATOR_ANIM_INFOS_Seed;

static const BLOODY_INCUBATOR_ANIM_INFOS_Seed s_seeds[24] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 1, 0x03, 0, 0x03, 40960, 0, 54 },
    { 0, 0x04, 0, 0x05, 40960, -1, 55 },
    { 1, 0x05, 0, 0x05, 40960, 55, 117 },
    { 0, 0x06, 0, 0x07, 40960, -1, 118 },
    { 1, 0x07, 0, 0x07, 40960, 118, 152 },
    { 0, 0x08, 0, 0x09, 40960, -1, 153 },
    { 1, 0x09, 0, 0x09, 40960, 153, 213 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 214 },
    { 2, 0x0B, 0, 0xFF, 40960, 214, 229 },
    { 0, 0x0C, 0, 0x0D, 40960, -1, 230 },
    { 1, 0x0D, 0, 0x0D, 40960, 230, 260 },
    { 0, 0x0E, 0, 0x0F, 40960, -1, 261 },
    { 2, 0x0F, 0, 0xFF, 40960, 261, 276 },
    { 0, 0x10, 0, 0x11, 40960, -1, 277 },
    { 1, 0x11, 0, 0x11, 40960, 277, 295 },
    { 0, 0x12, 0, 0x13, 40960, -1, 296 },
    { 1, 0x13, 0, 0x13, 32768, 296, 365 },
    { 0, 0x14, 0, 0x15, 40960, -1, 366 },
    { 2, 0x15, 0, 0xFF, 40960, 366, 382 },
    { 0, 0x16, 0, 0x17, 40960, -1, 295 },
    { 1, 0x17, 0, 0x17, 40960, 295, 295 },
};

void BloodyIncubatorAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 24; i++) {
        const BLOODY_INCUBATOR_ANIM_INFOS_Seed* s = &s_seeds[i];
        BLOODY_INCUBATOR_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        BLOODY_INCUBATOR_ANIM_INFOS[i].status              = s->status;
        BLOODY_INCUBATOR_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        BLOODY_INCUBATOR_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        BLOODY_INCUBATOR_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        BLOODY_INCUBATOR_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        BLOODY_INCUBATOR_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
