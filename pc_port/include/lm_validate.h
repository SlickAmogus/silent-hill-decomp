#ifndef LM_VALIDATE_H
#define LM_VALIDATE_H

/* Shared raw-ILM/PLM validator (Stage 1 item 3, docs/Oversized_Models_Plan.md).
 * Input is the RAW PSX-layout file bytes, validated BEFORE LmHeader_FixOffsets_PC
 * clobbers bytes 0-39 and before any heap allocation trusts the file. The TU is
 * standalone-compilable (offline harness, converter twin): it includes no game
 * headers and reports through the caller's message buffer — callers log.
 *
 * Rule V10 (quad corner-3 != 0xFF) is intentionally absent as a scan: the value
 * 0xFF in a quad's 4th corner byte IS the triangle sentinel, so the offending
 * file is byte-identical to a legal one and no post-hoc check can exist. The
 * hazard is made structurally impossible instead: skeletal V5 caps vertex
 * windows at 255 (slot 255 never stamped) and V11 rejects any corner reading an
 * unstamped slot. Do not "add V10 back". */

#ifndef LM_VALIDATE_HAVE_TYPES
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t  s32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LM_VALIDATE_POOL_SLOTS    256 /* u8 index space */
#define LM_VALIDATE_VERT_SLOT_CAP 255 /* skeletal: slot 255 reserved (tri sentinel) */
#define LM_VALIDATE_ARRAY_SLACK   8   /* after verticesXy / verticesZ / normals */
#define LM_VALIDATE_SHADE_SLACK   3   /* after the shade stream */
#define LM_VALIDATE_BUF_TAIL      16  /* every PC-owned model malloc adds this (F1) */
#define LM_VALIDATE_MAX_PARTS     56  /* Skeleton_Init cap, world_draw.c:1423 */

typedef struct
{
    u32 tailSlackBytes; /* guaranteed readable bytes past fileSize in the destination
                           buffer. Runtime callers pass LM_VALIDATE_BUF_TAIL (16, the
                           F1 malloc contract). 0 = strict authoring check. */
    s32 anmBoneCount;   /* <= 0: unknown, V13 skipped */
    u8  skeletal;       /* 1 = chara path: enforces V3 (<=56 parts), V5-strict
                           (vertex windows <= 255) and V11 (no dangling refs).
                           0 = held-item/prop/diagnostic mode: those resolve prim
                           corners against the wielder's/map pool at draw time,
                           so dangling refs and >56 parts are correct by design. */
} s_LmValidateParams;

/* Returns 0 on pass, else the failing rule id (positive). First failure wins,
 * rules evaluated in numeric order. On failure writes one line into err
 * (single sentence, no newline). */
int Lm_Validate(const u8* file, u32 fileSize, const s_LmValidateParams* p,
                char* err, u32 errCap);

#ifdef __cplusplus
}
#endif

#endif /* LM_VALIDATE_H */
