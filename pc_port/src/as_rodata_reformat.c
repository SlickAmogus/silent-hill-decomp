/* as_rodata_reformat.c — translate the Air Screamer per-keyframe AI rodata
 * from PSX layout to PC layout at startup.
 *
 * The PSX struct s_func_800D2E04 contains s_AnimInfo[56] which on PSX is
 * 16 bytes per entry (4-byte function pointers). On PC, s_AnimInfo grows
 * to 32 bytes (8-byte function pointers + 8-byte alignment slot for the
 * union with variableFunc), so animInfo_0[56] alone shifts every later
 * field by ~896 bytes. ptr_D48[5] also expands from 20 B to 40 B.
 *
 * Rather than try to make C aliasing work between two incompatible
 * layouts, we keep the raw PSX bytes in g_AsRodataPsxRaw[0xD78] and
 * walk them here with explicit PSX offsets, populating the real
 * sharedData_800CAA98_0_s01 struct in its native PC layout. After this
 * runs once at startup, every read from sharedData_800CAA98_0_s01 in
 * air_screamer.c hits correctly-laid-out memory and the AI behaves as
 * the PSX game intended (per-keyframe behavior bitfield, attack-distance
 * LUT, weighted attack tables, SFX volumes).
 *
 * Function/data pointers in the raw bytes are PSX 0x80XXXXXX addresses
 * which were already zeroed at extract time. We translate those zero
 * 4-byte slots into NULL 8-byte pointers on PC. Game code already
 * NULL-checks ptr_D48 entries (air_screamer.c:13257/13281/13372 etc.)
 * and the s_AnimInfo dispatch path handles NULL playbackFunc gracefully
 * via the existing PC shim.
 */

#include "bodyprog/bodyprog.h"
#include "maps/shared.h"
#include "sh_log.h"

#include <stdio.h>
#include <string.h>

/* Storage for the runtime-populated PC struct. The header in
 * include/maps/shared.h declares this as `extern const`, but we need to
 * write to it from this init function — declare locally without const
 * so we can populate it. The linker resolves both declarations to the
 * same symbol. After init, game code reads it through the const-qualified
 * header view, which is the read-only contract we want. */
s_func_800D2E04 sharedData_800CAA98_0_s01;

/* Raw bytes — owned by data_stubs.c. */
extern const u8 g_AsRodataPsxRaw[0xD78];

static inline u32 rd32(const u8* p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u16 rd16(const u8* p) { return (u16)p[0] | ((u16)p[1] << 8); }
static inline s16 rds16(const u8* p) { return (s16)rd16(p); }
static inline s32 rds32(const u8* p) { return (s32)rd32(p); }

void AsRodata_Reformat(void)
{
    s_func_800D2E04* dst = &sharedData_800CAA98_0_s01;
    const u8* src = g_AsRodataPsxRaw;
    int i;

    /* Zero everything first so any field we don't explicitly populate is
     * predictable rather than uninitialized. */
    memset(dst, 0, sizeof(*dst));

    /* animInfo_0[56] — 16 B PSX → 32 B PC (per s_AnimInfo). */
    for (i = 0; i < 56; i++)
    {
        const u8*  p = &src[i * 16];
        s_AnimInfo* a = &dst->animInfo_0[i];

        /* PSX address invalid on PC; existing dispatch shim handles NULL. */
        a->playbackFunc        = NULL;
        a->status              = p[4];
        a->hasVariableDuration = (s8)p[5];
        a->linkStatus          = p[6];
        /* p[7] is pad on both PSX and PC. */

        if (a->hasVariableDuration)
        {
            /* Variable-duration entries hold a function pointer in PSX
             * bytes. Set NULL on PC; Anim_DurationGet's variableFunc
             * cast at bodyprog_anim_800445A4.c handles NULL via the
             * existing PC fallback. */
            a->duration.variableFunc = NULL;
        }
        else
        {
            a->duration.constant = rds32(&p[8]);
        }

        a->startKeyframeIdx = rds16(&p[12]);
        a->endKeyframeIdx   = rds16(&p[14]);
    }

    /* field_380[301][2] — direct copy (s32 array, identical on PSX/PC). */
    memcpy(dst->field_380, &src[0x380], 301 * 2 * 4);

    /* sfxVolumes_CE8[11] — 4 B per entry on both layouts. */
    memcpy(dst->sfxVolumes_CE8, &src[0xCE8], 11 * 4);

    /* properties_D14[4] — u_Property union, 4 B on both layouts. */
    memcpy(dst->properties_D14, &src[0xD14], 4 * 4);

    /* field_D24[2][3][2] — 24 B u16 array. */
    memcpy(dst->field_D24, &src[0xD24], 2 * 3 * 2 * sizeof(u16));

    /* field_D3C[2][6] — 12 B u8 array. */
    memcpy(dst->field_D3C, &src[0xD3C], 2 * 6);

    /* ptr_D48[5] — PSX 4-B addresses, already zeroed at extract → NULL on PC. */
    for (i = 0; i < 5; i++)
        dst->ptr_D48[i] = NULL;

    /* field_D5C[4][2] — 16 B s16 array. */
    memcpy(dst->field_D5C, &src[0xD5C], 4 * 2 * sizeof(s16));

    /* unk_D6C[4] — 4 B s8 array. */
    memcpy(dst->unk_D6C, &src[0xD6C], 4);

    /* field_D70[2][2] — 8 B s16 array. */
    memcpy(dst->field_D70, &src[0xD70], 2 * 2 * sizeof(s16));

    SH_LOG("AsRodata_Reformat: populated sharedData_800CAA98_0_s01 from %d-byte raw extract",
           0xD78);
}
