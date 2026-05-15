/* hanged_scratcher_anim_infos.c -- Hanged Scratcher animation info table.
 *
 * Extracted from disc_extract/VIN/MAP5_S00.BIN at PSX
 * HANGED_SCRATCHER_ANIM_INFOS (see configs/USA/maps/sym.map5_s00.txt).
 * Replaces u8[256] = {0} stub. Same seed-table + runtime-init pattern. */

#include "bodyprog/bodyprog.h"

s_AnimInfo HANGED_SCRATCHER_ANIM_INFOS[54];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} HANGED_SCRATCHER_ANIM_INFOS_Seed;

static const HANGED_SCRATCHER_ANIM_INFOS_Seed s_seeds[54] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 0, -1, 0 },
    { 1, 0x03, 0, 0x30, 0, 0, 50 },
    { 0, 0x04, 0, 0x05, 0, -1, 51 },
    { 1, 0x05, 0, 0x32, 0, 51, 86 },
    { 0, 0x06, 0, 0x07, 0, -1, 87 },
    { 1, 0x07, 0, 0x34, 0, 87, 97 },
    { 0, 0x08, 0, 0x09, 73728, -1, 98 },
    { 1, 0x09, 0, 0x1C, 79872, 98, 115 },
    { 0, 0x0A, 0, 0x0B, 49152, -1, 116 },
    { 1, 0x0B, 0, 0x1E, 94208, 116, 135 },
    { 0, 0x0C, 0, 0x0D, 16384, -1, 136 },
    { 1, 0x0D, 0, 0x26, 8192, 136, 147 },
    { 0, 0x0E, 0, 0x0F, 16384, -1, 148 },
    { 1, 0x0F, 0, 0x28, 4096, 148, 153 },
    { 0, 0x10, 0, 0x11, 16384, -1, 154 },
    { 1, 0x11, 0, 0x1C, 57344, 154, 165 },
    { 0, 0x12, 0, 0x13, 16384, -1, 168 },
    { 1, 0x13, 0, 0x1E, 0, 168, 175 },
    { 0, 0x14, 0, 0x15, 65536, -1, 176 },
    { 1, 0x15, 0, 0x18, 0, 176, 191 },
    { 0, 0x16, 0, 0x17, 8192, -1, 192 },
    { 1, 0x17, 0, 0x1A, 0, 192, 201 },
    { 0, 0x18, 0, 0x19, 65536, -1, 202 },
    { 2, 0x19, 0, 0xFF, 12288, 202, 215 },
    { 0, 0x1A, 0, 0x1B, 8192, -1, 216 },
    { 2, 0x1B, 0, 0xFF, 12288, 216, 221 },
    { 0, 0x1C, 0, 0x1D, 8192, -1, 222 },
    { 2, 0x1D, 0, 0xFF, 0, 222, 231 },
    { 0, 0x1E, 0, 0x1F, 8192, -1, 232 },
    { 2, 0x1F, 0, 0xFF, 0, 232, 243 },
    { 0, 0x20, 0, 0x21, 65536, -1, 244 },
    { 1, 0x21, 0, 0x1E, 0, 244, 266 },
    { 0, 0x22, 0, 0x23, 32768, -1, 267 },
    { 2, 0x23, 0, 0xFF, 0, 267, 276 },
    { 0, 0x24, 0, 0x25, 16384, -1, 277 },
    { 2, 0x25, 0, 0xFF, 0, 277, 292 },
    { 0, 0x26, 0, 0x27, 65536, -1, 147 },
    { 2, 0x27, 0, 0xFF, 0, 147, 148 },
    { 0, 0x28, 0, 0x29, 65536, -1, 153 },
    { 2, 0x29, 0, 0xFF, 0, 153, 154 },
    { 0, 0x2A, 0, 0x2B, 65536, -1, 136 },
    { 1, 0x2B, 0, 0x18, 65536, 136, 147 },
    { 0, 0x2C, 0, 0x2D, 65536, -1, 148 },
    { 1, 0x2D, 0, 0x1A, 32768, 148, 153 },
    { 0, 0x2E, 0, 0x2F, 16384, -1, 168 },
    { 1, 0x2F, 0, 0x16, 0, 168, 174 },
    { 0, 0x30, 0, 0x31, 0, -1, 50 },
    { 2, 0x31, 0, 0xFF, 0, 50, 51 },
    { 0, 0x32, 0, 0x33, 0, -1, 86 },
    { 2, 0x33, 0, 0xFF, 0, 86, 87 },
    { 0, 0x34, 0, 0x35, 0, -1, 97 },
    { 2, 0x35, 0, 0xFF, 0, 97, 98 },
};

void HangedScratcherAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 54; i++) {
        const HANGED_SCRATCHER_ANIM_INFOS_Seed* s = &s_seeds[i];
        HANGED_SCRATCHER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        HANGED_SCRATCHER_ANIM_INFOS[i].status              = s->status;
        HANGED_SCRATCHER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        HANGED_SCRATCHER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        HANGED_SCRATCHER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        HANGED_SCRATCHER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        HANGED_SCRATCHER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
