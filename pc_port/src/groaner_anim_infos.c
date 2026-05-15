/* groaner_anim_infos.c — Groaner animation info table.
 *
 * Ported from the commented-out PSX reference in
 * include/maps/characters/groaner.h:76-125. Replaces the previous
 * `u8 GROANER_ANIM_INFOS[256] = {0};` stub that caused
 * sharedFunc_800E5EC4_2_s00 (called from Ai_Groaner_Update) to read
 * GROANER_ANIM_INFOS[anim.status].playbackFunc as NULL and crash with
 * INVALID_POINTER_EXECUTE on the first Groaner update.
 *
 * The playbackFunc field points at exe-resident Anim_BlendLinear /
 * Anim_PlaybackOnce / Anim_PlaybackLoop (bodyprog_anim_800445A4.c).
 * MinGW doesn't treat function symbols as compile-time constants for
 * static initializers, so the array is zero-initialized and patched in
 * GroanerAnimInfos_Init() called from main_pc.c at startup.
 *
 * Indexed by groaner->model.anim.status (0..47). Lives in the exe so
 * every map DLL that #includes groaner.c (map2_s00, map2_s02, map4_s02,
 * map5_s01, map6_s00) reads the same table via the exe's import lib.
 *
 * Duration constants are pre-multiplied Q12 (= value * 4096) — gcc
 * refuses to accept the Q12() macro (which does a float * int cast)
 * inside a `static const` initializer for some entries past index 45,
 * so we just store the raw fixed-point ints. */

#include "bodyprog/bodyprog.h"

s_AnimInfo GROANER_ANIM_INFOS[48];

typedef struct {
    u8     kind;   /* 0=BlendLinear, 1=PlaybackOnce, 2=PlaybackLoop, 3=NULL */
    u8     status;
    s8     hasVariableDuration;
    u8     linkStatus;
    s32    durationConstantQ12;   /* raw Q12 (= floatValue * 4096) */
    s16    startKeyframeIdx;
    s16    endKeyframeIdx;
} GroanerAnimSeed;

