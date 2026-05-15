/* larval_stalker_anim_infos.c -- Larval Stalker (small grey-children) anim info.
 *
 * Extracted from disc_extract/VIN/MAP1_S00.BIN at PSX 0x800DA6C8.
 * Replaces `u8 LARVAL_STALKER_ANIM_INFOS[256] = {0};` stub in data_stubs.c.
 * Same seed-table + runtime-init pattern. LarvalStalker was already in the
 * isFullAiNpc whitelist but reading from a zero-stub anim_info table would
 * NULL-call its playbackFunc on first invocation -- this fixes it. */

#include "bodyprog/bodyprog.h"

s_AnimInfo LARVAL_STALKER_ANIM_INFOS[38];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} LARVAL_STALKER_ANIM_INFOS_Seed;

static const LARVAL_STALKER_ANIM_INFOS_Seed s_seeds[38] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 32768, -1, 0 },
    { 2, 0x03, 0, 0xFF, 65536, 0, 11 },
    { 0, 0x04, 0, 0x05, 65536, -1, 12 },
    { 2, 0x05, 0, 0xFF, 4096, 12, 21 },
    { 0, 0x06, 0, 0x07, 32768, -1, 22 },
    { 1, 0x07, 0, 0x1C, 8192, 22, 32 },
    { 0, 0x08, 0, 0x09, 32768, -1, 33 },
    { 1, 0x09, 0, 0x1A, 8192, 33, 40 },
    { 0, 0x0A, 0, 0x0B, 65536, -1, 41 },
    { 1, 0x0B, 0, 0x14, 65536, 41, 85 },
    { 0, 0x0C, 0, 0x0D, 65536, -1, 86 },
    { 1, 0x0D, 0, 0x14, 40960, 86, 120 },
    { 0, 0x0E, 0, 0x0F, 65536, -1, 121 },
    { 1, 0x0F, 0, 0x04, 40960, 121, 142 },
    { 0, 0x10, 0, 0x11, 65536, -1, 143 },
    { 1, 0x11, 0, 0x12, 40960, 143, 153 },
    { 0, 0x12, 0, 0x13, 65536, -1, 154 },
    { 2, 0x13, 0, 0xFF, 16384, 154, 161 },
    { 0, 0x14, 0, 0x15, 4096, -1, 162 },
    { 2, 0x15, 0, 0xFF, 16384, 162, 184 },
    { 0, 0x16, 0, 0x17, 32768, -1, 185 },
    { 2, 0x17, 0, 0xFF, 68812, 185, 200 },
    { 0, 0x18, 0, 0x19, 65536, -1, 185 },
    { 2, 0x19, 0, 0xFF, 0, 185, 200 },
    { 0, 0x1A, 0, 0x1B, 65536, -1, 40 },
    { 2, 0x1B, 0, 0xFF, 0, 40, 41 },
    { 0, 0x1C, 0, 0x1D, 65536, -1, 32 },
    { 2, 0x1D, 0, 0xFF, 0, 32, 33 },
    { 0, 0x1E, 0, 0x1F, 0, -1, 41 },
    { 1, 0x1F, 0, 0x14, 0, 41, 85 },
    { 0, 0x20, 0, 0x21, 0, -1, 86 },
    { 1, 0x21, 0, 0x14, 0, 86, 120 },
    { 0, 0x22, 0, 0x23, 65536, -1, 154 },
    { 1, 0x23, 0, 0x12, 49152, 154, 161 },
    { 0, 0x24, 0, 0x25, 65536, -1, 22 },
    { 1, 0x25, 0, 0x04, 32768, 22, 32 },
};

void LarvalStalkerAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 38; i++) {
        const LARVAL_STALKER_ANIM_INFOS_Seed* s = &s_seeds[i];
        LARVAL_STALKER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        LARVAL_STALKER_ANIM_INFOS[i].status              = s->status;
        LARVAL_STALKER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        LARVAL_STALKER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        LARVAL_STALKER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        LARVAL_STALKER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        LARVAL_STALKER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
