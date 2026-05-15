/* kaufmann_anim_infos.c — Kaufmann animation info table.
 *
 * Extracted from disc_extract/VIN/MAP3_S00.BIN at PSX
 * KAUFMANN_ANIM_INFOS symbol (see configs/USA/maps/sym.map3_s00.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo KAUFMANN_ANIM_INFOS[46];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} KAUFMANN_ANIM_INFOS_Seed;

static const KAUFMANN_ANIM_INFOS_Seed s_seeds[46] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 20480, -1, 0 },
    { 2, 0x03, 0, 0xFF, 20480, 0, 15 },
    { 0, 0x04, 0, 0x05, 40960, -1, 16 },
    { 1, 0x05, 0, 0x05, 40960, 16, 36 },
    { 0, 0x06, 0, 0x07, 20480, -1, 37 },
    { 2, 0x07, 0, 0xFF, 49152, 37, 59 },
    { 0, 0x08, 0, 0x09, 40960, -1, 60 },
    { 1, 0x09, 0, 0x09, 81920, 60, 89 },
    { 0, 0x0A, 0, 0x0B, 20480, -1, 90 },
    { 1, 0x0B, 0, 0x0B, 16384, 90, 104 },
    { 0, 0x0C, 0, 0x0D, 40960, -1, 105 },
    { 1, 0x0D, 0, 0x0D, 20480, 105, 115 },
    { 0, 0x0E, 0, 0x0F, 40960, -1, 116 },
    { 1, 0x0F, 0, 0x0F, 61440, 116, 181 },
    { 0, 0x10, 0, 0x11, 40960, -1, 182 },
    { 1, 0x11, 0, 0x11, 61440, 182, 202 },
    { 0, 0x12, 0, 0x13, 40960, -1, 203 },
    { 1, 0x13, 0, 0x13, 40960, 203, 290 },
    { 0, 0x14, 0, 0x15, 40960, -1, 291 },
    { 1, 0x15, 0, 0x15, 40960, 291, 321 },
    { 0, 0x16, 0, 0x17, 40960, -1, 322 },
    { 1, 0x17, 0, 0x17, 40960, 322, 346 },
    { 0, 0x18, 0, 0x19, 40960, -1, 347 },
    { 1, 0x19, 0, 0x19, 40960, 347, 383 },
    { 0, 0x1A, 0, 0x1B, 40960, -1, 384 },
    { 1, 0x1B, 0, 0x1B, 61440, 384, 404 },
    { 0, 0x1C, 0, 0x1D, 20480, -1, 405 },
    { 2, 0x1D, 0, 0xFF, 20480, 405, 420 },
    { 0, 0x1E, 0, 0x1F, 40960, -1, 0 },
    { 1, 0x1F, 0, 0x1F, 40960, 0, 62 },
    { 0, 0x20, 0, 0x21, 40960, -1, 63 },
    { 1, 0x21, 0, 0x21, 40960, 63, 85 },
    { 0, 0x22, 0, 0x23, 40960, -1, 86 },
    { 2, 0x23, 0, 0xFF, 40960, 86, 101 },
    { 0, 0x24, 0, 0x25, 40960, -1, 102 },
    { 1, 0x25, 0, 0x25, 40960, 102, 201 },
    { 0, 0x26, 0, 0x27, 40960, -1, 202 },
    { 1, 0x27, 0, 0x27, 40960, 202, 225 },
    { 0, 0x28, 0, 0x29, 20480, -1, 226 },
    { 2, 0x29, 0, 0xFF, 20480, 226, 241 },
    { 0, 0x2A, 0, 0x2B, 40960, -1, 242 },
    { 1, 0x2B, 0, 0x2B, 40960, 242, 262 },
    { 0, 0x2C, 0, 0x2D, 40960, -1, 208 },
    { 1, 0x2D, 0, 0x2D, 40960, 208, 290 },
};

void KaufmannAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 46; i++) {
        const KAUFMANN_ANIM_INFOS_Seed* s = &s_seeds[i];
        KAUFMANN_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        KAUFMANN_ANIM_INFOS[i].status              = s->status;
        KAUFMANN_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        KAUFMANN_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        KAUFMANN_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        KAUFMANN_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        KAUFMANN_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