static const GroanerAnimSeed s_seeds[48] = {
    { 0, 0xFF,                      0, ANIM_STATUS(0,  false), 0,      -1,   0 }, /* status=NO_VALUE */
    { 3, ANIM_STATUS(0,  false),    0, ANIM_STATUS(0,  false), 0,       0,   0 },
    { 0, ANIM_STATUS(1,  false),    0, ANIM_STATUS(1,  true),  8192,   -1,   0 },
    { 1, ANIM_STATUS(1,  true),     0, ANIM_STATUS(18, false), 12288,   0,  15 },
    { 0, ANIM_STATUS(2,  false),    0, ANIM_STATUS(2,  true),  65536,  -1,  19 },
    { 1, ANIM_STATUS(2,  true),     0, ANIM_STATUS(9,  false), 101580, 19,  36 },
    { 0, ANIM_STATUS(3,  false),    0, ANIM_STATUS(3,  true),  65536,  -1,  39 },
    { 1, ANIM_STATUS(3,  true),     0, ANIM_STATUS(4,  false), 0,      39,  77 },
    { 0, ANIM_STATUS(4,  false),    0, ANIM_STATUS(4,  true),  4096,   -1,  78 },
    { 2, ANIM_STATUS(4,  true),     0, 0xFF,                   16384,  78,  93 },
    { 0, ANIM_STATUS(5,  false),    0, ANIM_STATUS(5,  true),  8192,   -1,  94 },
    { 1, ANIM_STATUS(5,  true),     0, ANIM_STATUS(19, false), 14336,  94, 110 },
    { 0, ANIM_STATUS(6,  false),    0, ANIM_STATUS(6,  true),  65536,  -1, 114 },
    { 1, ANIM_STATUS(6,  true),     0, ANIM_STATUS(9,  false), 99123, 114, 130 },
    { 0, ANIM_STATUS(7,  false),    0, ANIM_STATUS(7,  true),  65536,  -1, 134 },
    { 1, ANIM_STATUS(7,  true),     0, ANIM_STATUS(8,  false), 0,     134, 164 },
    { 0, ANIM_STATUS(8,  false),    0, ANIM_STATUS(8,  true),  4096,   -1, 165 },
    { 2, ANIM_STATUS(8,  true),     0, 0xFF,                   24576, 165, 190 },
    { 0, ANIM_STATUS(9,  false),    0, ANIM_STATUS(9,  true),  8192,   -1, 191 },
    { 2, ANIM_STATUS(9,  true),     0, 0xFF,                   28672, 191, 206 },
    { 0, ANIM_STATUS(10, false),    0, ANIM_STATUS(10, true),  81920,  -1, 214 },
    { 1, ANIM_STATUS(10, true),     0, ANIM_STATUS(16, false), 0,     214, 243 },
    { 0, ANIM_STATUS(11, false),    0, ANIM_STATUS(11, true),  8192,   -1, 244 },
    { 1, ANIM_STATUS(11, true),     0, ANIM_STATUS(20, false), 14336, 244, 264 },
    { 0, ANIM_STATUS(12, false),    0, ANIM_STATUS(12, true),  65536,  -1, 268 },
    { 1, ANIM_STATUS(12, true),     0, ANIM_STATUS(9,  false), 99123, 268, 284 },
    { 0, ANIM_STATUS(13, false),    0, ANIM_STATUS(13, true),  131072, -1, 288 },
    { 1, ANIM_STATUS(13, true),     0, ANIM_STATUS(14, false), 0,     288, 318 },
    { 0, ANIM_STATUS(14, false),    0, ANIM_STATUS(14, true),  4096,   -1, 319 },
    { 2, ANIM_STATUS(14, true),     0, 0xFF,                   24576, 319, 345 },
    { 0, ANIM_STATUS(15, false),    0, ANIM_STATUS(15, true),  131072, -1, 346 },
    { 1, ANIM_STATUS(15, true),     0, ANIM_STATUS(10, false), 221184,346, 362 },
    { 0, ANIM_STATUS(16, false),    0, ANIM_STATUS(16, true),  65536,  -1, 363 },
    { 2, ANIM_STATUS(16, true),     0, 0xFF,                   0,     363, 370 },
    { 0, ANIM_STATUS(17, false),    0, ANIM_STATUS(17, true),  8192,   -1, 371 },
    { 2, ANIM_STATUS(17, true),     0, 0xFF,                   47104, 371, 396 },
    { 0, ANIM_STATUS(18, false),    0, ANIM_STATUS(18, true),  65536,  -1,  15 },
    { 2, ANIM_STATUS(18, true),     0, 0xFF,                   0,      15,  16 },
    { 0, ANIM_STATUS(19, false),    0, ANIM_STATUS(19, true),  65536,  -1, 110 },
    { 2, ANIM_STATUS(19, true),     0, 0xFF,                   0,     110, 111 },
    { 0, ANIM_STATUS(20, false),    0, ANIM_STATUS(20, true),  65536,  -1, 264 },
    { 2, ANIM_STATUS(20, true),     0, 0xFF,                   0,     264, 265 },
    { 0, ANIM_STATUS(21, false),    0, ANIM_STATUS(21, true),  65536,  -1,   0 },
    { 1, ANIM_STATUS(21, true),     0, ANIM_STATUS(4,  false), 49152,   0,  15 },
    { 0, ANIM_STATUS(22, false),    0, ANIM_STATUS(22, true),  65536,  -1,  94 },
    { 1, ANIM_STATUS(22, true),     0, ANIM_STATUS(8,  false), 49152,  94, 110 },
    { 0, ANIM_STATUS(23, false),    0, ANIM_STATUS(23, true),  65536,  -1, 244 },
    { 1, ANIM_STATUS(23, true),     0, ANIM_STATUS(14, false), 49152, 244, 264 },
};

void GroanerAnimInfos_Init(void)
{
    void (*playbackFns[3])(struct _Model*, struct _AnmHeader*, GsCOORDINATE2*, struct _AnimInfo*) = {
        Anim_BlendLinear,
        Anim_PlaybackOnce,
        Anim_PlaybackLoop,
    };
    for (int i = 0; i < 48; i++) {
        const GroanerAnimSeed* s = &s_seeds[i];
        GROANER_ANIM_INFOS[i].playbackFunc        = (s->kind < 3) ? playbackFns[s->kind] : NULL;
        GROANER_ANIM_INFOS[i].status              = s->status;
        GROANER_ANIM_INFOS[i].hasVariableDuration = s->hasVariableDuration;
        GROANER_ANIM_INFOS[i].linkStatus          = s->linkStatus;
        GROANER_ANIM_INFOS[i].duration.constant   = s->durationConstantQ12;
        GROANER_ANIM_INFOS[i].startKeyframeIdx    = s->startKeyframeIdx;
        GROANER_ANIM_INFOS[i].endKeyframeIdx      = s->endKeyframeIdx;
    }
}
