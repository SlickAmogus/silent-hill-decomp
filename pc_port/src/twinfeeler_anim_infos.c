/* TWINFEELER_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP4_S03.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo TWINFEELER_ANIM_INFOS[48];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} TWINFEELER_ANIM_INFOS_Seed;

static const TWINFEELER_ANIM_INFOS_Seed s_seeds[48] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 81920, -1, 0 },
    { 1, 0x03, 0, 0x24, 65536, 0, 31 },
    { 0, 0x04, 0, 0x05, 32768, -1, 32 },
    { 1, 0x05, 0, 0x24, 57344, 32, 72 },
    { 0, 0x06, 0, 0x07, 81920, -1, 73 },
    { 1, 0x07, 0, 0x26, 57344, 73, 104 },
    { 0, 0x08, 0, 0x09, 81920, -1, 105 },
    { 1, 0x09, 0, 0x26, 57344, 105, 130 },
    { 0, 0x0A, 0, 0x0B, 81920, -1, 131 },
    { 2, 0x0B, 0, 0xFF, 57344, 131, 162 },
    { 0, 0x0C, 0, 0x0D, 81920, -1, 163 },
    { 2, 0x0D, 0, 0xFF, 57344, 163, 194 },
    { 0, 0x0E, 0, 0x0F, 81920, -1, 195 },
    { 1, 0x0F, 0, 0x28, 57344, 195, 226 },
    { 0, 0x10, 0, 0x11, 40960, -1, 227 },
    { 2, 0x11, 0, 0xFF, 57344, 227, 258 },
    { 0, 0x12, 0, 0x13, 81920, -1, 259 },
    { 1, 0x13, 0, 0x24, 57344, 259, 290 },
    { 0, 0x14, 0, 0x15, 81920, -1, 291 },
    { 1, 0x15, 0, 0x24, 57344, 291, 322 },
    { 0, 0x16, 0, 0x17, 81920, -1, 323 },
    { 1, 0x17, 0, 0x24, 57344, 323, 354 },
    { 0, 0x18, 0, 0x19, 81920, -1, 355 },
    { 1, 0x19, 0, 0x24, 57344, 355, 386 },
    { 0, 0x1A, 0, 0x1B, 81920, -1, 387 },
    { 1, 0x1B, 0, 0x24, 57344, 387, 407 },
    { 0, 0x1C, 0, 0x1D, 81920, -1, 408 },
    { 1, 0x1D, 0, 0x24, 57344, 408, 428 },
    { 0, 0x1E, 0, 0x1F, 81920, -1, 429 },
    { 1, 0x1F, 0, 0x2A, 57344, 429, 459 },
    { 0, 0x20, 0, 0x21, 81920, -1, 460 },
    { 1, 0x21, 0, 0x2C, 57344, 460, 490 },
    { 0, 0x22, 0, 0x23, 81920, -1, 491 },
    { 1, 0x23, 0, 0x24, 122880, 491, 521 },
    { 0, 0x24, 0, 0x25, 245760, -1, 258 },
    { 2, 0x25, 0, 0xFF, 0, 258, 258 },
    { 0, 0x26, 0, 0x27, 245760, -1, 104 },
    { 2, 0x27, 0, 0xFF, 0, 104, 104 },
    { 0, 0x28, 0, 0x29, 245760, -1, 226 },
    { 2, 0x29, 0, 0xFF, 0, 226, 226 },
    { 0, 0x2A, 0, 0x2B, 245760, -1, 459 },
    { 2, 0x2B, 0, 0xFF, 0, 459, 459 },
    { 0, 0x2C, 0, 0x2D, 245760, -1, 490 },
    { 2, 0x2D, 0, 0xFF, 0, 490, 490 },
    { 0, 0x2E, 0, 0x2F, 81920, -1, 0 },
    { 1, 0x2F, 0, 0x24, 53248, 0, 31 },
};

void TwinfeelerAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 48; i++) {
        const TWINFEELER_ANIM_INFOS_Seed* s = &s_seeds[i];
        TWINFEELER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        TWINFEELER_ANIM_INFOS[i].status              = s->status;
        TWINFEELER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        TWINFEELER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        TWINFEELER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        TWINFEELER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        TWINFEELER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
