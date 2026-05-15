/* puppet_nurse_data.c -- Puppet Nurse (main hospital nurse) AI data
 *
 * Extracted from disc_extract/VIN/MAP3_S03.BIN. Combines four PSX rodata
 * blocks that puppet_nurse.c depends on:
 *
 *   1. Two parallel inline anim_info arrays at 0x800D5150 and 0x800D5430.
 *      Each is 46 x s_AnimInfo (16 B PSX / 32 B PC). Both arrays cover the
 *      same anim status set but with different per-entry durations -- they
 *      back two of the four PuppetNurse variants (see below). The PSX
 *      symbol table has no names for these so we call them _A / _B here.
 *
 *   2. sharedData_800D5710_3_s03[4] -- per-variant instance table.
 *      Each entry is s_800D5710 (52 B PSX, 56 B PC) with an embedded
 *      s_AnimInfo* (animInfo_24) that selects either the _A or _B table
 *      above. PSX held raw 0x800D5150 / 0x800D5430 addresses; we map
 *      those to the PC-resident arrays in PuppetNurseData_Init().
 *
 *   3. sharedData_800D5A8C_3_s03[3] -- combat tuning constants. Pure
 *      scalar data (s32, s32, s16, s16) -- no pointer reformat needed.
 *
 * Indexed by Chara_PuppetNurse properties.puppetNurse.field_124 (set in
 * puppet_nurse.c:278 from sharedData_800D5710_3_s03[charStatIdx]).
 *
 * Same seed-table + runtime-init pattern as groaner_anim_infos.c -- MinGW
 * won't accept exe-resident playback function pointers as compile-time
 * constants in static initialisers, so the s_AnimInfo arrays + the
 * instance table are zero-initialised at link time and patched at
 * startup. */

#include "bodyprog/bodyprog.h"
#include "maps/shared.h"

/* === Two parallel anim_info backing arrays === */
static s_AnimInfo s_animInfoA[46];   /* PSX 0x800D5150 */
static s_AnimInfo s_animInfoB[46];   /* PSX 0x800D5430 */

typedef struct {
    u8  kind;       /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8  status;
    s8  hasVariableDuration;
    u8  linkStatus;
    s32 durationConstantQ12;
    s16 startKeyframeIdx;
    s16 endKeyframeIdx;
} PnAnimSeed;

static const PnAnimSeed s_seedsA[46] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 1, 0x03, 0, 0x24, 73728, 0, 36 },
    { 0, 0x04, 0, 0x05, 16384, -1, 37 },
    { 1, 0x05, 0, 0x24, 81920, 37, 79 },
    { 0, 0x06, 0, 0x07, 81920, -1, 80 },
    { 1, 0x07, 0, 0x1C, 81920, 80, 172 },
    { 0, 0x08, 0, 0x09, 40960, -1, 173 },
    { 1, 0x09, 0, 0x0A, 81920, 173, 203 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 204 },
    { 1, 0x0B, 0, 0x1E, 81920, 204, 230 },
    { 0, 0x0C, 0, 0x0D, 16384, -1, 231 },
    { 2, 0x0D, 0, 0xFF, 81920, 231, 264 },
    { 0, 0x0E, 0, 0x0F, 81920, -1, 265 },
    { 1, 0x0F, 0, 0x24, 81920, 265, 306 },
    { 0, 0x10, 0, 0x11, 40960, -1, 307 },
    { 1, 0x11, 0, 0x24, 81920, 307, 362 },
    { 0, 0x12, 0, 0x13, 40960, -1, 363 },
    { 2, 0x13, 0, 0xFF, 81920, 363, 384 },
    { 0, 0x14, 0, 0x15, 81920, -1, 385 },
    { 1, 0x15, 0, 0x24, 81920, 385, 418 },
    { 0, 0x16, 0, 0x17, 40960, -1, 419 },
    { 1, 0x17, 0, 0x22, 102400, 419, 459 },
    { 0, 0x18, 0, 0x19, 40960, -1, 460 },
    { 1, 0x19, 0, 0x22, 102400, 460, 510 },
    { 0, 0x1A, 0, 0x1B, 40960, -1, 511 },
    { 1, 0x1B, 0, 0x22, 102400, 511, 577 },
    { 0, 0x1C, 0, 0x1D, 245760, -1, 172 },
    { 2, 0x1D, 0, 0xFF, 0, 172, 172 },
    { 0, 0x1E, 0, 0x1F, 245760, -1, 230 },
    { 2, 0x1F, 0, 0xFF, 0, 230, 230 },
    { 0, 0x20, 0, 0x21, 245760, -1, 36 },
    { 2, 0x21, 0, 0xFF, 0, 36, 36 },
    { 0, 0x22, 0, 0x23, 245760, -1, 459 },
    { 2, 0x23, 0, 0xFF, 0, 459, 459 },
    { 0, 0x24, 0, 0x25, 245760, -1, 418 },
    { 2, 0x25, 0, 0xFF, 0, 418, 418 },
    { 0, 0x26, 0, 0x27, 16384, -1, 265 },
    { 2, 0x27, 0, 0xFF, 8192, 265, 306 },
    { 0, 0x28, 0, 0x29, 20480, -1, 363 },
    { 1, 0x29, 0, 0x12, 81920, 363, 384 },
    { 0, 0x2A, 0, 0x2B, 20480, -1, 0 },
    { 1, 0x2B, 0, 0x24, 61440, 0, 36 },
    { 0, 0x2C, 0, 0x2D, 81920, -1, 120 },
    { 1, 0x2D, 0, 0x1C, 81920, 120, 172 },
};

