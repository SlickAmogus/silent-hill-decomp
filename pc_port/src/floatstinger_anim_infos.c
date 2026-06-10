/* FLOATSTINGER_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP4_S05.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo FLOATSTINGER_ANIM_INFOS[28];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} FLOATSTINGER_ANIM_INFOS_Seed;

static const FLOATSTINGER_ANIM_INFOS_Seed s_seeds[28] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 16384, -1, 1 },
    { 1, 0x03, 0, 0x12, 131072, 1, 31 },
    { 0, 0x04, 0, 0x05, 20480, -1, 32 },
    { 1, 0x05, 0, 0x12, 81920, 32, 57 },
    { 0, 0x06, 0, 0x07, 40960, -1, 58 },
    { 1, 0x07, 0, 0x16, 81920, 58, 78 },
    { 0, 0x08, 0, 0x09, 81920, -1, 116 },
    { 1, 0x09, 0, 0x0E, 0, 116, 136 },
    { 0, 0x0A, 0, 0x0B, 8192, -1, 85 },
    { 1, 0x0B, 0, 0x06, 16384, 85, 115 },
    { 0, 0x0C, 0, 0x0D, 81920, -1, 116 },
    { 2, 0x0D, 0, 0x16, 0, 116, 136 },
    { 0, 0x0E, 0, 0x0F, 81920, -1, 137 },
    { 2, 0x0F, 0, 0xFF, 0, 137, 158 },
    { 0, 0x10, 0, 0x11, 81920, -1, 159 },
    { 2, 0x11, 0, 0xFF, 81920, 159, 190 },
    { 0, 0x12, 0, 0x13, 20480, -1, 191 },
    { 2, 0x13, 0, 0xFF, 0, 191, 222 },
    { 0, 0x14, 0, 0x15, 81920, -1, 223 },
    { 2, 0x15, 0, 0xFF, 81920, 223, 254 },
    { 0, 0x16, 0, 0x17, 81920, -1, 78 },
    { 2, 0x17, 0, 0xFF, 0, 78, 79 },
    { 0, 0x18, 0, 0x19, 16384, -1, 1 },
    { 1, 0x19, 0, 0x12, 131072, 1, 31 },
    { 3, 0x00, 0, 0x00, -2146624520, 11152, -32755 },
    { 3, 0x24, 52, 0x0D, -2146618012, -30, -10 },  /* PSX playback 0x800D341C not mapped */
};

void FloatstingerAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 28; i++) {
        const FLOATSTINGER_ANIM_INFOS_Seed* s = &s_seeds[i];
        FLOATSTINGER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        FLOATSTINGER_ANIM_INFOS[i].status              = s->status;
        FLOATSTINGER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        FLOATSTINGER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        FLOATSTINGER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        FLOATSTINGER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        FLOATSTINGER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
