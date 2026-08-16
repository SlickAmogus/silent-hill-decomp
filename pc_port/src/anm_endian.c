/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * anm_endian.c - byte-swap an ANM header in place after it is read off the disc.
 *
 * ANM is one of the disc formats with NO reformat walker: s_AnmHeader is
 * overlaid straight onto the file buffer, so on a big-endian host every
 * multi-byte field reads byte-swapped. Same shape as the TIM bug (see
 * tim_endian.c) and the same remedy -- convert once at the load chokepoint, so
 * everything downstream sees native values.
 *
 * THE PAYLOAD NEEDS NOTHING. Both variable-length regions are entirely s8:
 * the bind poses are {parent, rotIdx, transIdx, s8[3] translation} and a
 * keyframe is transBC*3 then rotBC*9 raw s8 matrix coefficients. So only the
 * five multi-byte header fields move:
 *
 *     0x00 u16 dataOffset      0x04 u16 keyframeDataSize
 *     0x08 u32 activeBones     0x0C u32 fileSize
 *     0x10 u16 keyframeCount
 *
 * THE HARD PART IS KNOWING WHETHER THERE IS A HEADER AT ALL. Harry's animation
 * pool is three buffers, and only the first has one: HB_BASE.ANM carries the
 * header, while HB_WEP* and HB_M*S* are HEADERLESS keyframe blobs that are only
 * meaningful read through HB_BASE's header at a fixed RAM delta. Byte 0 of
 * those is channel data. Swapping one would silently corrupt two bytes of
 * animation and there would be nothing to see but a slightly wrong pose.
 *
 * So this validates rather than assumes, using the header's own cross-field
 * invariants (verified across all 55 retail headered files):
 *   - dataOffset is ALWAYS 404 -- the bind region is sized for 64 slots
 *     (0x14 + 64*6) and zero-padded, whatever boneCount says
 *   - keyframeDataSize == rotationBoneCount*9 + translationBoneCount*3, exactly
 *   - fileSize == align4(dataOffset + keyframeDataSize * keyframeCount)
 * Three independent agreements is far past coincidence for a blob of s8 pose
 * data, and any one of them failing means "not a header, leave it alone".
 */
#include <stdint.h>

#include "anm_endian.h"
#include "sh_log.h"

#if defined(__BIG_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)

#define ANM_DATA_OFFSET 404   /* 0x14 + 64*6, constant across every retail ANM */

static uint32_t Swap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0x0000FF00u) | ((v << 8) & 0x00FF0000u) | (v << 24);
}

static uint16_t Swap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

void Anm_SwapForBigEndian(void* addr)
{
    uint8_t* p = (uint8_t*)addr;
    uint16_t dataOffset, kfSize, kfCount;
    uint8_t  rotBC, transBC;
    uint32_t fileSize, want;

    if (!p)
        return;

    /* Already native? dataOffset reads 404 converted and 0x9401 while foreign,
     * so a second call is a no-op -- which matters because a buffer can be
     * re-parsed without being re-read. */
    {
        uint16_t cur = (uint16_t)((p[0] << 8) | p[1]);   /* big-endian read */
        if (cur == ANM_DATA_OFFSET)
            return;
    }

    /* Read the candidate header as little-endian, i.e. what the disc holds. */
    dataOffset = (uint16_t)(p[0] | (p[1] << 8));
    rotBC      = p[2];
    transBC    = p[3];
    kfSize     = (uint16_t)(p[4] | (p[5] << 8));
    fileSize   = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                 ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    kfCount    = (uint16_t)(p[16] | (p[17] << 8));

    if (dataOffset != ANM_DATA_OFFSET)
        return;                                  /* headerless keyframe bank */
    if (kfSize != (uint16_t)(rotBC * 9 + transBC * 3))
        return;
    want = (uint32_t)dataOffset + (uint32_t)kfSize * (uint32_t)kfCount;
    want = (want + 3u) & ~3u;
    if (fileSize != want)
        return;

    /* Only the five multi-byte fields; every byte field and the whole payload
     * is already correct. */
    { uint16_t* v = (uint16_t*)(p +  0); *v = Swap16(*v); }   /* dataOffset  */
    { uint16_t* v = (uint16_t*)(p +  4); *v = Swap16(*v); }   /* kfSize      */
    { uint32_t* v = (uint32_t*)(p +  8); *v = Swap32(*v); }   /* activeBones */
    { uint32_t* v = (uint32_t*)(p + 12); *v = Swap32(*v); }   /* fileSize    */
    { uint16_t* v = (uint16_t*)(p + 16); *v = Swap16(*v); }   /* kfCount     */
}

#else

void Anm_SwapForBigEndian(void* addr)
{
    (void)addr;
}

#endif
