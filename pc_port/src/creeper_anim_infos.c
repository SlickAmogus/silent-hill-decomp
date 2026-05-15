/* creeper_anim_infos.c -- Creeper animation info table.
 *
 * Extracted from disc_extract/VIN/MAP1_S02.BIN at PSX
 * CREEPER_ANIM_INFOS (see configs/USA/maps/sym.map1_s02.txt).
 * Replaces u8[256] = {0} stub. Same seed-table + runtime-init pattern. */

#include "bodyprog/bodyprog.h"

s_AnimInfo CREEPER_ANIM_INFOS[36];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} CREEPER_ANIM_INFOS_Seed;

static const CREEPER_ANIM_INFOS_Seed s_seeds[36] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 262144, -1, 0 },
    { 1, 0x03, 0, 0x1A, 65536, 0, 3 },
    { 0, 0x04, 0, 0x05, 262144, -1, 4 },
    { 1, 0x05, 0, 0x02, 110592, 4, 14 },
    { 0, 0x06, 0, 0x07, 65536, -1, 15 },
    { 1, 0x07, 0, 0x1C, 16384, 15, 20 },
    { 0, 0x08, 0, 0x09, 32768, -1, 22 },
    { 1, 0x09, 0, 0x1E, 16384, 22, 27 },
    { 0, 0x0A, 0, 0x0B, 65536, -1, 29 },
    { 1, 0x0B, 0, 0x16, 65536, 29, 40 },
    { 0, 0x0C, 0, 0x0D, 65536, -1, 41 },
    { 1, 0x0D, 0, 0x16, 65536, 41, 49 },
    { 0, 0x0E, 0, 0x0F, 65536, -1, 50 },
    { 1, 0x0F, 0, 0x12, 135168, 50, 63 },
    { 0, 0x10, 0, 0x11, 65536, -1, 64 },
    { 1, 0x11, 0, 0x14, 61440, 64, 70 },
    { 0, 0x12, 0, 0x13, 131072, -1, 71 },
    { 2, 0x13, 0, 0xFF, 61440, 71, 83 },
    { 0, 0x14, 0, 0x15, 131072, -1, 84 },
    { 2, 0x15, 0, 0xFF, 81920, 84, 93 },
    { 0, 0x16, 0, 0x17, 65536, -1, 94 },
    { 2, 0x17, 0, 0xFF, 16384, 94, 105 },
    { 0, 0x18, 0, 0x19, 262144, -1, 106 },
    { 1, 0x19, 0, 0x04, 65536, 106, 111 },
    { 0, 0x1A, 0, 0x1B, 65536, -1, 112 },
    { 2, 0x1B, 0, 0xFF, 141312, 112, 131 },
    { 0, 0x1C, 0, 0x1D, 131072, -1, 20 },
    { 2, 0x1D, 0, 0xFF, 819, 20, 21 },
    { 0, 0x1E, 0, 0x1F, 131072, -1, 27 },
    { 2, 0x1F, 0, 0xFF, 819, 27, 28 },
    { 0, 0x20, 0, 0x21, 65536, -1, 50 },
    { 1, 0x21, 0, 0x06, 90112, 50, 63 },
    { 0, 0x22, 0, 0x23, 65536, -1, 64 },
    { 1, 0x23, 0, 0x08, 40960, 64, 70 },
};

void CreeperAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 36; i++) {
        const CREEPER_ANIM_INFOS_Seed* s = &s_seeds[i];
        CREEPER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        CREEPER_ANIM_INFOS[i].status              = s->status;
        CREEPER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        CREEPER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        CREEPER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        CREEPER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        CREEPER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
