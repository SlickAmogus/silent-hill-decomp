/* split_head_anim_infos.c -- Split Head (hospital boss) animation info table.
 *
 * Extracted from disc_extract/VIN/MAP1_S05.BIN at PSX SPLIT_HEAD_ANIM_INFOS
 * (0x800D5888; see configs/USA/maps/sym.map1_s05.txt).
 *
 * Replaces u8[256] = {0} stub. Same seed-table + runtime-init pattern as
 * GROANER_ANIM_INFOS -- function pointers can't be in static initialisers
 * under MinGW for symbols imported from another TU, so the array is zero-
 * initialised then patched in SplitHeadAnimInfos_Init() from main_pc.c.
 *
 * map1_s06 has its own SPLIT_HEAD_ANIM_INFOS (0x800D6E34) but the table
 * is the same encoding -- one shared exe-resident copy is fine since the
 * data describes the Split Head model itself, not map geometry. */

#include "bodyprog/bodyprog.h"

s_AnimInfo SPLIT_HEAD_ANIM_INFOS[30];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} SPLIT_HEAD_ANIM_INFOS_Seed;

static const SPLIT_HEAD_ANIM_INFOS_Seed s_seeds[30] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 8192, -1, 0 },
    { 2, 0x03, 0, 0xFF, 14336, 0, 13 },
    { 0, 0x04, 0, 0x05, 32768, -1, 14 },
    { 1, 0x05, 0, 0x14, 0, 14, 40 },
    { 0, 0x06, 0, 0x07, 73728, -1, 41 },
    { 1, 0x07, 0, 0x14, 49152, 41, 50 },
    { 0, 0x08, 0, 0x09, 49152, -1, 51 },
    { 1, 0x09, 0, 0x14, 49152, 51, 60 },
    { 0, 0x0A, 0, 0x0B, 49152, -1, 61 },
    { 1, 0x0B, 0, 0x0C, 49152, 61, 63 },
    { 0, 0x0C, 0, 0x0D, 49152, -1, 78 },
    { 1, 0x0D, 0, 0x14, 49152, 78, 80 },
    { 0, 0x0E, 0, 0x0F, 16384, -1, 94 },
    { 1, 0x0F, 0, 0x18, 8192, 94, 114 },
    { 0, 0x10, 0, 0x11, 2048, -1, 115 },
    { 1, 0x11, 0, 0x1A, 6144, 115, 131 },
    { 0, 0x12, 0, 0x13, 16384, -1, 132 },
    { 1, 0x13, 0, 0x02, 0, 132, 161 },
    { 0, 0x14, 0, 0x15, 32768, -1, 162 },
    { 2, 0x15, 0, 0xFF, 24576, 162, 171 },
    { 0, 0x16, 0, 0x17, 16384, -1, 172 },
    { 2, 0x17, 0, 0xFF, 0, 172, 201 },
    { 0, 0x18, 0, 0x19, 32768, -1, 114 },
    { 2, 0x19, 0, 0xFF, 0, 114, 115 },
    { 0, 0x1A, 0, 0x1B, 32768, -1, 131 },
    { 2, 0x1B, 0, 0xFF, 0, 131, 132 },
    { 0, 0x1C, 0, 0x1D, 8192, -1, 34 },
    { 2, 0x1D, 0, 0xFF, 8192, 34, 36 },
};

void SplitHeadAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 30; i++) {
        const SPLIT_HEAD_ANIM_INFOS_Seed* s = &s_seeds[i];
        SPLIT_HEAD_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        SPLIT_HEAD_ANIM_INFOS[i].status              = s->status;
        SPLIT_HEAD_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        SPLIT_HEAD_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        SPLIT_HEAD_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        SPLIT_HEAD_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        SPLIT_HEAD_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
