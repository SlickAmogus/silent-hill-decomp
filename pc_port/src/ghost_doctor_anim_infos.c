/* GHOST_DOCTOR_ANIM_INFOS — binary-extracted from disc_extract/VIN/MAP7_S02.BIN by
 * pc_port/tools/extract_anim_infos.py (seed + runtime Init pattern:
 * MinGW rejects cross-module function pointers in static initialisers).
 * Replaces the u8[256]={0} zero-stub that left this character
 * frozen/invisible/crashing. */

#include "bodyprog/bodyprog.h"

s_AnimInfo GHOST_DOCTOR_ANIM_INFOS[4];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} GHOST_DOCTOR_ANIM_INFOS_Seed;

static const GHOST_DOCTOR_ANIM_INFOS_Seed s_seeds[4] = {
    { 0, 0x00, 0, 0x00, 0, -1, 0 },
    { 2, 0x01, 0, 0xFF, 122880, -1, 1 },
    { 0, 0x02, 0, 0x03, 20480, -1, 0 },
    { 2, 0x03, 0, 0xFF, 61440, 0, 15 },
};

void GhostDoctorAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    for (int i = 0; i < 4; i++) {
        const GHOST_DOCTOR_ANIM_INFOS_Seed* s = &s_seeds[i];
        GHOST_DOCTOR_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        GHOST_DOCTOR_ANIM_INFOS[i].status              = s->status;
        GHOST_DOCTOR_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        GHOST_DOCTOR_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        GHOST_DOCTOR_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        GHOST_DOCTOR_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        GHOST_DOCTOR_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
