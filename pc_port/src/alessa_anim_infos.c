/* alessa_anim_infos.c — Alessa animation info table.
 *
 * Extracted from disc_extract/VIN/MAP3_S02.BIN at PSX
 * ALESSA_ANIM_INFOS symbol (see configs/USA/maps/sym.map3_s02.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo ALESSA_ANIM_INFOS[22];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} ALESSA_ANIM_INFOS_Seed;

static const ALESSA_ANIM_INFOS_Seed s_seeds[22] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 20480, -1, 0 },
    { 2, 0x03, 0, 0xFF, 20480, 0, 15 },
    { 0, 0x04, 0, 0x05, 20480, -1, 16 },
    { 2, 0x05, 0, 0xFF, 61440, 16, 44 },
    { 0, 0x06, 0, 0x07, 40960, -1, 45 },
    { 1, 0x07, 0, 0x07, 40960, 45, 131 },
    { 0, 0x08, 0, 0x09, 81920, -1, 132 },
    { 1, 0x09, 0, 0x09, 40960, 132, 170 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 171 },
    { 1, 0x0B, 0, 0x0B, 40960, 171, 216 },
    { 0, 0x0C, 0, 0x0D, 4096, -1, 217 },
    { 1, 0x0D, 0, 0x0D, 40960, 217, 237 },
    { 0, 0x0E, 0, 0x0F, 4096, -1, 238 },
    { 1, 0x0F, 0, 0x0F, 40960, 238, 288 },
    { 0, 0x10, 0, 0x11, 40960, -1, 289 },
    { 1, 0x11, 0, 0x11, 40960, 289, 309 },
    { 0, 0x12, 0, 0x13, 20480, -1, 310 },
    { 2, 0x13, 0, 0xFF, 12288, 310, 325 },
    { 0, 0x14, 0, 0x15, 20480, -1, 326 },
    { 2, 0x15, 0, 0xFF, 12288, 326, 341 },
};

void AlessaAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 22; i++) {
        const ALESSA_ANIM_INFOS_Seed* s = &s_seeds[i];
        ALESSA_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        ALESSA_ANIM_INFOS[i].status              = s->status;
        ALESSA_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        ALESSA_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        ALESSA_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        ALESSA_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        ALESSA_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
