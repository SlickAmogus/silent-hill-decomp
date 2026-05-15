/* ghost_child_alessa_anim_infos.c — Ghost Child Alessa animation info table.
 *
 * Extracted from disc_extract/VIN/MAP7_S01.BIN at PSX
 * GHOST_CHILD_ALESSA_ANIM_INFOS symbol (see configs/USA/maps/sym.map7_s01.txt). Replaces the
 * previous `u8[256] = {0}` stub. Same seed-table + runtime-init
 * pattern as groaner_anim_infos.c — MinGW won't accept exe-resident
 * function pointers in static initialisers. */

#include "bodyprog/bodyprog.h"

s_AnimInfo GHOST_CHILD_ALESSA_ANIM_INFOS[18];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} GHOST_CHILD_ALESSA_ANIM_INFOS_Seed;

static const GHOST_CHILD_ALESSA_ANIM_INFOS_Seed s_seeds[18] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 20480, -1, 0 },
    { 2, 0x03, 0, 0xFF, 61440, 0, 27 },
    { 0, 0x04, 0, 0x05, 20480, -1, 28 },
    { 2, 0x05, 0, 0xFF, 61440, 28, 49 },
    { 0, 0x06, 0, 0x07, 40960, -1, 50 },
    { 1, 0x07, 0, 0x07, 40960, 50, 63 },
    { 0, 0x08, 0, 0x09, 40960, -1, 64 },
    { 2, 0x09, 0, 0xFF, 40960, 64, 83 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 84 },
    { 1, 0x0B, 0, 0x0B, 40960, 84, 109 },
    { 0, 0x0C, 0, 0x0D, 20480, -1, 110 },
    { 2, 0x0D, 0, 0xFF, 20480, 110, 111 },
    { 0, 0x0E, 0, 0x0F, 20480, -1, 112 },
    { 2, 0x0F, 0, 0xFF, 20480, 112, 127 },
    { 0, 0x10, 0, 0x11, 20480, -1, 128 },
    { 2, 0x11, 0, 0xFF, 20480, 128, 137 },
};

void GhostChildAlessaAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 18; i++) {
        const GHOST_CHILD_ALESSA_ANIM_INFOS_Seed* s = &s_seeds[i];
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].status              = s->status;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        GHOST_CHILD_ALESSA_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
