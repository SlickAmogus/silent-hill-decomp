/* dahlia_anim_infos.c — Dahlia animation info table.
 *
 * Extracted from disc_extract/VIN/MAP2_S01.BIN at PSX
 * DAHLIA_ANIM_INFOS symbol (see configs/USA/maps/sym.map2_s01.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo DAHLIA_ANIM_INFOS[52];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} DAHLIA_ANIM_INFOS_Seed;

static const DAHLIA_ANIM_INFOS_Seed s_seeds[52] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 2, 0x03, 0, 0xFF, 20480, 0, 15 },
    { 0, 0x04, 0, 0x05, 40960, -1, 16 },
    { 2, 0x05, 0, 0xFF, 61440, 16, 44 },
    { 0, 0x06, 0, 0x07, 40960, -1, 45 },
    { 1, 0x07, 0, 0x07, 61440, 45, 85 },
    { 0, 0x08, 0, 0x09, 20480, -1, 86 },
    { 1, 0x09, 0, 0x09, 17203, 86, 109 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 110 },
    { 1, 0x0B, 0, 0x0B, 28672, 110, 175 },
    { 0, 0x0C, 0, 0x0D, 40960, -1, 176 },
    { 1, 0x0D, 0, 0x0D, 28672, 176, 226 },
    { 0, 0x0E, 0, 0x0F, 61440, -1, 227 },
    { 1, 0x0F, 0, 0x0F, 40960, 227, 247 },
    { 0, 0x10, 0, 0x11, 40960, -1, 248 },
    { 2, 0x11, 0, 0xFF, 20480, 248, 263 },
    { 0, 0x12, 0, 0x13, 40960, -1, 264 },
    { 1, 0x13, 0, 0x13, 24576, 264, 304 },
    { 0, 0x14, 0, 0x15, 40960, -1, 305 },
    { 1, 0x15, 0, 0x15, 40960, 305, 335 },
    { 0, 0x16, 0, 0x17, 40960, -1, 336 },
    { 1, 0x17, 0, 0x17, 40960, 336, 349 },
    { 0, 0x18, 0, 0x19, 40960, -1, 350 },
    { 2, 0x19, 0, 0xFF, 40960, 350, 369 },
    { 0, 0x1A, 0, 0x1B, 40960, -1, 370 },
    { 1, 0x1B, 0, 0x1B, 40960, 370, 395 },
    { 0, 0x1C, 0, 0x1D, 40960, -1, 16 },
    { 1, 0x1D, 0, 0x1D, 61440, 16, 80 },
    { 0, 0x1E, 0, 0x1F, 40960, -1, 81 },
    { 1, 0x1F, 0, 0x1F, 40960, 81, 96 },
    { 0, 0x20, 0, 0x21, 40960, -1, 97 },
    { 1, 0x21, 0, 0x21, 40960, 97, 103 },
    { 0, 0x22, 0, 0x23, 40960, -1, 104 },
    { 1, 0x23, 0, 0x23, 40960, 104, 139 },
    { 0, 0x24, 0, 0x25, 40960, -1, 140 },
    { 2, 0x25, 0, 0xFF, 40960, 140, 155 },
    { 0, 0x26, 0, 0x27, 40960, -1, 156 },
    { 1, 0x27, 0, 0x27, 40960, 156, 193 },
    { 0, 0x28, 0, 0x29, 40960, -1, 194 },
    { 2, 0x29, 0, 0xFF, 40960, 194, 209 },
    { 0, 0x2A, 0, 0x2B, 40960, -1, 210 },
    { 1, 0x2B, 0, 0x2B, 40960, 210, 228 },
    { 0, 0x2C, 0, 0x2D, 40960, -1, 229 },
    { 2, 0x2D, 0, 0xFF, 40960, 229, 244 },
    { 0, 0x2E, 0, 0x2F, 40960, -1, 245 },
    { 1, 0x2F, 0, 0x2F, 40960, 245, 259 },
    { 0, 0x30, 0, 0x31, 40960, -1, 260 },
    { 1, 0x31, 0, 0x31, 40960, 260, 296 },
    { 0, 0x32, 0, 0x33, 40960, -1, 260 },
    { 1, 0x33, 0, 0x31, 0, 260, 296 },
};

void DahliaAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 52; i++) {
        const DAHLIA_ANIM_INFOS_Seed* s = &s_seeds[i];
        DAHLIA_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        DAHLIA_ANIM_INFOS[i].status              = s->status;
        DAHLIA_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        DAHLIA_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        DAHLIA_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        DAHLIA_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        DAHLIA_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
