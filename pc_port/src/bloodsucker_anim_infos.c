/* bloodsucker_anim_infos.c — Bloodsucker animation info table.
 *
 * Extracted from disc_extract/VIN/MAP3_S03.BIN at PSX 0x800D5ABC (the
 * BLOODSUCKER_ANIM_INFOS symbol in sym.map3_s03.txt). The table is
 * 30 × s_AnimInfo = 480 B on PSX (16 B/entry); on PC s_AnimInfo is
 * 32 B/entry, so we don't blit raw bytes — we seed from a constant
 * descriptor table and patch in the playback function pointers at
 * runtime in BloodsuckerAnimInfos_Init() called from main_pc.c.
 *
 * Replaces the previous `u8 BLOODSUCKER_ANIM_INFOS[256] = {0};` stub.
 * Used by map3_s03 and map7_s02 — both maps' Chara_Bloodsucker.c
 * include the master src/maps/characters/bloodsucker.c which reads
 * BLOODSUCKER_ANIM_INFOS via shared.h's extern declaration. */

#include "bodyprog/bodyprog.h"

s_AnimInfo BLOODSUCKER_ANIM_INFOS[30];

typedef struct {
    u8  kind;   /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} BloodsuckerSeed;

static const BloodsuckerSeed s_seeds[30] = {
    { 0, 0xFF, 0, 0x00, 0,     -1,   0 },
    { 3, 0x00, 0, 0x00, 0,      0,   0 },
    { 0, 0x02, 0, 0x03, 16384, -1,   0 },
    { 1, 0x03, 0, 0x10, 10240,  0,   7 },
    { 0, 0x04, 0, 0x05, 16384, -1,   8 },
    { 1, 0x05, 0, 0x12, 7372,   8,  15 },
    { 0, 0x06, 0, 0x07, 16384, -1,  16 },
    { 1, 0x07, 0, 0x14, 13107, 16,  23 },
    { 0, 0x08, 0, 0x09, 40960, -1,  24 },
    { 2, 0x09, 0, 0xFF, 81920, 24,  42 },
    { 0, 0x0A, 0, 0x0B, 40960, -1,  43 },
    { 2, 0x0B, 0, 0xFF, 81920, 43,  61 },
    { 0, 0x0C, 0, 0x0D, 40960, -1,  62 },
    { 2, 0x0D, 0, 0xFF, 81920, 62,  80 },
    { 0, 0x0E, 0, 0x0F, 40960, -1,  81 },
    { 2, 0x0F, 0, 0xFF, 0,     81, 187 },
    { 0, 0x10, 0, 0x11, 40960, -1, 188 },
    { 2, 0x11, 0, 0xFF, 81920,188, 197 },
    { 0, 0x12, 0, 0x13, 40960, -1, 198 },
    { 2, 0x13, 0, 0xFF, 81920,198, 207 },
    { 0, 0x14, 0, 0x15, 40960, -1, 208 },
    { 2, 0x15, 0, 0xFF, 81920,208, 217 },
    { 0, 0x16, 0, 0x17, 40960, -1, 218 },
    { 2, 0x17, 0, 0xFF, 81920,218, 232 },
    { 0, 0x18, 0, 0x19, 40960, -1, 233 },
    { 2, 0x19, 0, 0xFF, 81920,233, 247 },
    { 0, 0x1A, 0, 0x1B, 40960, -1, 248 },
    { 2, 0x1B, 0, 0xFF, 81920,248, 262 },
    { 0, 0x1C, 0, 0x1D, 40960, -1,  81 },
    { 2, 0x1D, 0, 0xFF, 0,     81, 187 },
};

void BloodsuckerAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 30; i++) {
        const BloodsuckerSeed* s = &s_seeds[i];
        BLOODSUCKER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        BLOODSUCKER_ANIM_INFOS[i].status              = s->status;
        BLOODSUCKER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        BLOODSUCKER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        BLOODSUCKER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        BLOODSUCKER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        BLOODSUCKER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
