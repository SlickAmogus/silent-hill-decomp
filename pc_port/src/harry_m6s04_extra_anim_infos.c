/* D_800CC424 — Harry's special map-anim overrides for the map6_s04 Cybil boss
 * sequence, binary-extracted from disc_extract/VIN/MAP6_S04.BIN (PSX
 * 0x800CC424, 8 x s_AnimInfo). func_800E1CA0 copies these into
 * g_MapOverlayHdr.harryMapAnimInfos[16..23] (Harry statuses 54-61) during the
 * boss cutscene setup, OVERWRITING the real base table entries. As a
 * u8[256]={0} zero-stub it zeroed 8 of Harry's boss animations (no playback
 * func, no keyframes) -> Harry froze/T-posed at those moments.
 *
 * s_AnimInfo holds function pointers (playbackFunc), so raw byte extraction
 * stores dead PSX addresses on 64-bit. Use the seed+Init pattern like the
 * *_anim_infos.c tables: store the non-pointer fields + a playback-kind, then
 * resolve the real PC playback function at runtime (Init runs at boot, before
 * the cutscene). All 8 entries are constant-duration (no variableFunc), 4
 * BlendLinear+PlaybackOnce pairs. */

#include "bodyprog/bodyprog.h"

s_AnimInfo D_800CC424[8];

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} D_800CC424_Seed;

static const D_800CC424_Seed s_seeds[8] = {
    { 0, 0x5C, 0, 0x5D, 40960,  -1, 805 },
    { 1, 0x5D, 0, 0x5D, 73728, 805, 819 },
    { 0, 0x5E, 0, 0x5F, 40960,  -1, 820 },
    { 1, 0x5F, 0, 0x5F, 73728, 820, 834 },
    { 0, 0x60, 0, 0x61, 40960,  -1, 835 },
    { 1, 0x61, 0, 0x61, 122880, 835, 868 },
    { 0, 0x62, 0, 0x63, 40960,  -1, 869 },
    { 1, 0x63, 0, 0x63, 122880, 869, 902 },
};

void Map6S04ExtraAnimInfos_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };
    int i;
    for (i = 0; i < 8; i++) {
        const D_800CC424_Seed* s = &s_seeds[i];
        D_800CC424[i].playbackFunc        = (s->kind < 3) ? fns[s->kind] : NULL;
        D_800CC424[i].status              = s->status;
        D_800CC424[i].hasVariableDuration = s->hasVariableDuration;
        D_800CC424[i].linkStatus          = s->linkStatus;
        D_800CC424[i].duration.constant   = s->durationConstantQ12;
        D_800CC424[i].startKeyframeIdx    = s->startKeyframeIdx;
        D_800CC424[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