static const PnAnimSeed s_seedsB[46] = {
    { 0, 0xFF, 0, 0x00, 0, -1, 0 },
    { 3, 0x00, 0, 0x00, 0, 0, 0 },
    { 0, 0x02, 0, 0x03, 40960, -1, 0 },
    { 1, 0x03, 0, 0x24, 61440, 0, 36 },
    { 0, 0x04, 0, 0x05, 16384, -1, 37 },
    { 1, 0x05, 0, 0x24, 81920, 37, 79 },
    { 0, 0x06, 0, 0x07, 81920, -1, 80 },
    { 1, 0x07, 0, 0x1C, 81920, 80, 172 },
    { 0, 0x08, 0, 0x09, 40960, -1, 173 },
    { 1, 0x09, 0, 0x0A, 81920, 173, 203 },
    { 0, 0x0A, 0, 0x0B, 40960, -1, 204 },
    { 1, 0x0B, 0, 0x1E, 81920, 204, 230 },
    { 0, 0x0C, 0, 0x0D, 16384, -1, 231 },
    { 2, 0x0D, 0, 0xFF, 61440, 231, 264 },
    { 0, 0x0E, 0, 0x0F, 81920, -1, 265 },
    { 1, 0x0F, 0, 0x24, 81920, 265, 306 },
    { 0, 0x10, 0, 0x11, 40960, -1, 307 },
    { 1, 0x11, 0, 0x24, 81920, 307, 362 },
    { 0, 0x12, 0, 0x13, 40960, -1, 363 },
    { 2, 0x13, 0, 0xFF, 71680, 363, 384 },
    { 0, 0x14, 0, 0x15, 81920, -1, 385 },
    { 1, 0x15, 0, 0x24, 61440, 385, 418 },
    { 0, 0x16, 0, 0x17, 40960, -1, 419 },
    { 1, 0x17, 0, 0x22, 86016, 419, 459 },
    { 0, 0x18, 0, 0x19, 40960, -1, 460 },
    { 1, 0x19, 0, 0x22, 86016, 460, 510 },
    { 0, 0x1A, 0, 0x1B, 40960, -1, 511 },
    { 1, 0x1B, 0, 0x22, 86016, 511, 577 },
    { 0, 0x1C, 0, 0x1D, 245760, -1, 172 },
    { 2, 0x1D, 0, 0xFF, 0, 172, 172 },
    { 0, 0x1E, 0, 0x1F, 245760, -1, 230 },
    { 2, 0x1F, 0, 0xFF, 0, 230, 230 },
    { 0, 0x20, 0, 0x21, 245760, -1, 36 },
    { 2, 0x21, 0, 0xFF, 0, 36, 36 },
    { 0, 0x22, 0, 0x23, 245760, -1, 459 },
    { 2, 0x23, 0, 0xFF, 0, 459, 459 },
    { 0, 0x24, 0, 0x25, 245760, -1, 418 },
    { 2, 0x25, 0, 0xFF, 0, 418, 418 },
    { 0, 0x26, 0, 0x27, 16384, -1, 265 },
    { 2, 0x27, 0, 0xFF, 8192, 265, 306 },
    { 0, 0x28, 0, 0x29, 20480, -1, 363 },
    { 1, 0x29, 0, 0x12, 71680, 363, 384 },
    { 0, 0x2A, 0, 0x2B, 20480, -1, 0 },
    { 1, 0x2B, 0, 0x24, 57344, 0, 36 },
    { 0, 0x2C, 0, 0x2D, 81920, -1, 120 },
    { 1, 0x2D, 0, 0x1C, 81920, 120, 172 },
};

