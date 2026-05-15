/* romper_anim_infos.c -- Romper (Cybil-arena dog enemy) animation info table.
 *
 * Extracted from disc_extract/VIN/MAP2_S02.BIN at PSX ROPMER_ANIM_INFOS
 * (0x800EC954; see configs/USA/maps/sym.map2_s02.txt). 44 entries -- the
 * sym table places `sharedData_800ECA4C` at 0xF8 past ROPMER_ANIM_INFOS
 * which would imply 15 entries, but that's spurious symbol overlap; the
 * real table extends to 44 entries (statuses 0xFF, 0x00, 0x02..0x2B).
 * Matches the commented-out PSX reference in include/maps/characters/romper.h.
 *
 * Replaces u8[256] = {0} stub. Same seed-table + runtime-init pattern --
 * MinGW can't put cross-TU function pointers in static initialisers.
 *
 * Romper appears in 4 maps (map2_s02, map4_s02, map5_s01, map6_s00).
 * Each has its own copy in PSX rodata but the data describes the Romper
 * model itself, so one shared exe-resident copy is fine. */

#include "bodyprog/bodyprog.h"

s_AnimInfo ROPMER_ANIM_INFOS[44];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} ROPMER_ANIM_INFOS_Seed;

static const ROPMER_ANIM_INFOS_Seed s_seeds[44] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 81920, -1, 0 },
    { 2, 0x03, 0, 0xFF, 40960, 0, 1 },
    { 0, 0x04, 0, 0x05, 73728, -1, 2 },
    { 1, 0x05, 0, 0x08, 49152, 2, 7 },
    { 0, 0x06, 0, 0x07, 61440, -1, 8 },
    { 1, 0x07, 0, 0x0A, 61440, 8, 14 },
    { 0, 0x08, 0, 0x09, 81920, -1, 15 },
    { 1, 0x09, 0, 0x1A, 49152, 15, 19 },
    { 0, 0x0A, 0, 0x0B, 61440, -1, 20 },
    { 2, 0x0B, 0, 0xFF, 61440, 20, 33 },
    { 0, 0x0C, 0, 0x0D, 81920, -1, 34 },
    { 1, 0x0D, 0, 0x20, 20480, 34, 38 },
    { 0, 0x0E, 0, 0x0F, 40960, -1, 39 },
    { 1, 0x0F, 0, 0x1A, 32768, 39, 49 },
    { 0, 0x10, 0, 0x11, 40960, -1, 50 },
    { 1, 0x11, 0, 0x1A, 32768, 50, 60 },
    { 0, 0x12, 0, 0x13, 40960, -1, 61 },
    { 1, 0x13, 0, 0x2A, 40960, 61, 87 },
    { 0, 0x14, 0, 0x15, 81920, -1, 88 },
    { 2, 0x15, 0, 0xFF, 20480, 88, 92 },
    { 0, 0x16, 0, 0x17, 16384, -1, 93 },
    { 1, 0x17, 0, 0x14, 26624, 93, 99 },
    { 0, 0x18, 0, 0x19, 40960, -1, 100 },
    { 2, 0x19, 0, 0xFF, 24576, 100, 108 },
    { 0, 0x1A, 0, 0x1B, 81920, -1, 109 },
    { 2, 0x1B, 0, 0xFF, 0, 109, 130 },
    { 0, 0x1C, 0, 0x1D, 24576, -1, 131 },
    { 2, 0x1D, 0, 0xFF, 0, 131, 146 },
    { 0, 0x1E, 0, 0x1F, 65536, -1, 147 },
    { 2, 0x1F, 0, 0xFF, 16384, 147, 162 },
    { 0, 0x20, 0, 0x21, 65536, -1, 38 },
    { 2, 0x21, 0, 0xFF, 0, 38, 39 },
    { 0, 0x22, 0, 0x23, 0, -1, 120 },
    { 1, 0x23, 0, 0x0E, 0, 120, 127 },
    { 0, 0x24, 0, 0x25, 0, -1, 110 },
    { 1, 0x25, 0, 0x10, 0, 110, 116 },
    { 0, 0x26, 0, 0x27, 49152, -1, 109 },
    { 1, 0x27, 0, 0x1A, 0, 109, 130 },
    { 0, 0x28, 0, 0x29, 16384, -1, 109 },
    { 1, 0x29, 0, 0x1A, 0, 109, 130 },
    { 0, 0x2A, 0, 0x2B, 8192, -1, 18 },
    { 1, 0x2B, 0, 0x18, 24576, 18, 19 },
};

void RopmerAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 44; i++) {
        const ROPMER_ANIM_INFOS_Seed* s = &s_seeds[i];
        ROPMER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        ROPMER_ANIM_INFOS[i].status              = s->status;
        ROPMER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        ROPMER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        ROPMER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        ROPMER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        ROPMER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