/* === Instance table (4 variants) === */
/* `sharedData_800D5710_3_s03` is declared as `extern s_800D5710[]` in
 * shared.h; here we provide the matching definition. PSX size 0x34 (52B);
 * PC size grows to 56B because animInfo_24 expands from 4B to 8B. */
s_800D5710 sharedData_800D5710_3_s03[4];

typedef struct {
    q19_12 health_0;
    s32    field_4;
    s32    field_8;
    s32    field_C;
    s32    field_18;
    s32    idx_1C;
    s32    field_20;
    u8     animChoice;   /* 0 = use s_animInfoA, 1 = use s_animInfoB */
    q19_12 field_2C;
} PnInstSeed;

static const PnInstSeed s_pnInstSeeds[4] = {
    {   3072000,   1228800,  4096,  4096,  4485, 1, 58, 1,  20480 },  /* PSX animPtr=0x800D5430 */
    {   1638400,   1433600,  4096,  4096,  4096, 0, 57, 0,  30720 },  /* PSX animPtr=0x800D5150 */
    {   2662400,    614400,  4096,  4096,  4096, 0, 57, 0,  30720 },  /* PSX animPtr=0x800D5150 */
    {   3276800,   1433600,  6144,  4096,  4096, 0, 57, 0,  30720 },  /* PSX animPtr=0x800D5150 */
};

/* === Combat tuning array (3 entries) === */
s_D_800D5A8C sharedData_800D5A8C_3_s03[3] = {
    {  12288,  12288,  2048,   0 },
    {  16384,  20480,  2048,   0 },
    {   2048,      0,  4915,   0 },
};

void PuppetNurseData_Init(void)
{
    void (*fns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear, Anim_PlaybackOnce, Anim_PlaybackLoop };

    /* Populate the two parallel anim_info arrays. */
    for (int i = 0; i < 46; i++) {
        const PnAnimSeed* sA = &s_seedsA[i];
        s_animInfoA[i].playbackFunc        = (sA->kind < 3) ? fns[sA->kind] : NULL;
        s_animInfoA[i].status              = sA->status;
        s_animInfoA[i].hasVariableDuration = sA->hasVariableDuration;
        s_animInfoA[i].linkStatus          = sA->linkStatus;
        s_animInfoA[i].duration.constant   = sA->durationConstantQ12;
        s_animInfoA[i].startKeyframeIdx    = sA->startKeyframeIdx;
        s_animInfoA[i].endKeyframeIdx      = sA->endKeyframeIdx;

        const PnAnimSeed* sB = &s_seedsB[i];
        s_animInfoB[i].playbackFunc        = (sB->kind < 3) ? fns[sB->kind] : NULL;
        s_animInfoB[i].status              = sB->status;
        s_animInfoB[i].hasVariableDuration = sB->hasVariableDuration;
        s_animInfoB[i].linkStatus          = sB->linkStatus;
        s_animInfoB[i].duration.constant   = sB->durationConstantQ12;
        s_animInfoB[i].startKeyframeIdx    = sB->startKeyframeIdx;
        s_animInfoB[i].endKeyframeIdx      = sB->endKeyframeIdx;
    }

    /* Populate the 4-variant instance table; animInfo_24 picks A or B. */
    for (int i = 0; i < 4; i++) {
        const PnInstSeed* s = &s_pnInstSeeds[i];
        sharedData_800D5710_3_s03[i].health_0    = s->health_0;
        sharedData_800D5710_3_s03[i].field_4     = s->field_4;
        sharedData_800D5710_3_s03[i].field_8     = s->field_8;
        sharedData_800D5710_3_s03[i].field_C     = s->field_C;
        sharedData_800D5710_3_s03[i].field_18    = s->field_18;
        sharedData_800D5710_3_s03[i].idx_1C      = s->idx_1C;
        sharedData_800D5710_3_s03[i].field_20    = s->field_20;
        sharedData_800D5710_3_s03[i].animInfo_24 = (s->animChoice == 0) ? s_animInfoA : s_animInfoB;
        sharedData_800D5710_3_s03[i].field_2C    = s->field_2C;
    }
}
