/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Skinned glTF character replacements -- see pc_skin_chara.h.
 *
 * Load: the skin's joints map onto game bones by name; each game bone then
 * gets a retarget transform taking the model's bind pose onto the character's
 * rest pose (bone direction swung onto the model's, joint moved onto the
 * model's joint, segment length scaled to the game's), so every vertex is
 * stored as up to four bone-local positions.
 *
 * Frame: the skeleton drawer hands over each bone's view matrix; vertices are
 * blended from those, projected like the GTE, lit by the game's own per-bone
 * light setup, and emitted as one modern-mesh packet per material. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "game.h"
#include "inline_no_dmpsx.h"
#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/chara/chara_model.h"
#include "bodyprog/gfx/world.h"
#include "bodyprog/formats/lm.h"
#include "bodyprog/math/math.h"
#include "main/fileinfo.h"

#include "cgltf.h"

#include "PsyX/PsyX_render.h"
#include "psx/gtereg.h"
#include "psx/libgpu.h"
#include "psyq/libgs.h"
#include "hires_override.h"
#include "pc_config.h"
#include "pc_lipsync.h"
#include "pc_pplit.h"
#include "pc_modern_mesh.h"
#include "pc_modern_shader.h"
#include "pc_modern_vertex.h"
#include "pc_skin_chara.h"
#include "sh_log.h"

extern FILE* Pc_LooseFOpen(const char* path, const char* mode);
extern float PGXP_GetSzMax(void);
extern int   g_PsxUsePgxp;
extern int   g_currentOTBucketCount;
extern void  PsyX_NoteItemDepthSz(unsigned int sz);
extern void  func_80057228(MATRIX* mat, s32 alpha, SVECTOR* arg2, VECTOR3* arg3);
extern s_WorldEnvWork g_WorldEnvWork;

#define PC_SKIN_BONES     18
/* Loaded models: every play-as swap (PLAYAS_SWAP_MAX is 32) and a replacement
 * for each character can all be in use in one session. */
#define PC_SKIN_MAX_CHARA 64
/* Files looked for and not found, kept apart so they never take a slot. */
#define PC_SKIN_MAX_MISSING 128
#define PC_SKIN_MAX_MATS  32
#define PC_SKIN_MAX_JOINTS 256
#define PC_SKIN_INFL      4
/* Characters of one type share a model and so one PcSkinChara; each one drawn
 * in a frame needs its own output, or the last drawn overwrote the others' and
 * whichever the OT order put last showed in all their places. */
#define PC_SKIN_INST      4
/* Characters sit at func_800571D0(field_B_1 = 0)'s OT base. */
#define PC_SKIN_OT_BASE   2

typedef struct
{
    s16   R[9];  /* bone -> model rotation, Q12, row-major */
    s16   T[3];  /* joint, PSX model space */
    float C[3];  /* rest-pose centroid of the bone's stock geometry */
} PcSkinRestBone;

/* Rest poses: the ANM's keyframe 0, the pose the ILM converter exports
 * (<NAME>.ilmmeta.json restPose per part; centroids from that same <NAME>.obj).
 * HERO is HB_BASE.ANM. */
static const PcSkinRestBone s_heroRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -32, -96, 32, 4064, -64, 96, 64, 4064 }, { 0, -279, 0 }, { 0.5f, -322.1f, -6.4f } },
    { { 4036, -148, 124, 130, 4006, 383, -126, -385, 4012 }, { 0, -391, -6 }, { 1.2f, -407.5f, 7.7f } },
    { { 131, -44, 4031, -764, 3949, 94, -3950, -765, 128 }, { 0, -359, -2 }, { -35.7f, -357.2f, -7.1f } },
    { { 2822, -2809, -505, 273, -470, 3965, -2803, -2832, -144 }, { -40, -360, -4 }, { -46.5f, -339.9f, -8.0f } },
    { { 3100, -2487, 173, 196, 509, 3920, -2445, -3084, 525 }, { -47, -310, -6 }, { -50.9f, -285.7f, -5.2f } },
    { { 2027, -3378, 171, 357, 405, 3889, -3334, -2027, 520 }, { -48, -249, 4 }, { -46.5f, -227.0f, 10.4f } },
    { { -124, 77, 4038, -511, 4010, -92, -4015, -507, -129 }, { 0, -359, -2 }, { 33.7f, -357.3f, -7.3f } },
    { { 3882, 985, 75, 52, -544, 3974, 974, -3859, -535 }, { 39, -360, -4 }, { 41.1f, -339.9f, -11.6f } },
    { { 3720, 1419, 136, -333, 414, 3954, 1379, -3720, 496 }, { 39, -310, -11 }, { 44.3f, -285.6f, -12.0f } },
    { { 3928, 237, -456, 397, 490, 3919, 262, -3926, 468 }, { 44, -249, -2 }, { 44.5f, -226.3f, 2.9f } },
    { { -4064, 32, 192, 64, 4064, 64, -192, 96, -4064 }, { 0, -279, 0 }, { 0.4f, -259.8f, -4.7f } },
    { { -4008, -435, 157, -415, 4011, -159, -137, -151, -4039 }, { -16, -236, 0 }, { -22.9f, -186.8f, -9.7f } },
    { { -3977, -380, 256, -412, 3815, -1125, -136, -1124, -3845 }, { -26, -146, -4 }, { -32.4f, -93.0f, -27.2f } },
    { { -3708, -6, 1410, -175, 3934, -512, -1398, -514, -3687 }, { -36, -44, -43 }, { -44.1f, -26.4f, -37.9f } },
    { { 4044, -31, 63, 4, 4033, -36, -62, 60, 4045 }, { 16, -236, 1 }, { 17.4f, -186.4f, -5.4f } },
    { { 4012, -38, 58, 3, 3974, 437, -62, -415, 3988 }, { 15, -146, 2 }, { 15.7f, -89.3f, -11.1f } },
    { { 3873, -188, 843, 134, 3959, 20, -862, 30, 3893 }, { 13, -39, -18 }, { 19.1f, -22.1f, -9.4f } },
};

/* SIBYL (Cybil): SBL.ANM keyframe 0, the same 17 bones as HERO. */
static const PcSkinRestBone s_sibylRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -32, -96, 32, 4064, -288, 96, 288, 4064 }, { 0, -271, 12 }, { 0.2f, -312.8f, 9.9f } },
    { { 4033, 10, -67, 2, 4032, 161, 61, -161, 4032 }, { 0, -363, 5 }, { -1.8f, -381.2f, 14.9f } },
    { { 131, -44, 4031, -547, 3995, 93, -3995, -548, 131 }, { 0, -335, 3 }, { -29.9f, -331.8f, 1.2f } },
    { { 3900, -860, -223, 273, 84, 3991, -854, -3894, 146 }, { -32, -336, 1 }, { -38.5f, -308.0f, 3.8f } },
    { { 3940, -422, -13, 134, 1048, 3815, -420, -3808, 1052 }, { -39, -286, 3 }, { -41.5f, -264.6f, 9.9f } },
    { { 3930, 76, -188, 179, 1137, 3754, 111, -3777, 1152 }, { -40, -231, 18 }, { -43.9f, -215.1f, 26.1f } },
    { { -28, -83, 4034, -289, 4038, 104, -4044, -258, -28 }, { 0, -335, 3 }, { 29.3f, -330.7f, 1.8f } },
    { { 3981, 11, 388, -367, -663, 3960, 65, -3957, -637 }, { 31, -335, 2 }, { 38.9f, -307.2f, -1.8f } },
    { { 3887, 568, 587, -604, 248, 3945, 515, -3945, 349 }, { 39, -286, -7 }, { 45.2f, -263.8f, -4.2f } },
    { { 3224, 2236, 117, -890, 1108, 3700, 2043, -3081, 1434 }, { 47, -229, -2 }, { 48.8f, -213.8f, 8.0f } },
    { { -4064, 32, 192, 64, 4064, 64, -192, 96, -4064 }, { 0, -271, 12 }, { 0.4f, -248.9f, 8.6f } },
    { { -4007, -403, 157, -383, 4011, -159, -136, -149, -4039 }, { -16, -220, 12 }, { -20.1f, -186.4f, 8.6f } },
    { { -3976, -376, 208, -381, 3927, -690, -135, -684, -3957 }, { -21, -138, 9 }, { -25.0f, -92.3f, -4.2f } },
    { { -3906, 5, 619, 18, 3976, -108, -614, -67, -3940 }, { -31, -36, -13 }, { -34.2f, -18.3f, -3.1f } },
    { { 4028, 102, -253, -128, 4033, 96, 252, -61, 4033 }, { 16, -220, 13 }, { 18.1f, -186.5f, 10.6f } },
    { { 3996, 132, -237, -127, 3957, 598, 250, -565, 3962 }, { 14, -138, 11 }, { 15.9f, -91.8f, -0.7f } },
    { { 3924, -31, 566, -24, 3987, 23, -563, 24, 3933 }, { 17, -35, -8 }, { 20.1f, -17.6f, 2.3f } },
};

/* Cheryl (SRL.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_srlRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, 32, -128, 0, 4032, 448, 128, -448, 4032 }, { 0, -183, 0 }, { -0.1f, -209.8f, 0.1f } },
    { { 4028, 38, -253, 14, 3976, 665, 253, -665, 3972 }, { -1, -252, -5 }, { -3.1f, -265.8f, 6.3f } },
    { { 127, 31, 4032, -445, 4000, 0, -4001, -445, 127 }, { 0, -228, -7 }, { -25.1f, -226.4f, -7.3f } },
    { { 4000, 124, 35, 0, -598, 3951, 126, -3953, -598 }, { -24, -225, -9 }, { -29.8f, -210.1f, -8.7f } },
    { { 3952, -312, -204, 217, 2, 3951, -320, -3963, 26 }, { -28, -195, -14 }, { -33.7f, -173.6f, -11.8f } },
    { { 3921, -310, -203, 215, 1, 3920, -318, -3933, 25 }, { -38, -153, -14 }, { -37.5f, -140.9f, -7.6f } },
    { { 127, 31, 4032, -445, 4000, 0, -4001, -445, 127 }, { 0, -228, -7 }, { 24.5f, -226.5f, -5.5f } },
    { { 4000, 126, 27, 0, -348, 3979, 126, -3981, -348 }, { 23, -225, -7 }, { 28.8f, -210.2f, -5.4f } },
    { { 3811, 1091, 264, -261, -24, 3949, 1084, -3823, 38 }, { 27, -194, -10 }, { 32.8f, -172.9f, -6.9f } },
    { { 3781, 1082, 261, -259, -24, 3918, 1075, -3794, 37 }, { 37, -153, -8 }, { 35.6f, -140.7f, -2.0f } },
    { { -4064, 0, 0, 0, 4064, 96, 0, 96, -4064 }, { 0, -183, 0 }, { 0.0f, -166.6f, 1.3f } },
    { { -4001, 0, -381, -9, 4032, 94, 381, 95, -4001 }, { -12, -140, 1 }, { -13.7f, -99.6f, 1.6f } },
    { { -3970, -188, -379, -198, 4000, 93, 373, 112, -3970 }, { -12, -81, 2 }, { -14.0f, -47.2f, -0.1f } },
    { { -3939, -196, -372, -197, 3970, -2, 370, 18, -3942 }, { -16, -11, 3 }, { -15.0f, -2.1f, 11.8f } },
    { { 4032, 0, -286, -7, 4032, -96, 285, 95, 4032 }, { 11, -140, 1 }, { 14.1f, -99.6f, 1.5f } },
    { { 4000, 220, -284, -228, 4000, -96, 277, 109, 4000 }, { 11, -81, 2 }, { 12.8f, -47.1f, -0.2f } },
    { { 3968, 227, -275, -227, 3971, 29, 274, -17, 3972 }, { 14, -11, 3 }, { 13.3f, -2.5f, 11.4f } },
};
/* Dahlia (DARIA.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_dariaRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -64, 288, 64, 4064, 192, -288, -160, 4064 }, { 8, -271, -8 }, { 8.7f, -309.5f, -3.8f } },
    { { 4014, 395, -230, -357, 3996, 498, 276, -446, 4024 }, { 9, -371, -9 }, { 7.0f, -381.0f, -1.9f } },
    { { -289, -50, 4032, 0, 4041, 63, -4040, 31, -286 }, { 8, -331, -6 }, { -25.3f, -341.0f, -12.9f } },
    { { 3646, -1659, -302, 468, 325, 3973, -1620, -3655, 492 }, { -28, -324, -4 }, { -34.7f, -296.5f, -6.3f } },
    { { 3604, -1706, 16, 466, 1058, 3818, -1636, -3465, 1165 }, { -36, -271, 4 }, { -41.2f, -253.9f, 4.9f } },
    { { 3509, -1794, -312, 652, 609, 3855, -1714, -3488, 831 }, { -40, -231, 18 }, { -43.2f, -211.9f, 25.4f } },
    { { -277, -61, 4032, -602, 3981, 31, -3985, -573, -285 }, { 8, -331, -6 }, { 41.0f, -340.9f, -16.7f } },
    { { 3524, 1938, 212, -250, 51, 4002, 1932, -3499, 172 }, { 43, -323, -10 }, { 49.6f, -295.6f, -13.2f } },
    { { 3481, 1963, -49, -249, 579, 3932, 1944, -3407, 634 }, { 49, -269, -6 }, { 54.6f, -251.9f, -7.0f } },
    { { 2984, 2601, 71, 14, -109, 3954, 2608, -2979, -88 }, { 51, -228, 2 }, { 52.4f, -207.4f, 5.1f } },
    { { -4064, -32, -32, -32, 4064, 160, 32, 160, -4064 }, { 8, -271, -8 }, { 7.8f, -248.3f, -4.7f } },
    { { -3999, 92, -410, 113, 4015, -163, 387, -193, -4010 }, { -9, -212, -6 }, { -11.9f, -181.9f, -14.2f } },
    { { -3969, 97, -395, 80, 3968, 215, 385, 188, -3966 }, { -8, -150, -10 }, { -8.3f, -88.9f, -15.7f } },
    { { -3949, 40, 8, 38, 3951, -45, -27, -59, -3955 }, { -2, -30, -13 }, { -2.1f, -20.2f, -2.0f } },
    { { 3934, 314, 854, -383, 4019, 169, -843, -258, 3943 }, { 23, -212, -6 }, { 28.6f, -182.7f, -16.8f } },
    { { 3901, 276, 868, -319, 3998, 39, -872, -120, 3913 }, { 27, -150, -11 }, { 30.8f, -89.1f, -22.1f } },
    { { 3655, 151, 1581, -194, 3976, 9, -1589, -92, 3656 }, { 29, -29, -22 }, { 33.8f, -19.9f, -12.3f } },
};
/* Lisa (LISA.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_lisaRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -64, 160, 64, 4064, 64, -160, -64, 4064 }, { 0, -280, 0 }, { -0.5f, -313.7f, 0.6f } },
    { { 4033, 168, 226, -160, 4039, -222, -219, 181, 4034 }, { 1, -368, -7 }, { -0.8f, -387.4f, -0.3f } },
    { { -161, -92, 4031, 31, 4033, 95, -4034, 33, -160 }, { 0, -344, -7 }, { -27.3f, -342.9f, -4.7f } },
    { { 3962, 506, -97, 119, -81, 4002, 505, -3967, -94 }, { -32, -345, -6 }, { -33.0f, -319.5f, -5.9f } },
    { { 3935, 445, -190, 117, 670, 3923, 470, -3896, 652 }, { -34, -291, -8 }, { -36.3f, -270.3f, -6.3f } },
    { { 3894, -351, -484, 499, 190, 3914, -326, -3911, 236 }, { -37, -246, -1 }, { -40.0f, -225.5f, 3.2f } },
    { { -163, -22, 4032, 190, 4036, 31, -4037, 189, -159 }, { 0, -344, -7 }, { 27.0f, -342.1f, -7.4f } },
    { { 4010, 31, 114, -106, -348, 3999, 57, -4007, -357 }, { 31, -344, -9 }, { 32.5f, -315.4f, -10.4f } },
    { { 3978, 82, 105, -103, 409, 3970, 87, -3980, 402 }, { 32, -290, -14 }, { 34.2f, -269.0f, -13.6f } },
    { { 3514, 1806, -14, -20, 71, 3946, 1812, -3532, 50 }, { 33, -244, -10 }, { 32.3f, -223.2f, -6.8f } },
    { { -4064, 32, -128, 32, 4064, 32, 128, 32, -4064 }, { 0, -280, 0 }, { 0.0f, -259.9f, -4.3f } },
    { { -3991, -166, -478, -162, 4035, -220, 473, -185, -3992 }, { -16, -225, 0 }, { -18.4f, -173.3f, -4.8f } },
    { { -3960, -147, -481, -161, 4012, -61, 469, -28, -3969 }, { -20, -139, -4 }, { -21.8f, -103.5f, -8.0f } },
    { { -3973, 8, -168, -8, 3986, -48, 154, -47, -3975 }, { -24, -53, -5 }, { -23.6f, -31.6f, 0.8f } },
    { { 4026, 59, 318, -62, 4033, 93, -318, -97, 4027 }, { 16, -225, -1 }, { 17.1f, -173.3f, -5.5f } },
    { { 3994, 51, 316, -62, 3999, 186, -316, -191, 3993 }, { 17, -139, -4 }, { 17.2f, -103.7f, -9.5f } },
    { { 3956, 3, 404, -4, 3977, -5, -411, 2, 3963 }, { 18, -54, -9 }, { 18.6f, -32.7f, -3.0f } },
};
/* Bloody Lisa (BLISA.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_blisaRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, 64, -64, -64, 4064, 128, 64, -128, 4064 }, { 0, -280, 0 }, { -1.5f, -313.6f, 1.4f } },
    { { 4007, 159, 445, -175, 4029, 118, -442, -158, 4008 }, { -2, -368, -6 }, { -3.2f, -386.7f, 2.4f } },
    { { 63, 63, 4032, -127, 4032, -64, -4033, -127, 63 }, { -1, -344, -6 }, { -28.4f, -341.7f, -4.8f } },
    { { 3999, 150, -114, 3, 2291, 3290, 186, -3289, 2291 }, { -33, -344, -7 }, { -34.0f, -323.5f, 8.2f } },
    { { 3968, 91, -140, 20, 2861, 2739, 158, -2741, 2861 }, { -35, -300, 24 }, { -37.1f, -284.3f, 37.8f } },
    { { 3686, 29, -1348, 1143, 2064, 3145, 741, -3353, 1944 }, { -37, -268, 57 }, { -44.9f, -253.1f, 69.6f } },
    { { 63, 63, 4032, -127, 4032, -64, -4033, -127, 63 }, { -1, -344, -6 }, { 26.0f, -342.5f, -4.1f } },
    { { 3972, -296, 473, -331, 1497, 3696, -435, -3712, 1467 }, { 30, -345, -6 }, { 34.2f, -319.2f, 5.5f } },
    { { 3909, 382, 665, -750, 2580, 2911, -126, -2984, 2626 }, { 36, -295, 14 }, { 41.3f, -278.8f, 26.4f } },
    { { 2941, 2102, 1591, -2509, 1473, 2655, 853, -2974, 2446 }, { 43, -261, 44 }, { 50.4f, -248.5f, 59.3f } },
    { { -4032, 96, -384, 96, 4064, -32, 384, -32, -4032 }, { 0, -280, 0 }, { 0.1f, -259.2f, -3.8f } },
    { { -3990, 263, 538, 244, 4026, -307, -543, -271, -3960 }, { -15, -225, 1 }, { -12.0f, -172.9f, -4.8f } },
    { { -3959, 248, 539, 242, 4001, -211, -539, -177, -3936 }, { -10, -139, -5 }, { -7.3f, -103.8f, -10.3f } },
    { { -3959, 12, 418, 27, 3989, -77, -403, -83, -3928 }, { -5, -54, -9 }, { -5.4f, -32.5f, -3.6f } },
    { { 3981, 388, 478, -34, 3317, -2266, -602, 2233, 3273 }, { 17, -225, -2 }, { 23.1f, -180.4f, 19.7f } },
    { { 3949, 18, 610, -34, 3980, 207, -598, -207, 3925 }, { 25, -154, 45 }, { 24.6f, -118.9f, 39.5f } },
    { { 3501, -433, 1813, -225, 3706, 1323, -1843, -1258, 3244 }, { 25, -69, 40 }, { 25.8f, -46.9f, 38.4f } },
};
/* Alessa (AR.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_arRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4000, -64, -768, 0, 4064, -224, 768, 224, 4000 }, { -4, -251, 12 }, { -3.3f, -280.8f, 9.1f } },
    { { 4008, -168, -378, 84, 4020, -443, 384, 429, 3997 }, { -2, -331, 3 }, { -2.2f, -349.9f, 8.1f } },
    { { 760, -88, 3968, 349, 4025, 0, -3962, 347, 762 }, { -2, -303, 1 }, { -31.2f, -304.3f, -3.7f } },
    { { 4012, 36, -28, -1, 26, 4020, 26, -4034, 34 }, { -34, -296, -5 }, { -36.6f, -285.5f, -3.0f } },
    { { 3980, -1, -33, -1, 622, 3953, -6, -3967, 632 }, { -39, -257, -5 }, { -40.7f, -235.0f, -3.2f } },
    { { 3791, -1025, -282, 313, 253, 3939, -1004, -3834, 312 }, { -44, -207, 3 }, { -44.5f, -190.9f, 7.9f } },
    { { 761, -45, 3969, 285, 4028, -32, -3966, 290, 760 }, { -2, -303, 1 }, { 25.6f, -304.4f, 7.2f } },
    { { 3694, 1515, 425, -498, -14, 3974, 1512, -3715, 182 }, { 28, -296, 7 }, { 30.7f, -286.0f, 10.2f } },
    { { 3653, 1583, 193, -494, 572, 3913, 1529, -3619, 730 }, { 35, -258, 10 }, { 38.5f, -236.4f, 13.1f } },
    { { 2973, 2571, 485, -581, -134, 3916, 2559, -3024, 273 }, { 41, -209, 20 }, { 41.0f, -192.6f, 24.4f } },
    { { -3904, 128, 1184, 32, 4064, -352, -1184, -320, -3872 }, { -4, -251, 12 }, { -2.4f, -234.1f, 7.0f } },
    { { -4035, -390, 161, -366, 3987, 535, -223, 495, -3994 }, { -17, -216, 0 }, { -22.0f, -161.0f, 4.4f } },
    { { -4004, -368, 198, -364, 3979, 121, -222, 81, -3982 }, { -27, -119, 12 }, { -28.7f, -85.1f, 8.3f } },
    { { -3965, 36, 561, 87, 3949, 246, -552, 257, -3898 }, { -35, -34, 9 }, { -35.8f, -21.9f, 19.5f } },
    { { 3993, 406, -488, -447, 4017, -334, 448, 401, 3976 }, { 13, -216, 10 }, { 17.5f, -160.9f, 11.9f } },
    { { 3961, 445, -443, -444, 3985, 47, 444, 21, 3951 }, { 22, -118, 19 }, { 25.1f, -84.3f, 14.9f } },
    { { 3903, 152, 726, -154, 3986, -84, -721, 82, 3863 }, { 31, -33, 15 }, { 32.8f, -20.4f, 24.8f } },
};
/* ghost Alessa (TAR.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_tarRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4032, 96, -512, -96, 4064, 160, 512, -128, 4032 }, { 0, -195, 4 }, { -0.8f, -221.3f, 5.4f } },
    { { 4036, -31, 0, 12, 4035, 177, -4, -143, 4032 }, { -1, -263, -6 }, { -2.3f, -280.7f, 7.0f } },
    { { 697, 63, 3977, -164, 4033, -56, -3977, -131, 696 }, { 0, -240, -7 }, { -28.1f, -239.9f, -7.1f } },
    { { 3924, 801, 191, -51, -731, 3946, 814, -3875, -683 }, { -24, -236, -12 }, { -31.3f, -223.2f, -11.0f } },
    { { 3905, 752, -31, -63, 352, 3955, 747, -3888, 378 }, { -27, -206, -19 }, { -32.2f, -183.4f, -17.2f } },
    { { 3467, -1866, 219, -11, 432, 3922, -1907, -3432, 391 }, { -35, -164, -17 }, { -31.1f, -152.5f, -10.4f } },
    { { 513, 46, 4000, 223, 4015, -96, -3981, 252, 508 }, { 0, -240, -7 }, { 26.2f, -241.3f, 1.3f } },
    { { 3992, 305, 137, -117, -500, 3960, 315, -3957, -470 }, { 23, -237, -4 }, { 30.2f, -224.7f, -0.2f } },
    { { 3960, 328, 53, -117, 540, 3934, 312, -3924, 568 }, { 27, -207, -8 }, { 32.7f, -184.8f, -3.8f } },
    { { 3349, 2083, -95, -164, 392, 3920, 2080, -3328, 459 }, { 35, -165, -2 }, { 31.1f, -153.6f, 4.6f } },
    { { -4064, 128, -96, 128, 4064, -96, 96, -96, -4064 }, { 0, -195, 4 }, { 0.4f, -178.0f, 5.4f } },
    { { -4034, 82, -93, 64, 3930, 893, 128, 893, -3928 }, { -11, -151, 3 }, { -12.1f, -115.0f, 10.2f } },
    { { -4003, 10, -123, 63, 3661, -1622, 127, -1621, -3661 }, { -10, -94, 16 }, { -10.1f, -69.4f, 2.0f } },
    { { -3980, 167, 159, 91, 3642, -1586, -195, -1585, -3654 }, { -10, -30, -13 }, { -10.3f, -20.5f, -9.4f } },
    { { 3994, -250, -486, 238, 4017, -143, 501, 136, 3988 }, { 13, -152, 2 }, { 14.6f, -115.2f, 3.9f } },
    { { 3962, -237, -489, 236, 3988, -48, 497, 41, 3960 }, { 9, -94, 3 }, { 7.7f, -66.2f, 0.1f } },
    { { 3944, -19, -486, 16, 3969, -48, 490, 67, 3929 }, { 4, -24, 3 }, { 3.4f, -16.8f, 10.1f } },
};
/* the Incubator (MAR.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_marRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 3968, 96, -928, -352, 3904, -1056, 896, 1120, 3808 }, { 8, -251, 16 }, { 7.5f, -282.3f, 4.8f } },
    { { 3937, 95, -921, -350, 3873, -1048, 889, 1111, 3778 }, { 7, -327, -10 }, { 4.6f, -346.9f, -8.3f } },
    { { 826, 435, 3940, -1131, 3849, -197, -3770, -1047, 932 }, { 8, -299, -6 }, { -19.6f, -296.5f, -13.6f } },
    { { 3898, 321, -969, 803, 1254, 3702, 625, -3753, 1164 }, { -22, -290, -16 }, { -27.5f, -280.4f, -11.7f } },
    { { 3004, 1132, -2415, 503, 3279, 2171, 2578, -1911, 2289 }, { -36, -255, -6 }, { -53.8f, -241.1f, 7.7f } },
    { { 2438, 2153, -2314, 633, 2492, 2972, 3045, -2149, 1168 }, { -70, -228, 20 }, { -81.9f, -218.0f, 26.6f } },
    { { 808, 480, 3934, -258, 3990, -441, -3936, -149, 862 }, { 8, -299, -6 }, { 35.3f, -301.3f, 0.6f } },
    { { 3685, 1126, 1159, -1430, 1012, 3588, 742, -3691, 1341 }, { 39, -295, 0 }, { 44.4f, -286.0f, 5.7f } },
    { { 3264, -100, 2291, -1746, 2442, 2588, -1430, -3137, 1954 }, { 53, -262, 13 }, { 70.6f, -246.1f, 24.9f } },
    { { 2548, 20, 3044, -2422, 2388, 1998, -1780, -3142, 1558 }, { 85, -231, 36 }, { 97.9f, -225.1f, 45.2f } },
    { { -4064, -64, 0, -64, 4032, -448, 32, -448, -4032 }, { 8, -251, 16 }, { 7.7f, -234.8f, 10.4f } },
    { { -4001, -127, 468, -116, 4024, 74, -441, 63, -3998 }, { -9, -217, 8 }, { -10.6f, -180.3f, 6.9f } },
    { { -3970, -116, 467, -116, 3994, -21, -438, -32, -3969 }, { -13, -119, 9 }, { -18.4f, -81.1f, 5.2f } },
    { { -3911, 222, 524, 228, 3972, 12, -496, 36, -3932 }, { -16, -34, 4 }, { -18.4f, -20.1f, 18.0f } },
    { { 4003, 191, 579, -193, 4025, -58, -609, 29, 3989 }, { 23, -216, 8 }, { 24.8f, -179.8f, 6.4f } },
    { { 3971, 175, 578, -192, 3994, 36, -605, -65, 3958 }, { 27, -118, 8 }, { 29.1f, -80.8f, 3.9f } },
    { { 3722, 226, 1398, -256, 3959, 25, -1420, -136, 3706 }, { 30, -33, 2 }, { 37.1f, -19.1f, 14.2f } },
};
/* the bloodied Incubator (BAR.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_barRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 3584, 1504, -1216, 128, 2336, 3328, 1920, -2976, 1984 }, { 36, -63, -24 }, { 24.1f, -84.5f, 0.4f } },
    { { 4033, -168, -41, 44, 380, 4037, -179, -4006, 352 }, { 7, -112, 32 }, { 7.6f, -106.3f, 51.4f } },
    { { 1059, -522, 3868, -1489, 3651, 870, -3583, -1672, 731 }, { 19, -100, 9 }, { -10.3f, -105.6f, 2.7f } },
    { { 3947, 586, -493, 295, 1006, 3858, 676, -3809, 923 }, { -13, -100, 0 }, { -17.3f, -89.5f, 3.6f } },
    { { 3926, 189, -676, 346, 2681, 2906, 588, -2917, 2598 }, { -22, -63, 8 }, { -28.9f, -44.1f, 24.7f } },
    { { -58, -3024, -2534, 3417, -1285, 1457, -1971, -2177, 2619 }, { -35, -27, 40 }, { -42.4f, -19.8f, 53.1f } },
    { { 1518, 1649, 3351, -2864, 2823, -148, -2381, -2348, 2238 }, { 19, -100, 9 }, { 42.3f, -100.1f, 23.8f } },
    { { 1120, 3419, 1750, 61, -1862, 3528, 3821, -935, -603 }, { 48, -96, 21 }, { 52.5f, -85.1f, 22.2f } },
    { { 950, 3703, -1044, 78, 1019, 3813, 3837, -936, 130 }, { 66, -62, 18 }, { 59.5f, -36.7f, 21.7f } },
    { { -1922, 3407, -395, -3322, -1982, -630, -728, 29, 3869 }, { 53, -14, 23 }, { 48.1f, -14.6f, 38.6f } },
    { { -2208, -704, 3328, -1984, 3488, -576, -2784, -1952, -2240 }, { 36, -63, -24 }, { 36.2f, -49.2f, -34.1f } },
    { { -323, -3967, 587, -3987, 370, 340, -371, -557, -3982 }, { 24, -41, -55 }, { -11.2f, -37.9f, -61.6f } },
    { { -321, 1732, 3575, -3956, 211, -451, -369, -3599, 1708 }, { -73, -32, -69 }, { -46.9f, -35.4f, -106.9f } },
    { { -1065, 3578, 1323, -3796, -833, -806, -443, -1475, 3656 }, { -33, -28, -145 }, { -25.6f, -29.4f, -163.3f } },
    { { -155, -3930, 846, 3872, -355, -990, 1067, 777, 3819 }, { 41, -26, -33 }, { 2.0f, -20.6f, -36.2f } },
    { { -154, 1665, -3604, 3841, 991, 307, 1058, -3475, -1655 }, { -55, -35, -15 }, { -36.2f, -9.8f, -70.6f } },
    { { 1712, 3405, -966, 3509, -1495, 988, 506, -1293, -3714 }, { -16, -15, -89 }, { -6.9f, -15.5f, -106.6f } },
};
/* Kaufmann (KAU.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_kauRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, -64, 320, 64, 4064, -160, -320, 160, 4064 }, { 0, -247, 0 }, { 0.6f, -306.2f, -3.4f } },
    { { 4031, -12, -63, -16, 4022, -386, 63, 410, 4021 }, { 1, -382, -10 }, { -0.5f, -394.2f, -2.7f } },
    { { -315, -108, 4031, 0, 4038, 95, -4039, 2, -317 }, { 0, -334, -12 }, { -40.5f, -332.2f, -12.5f } },
    { { 3947, -436, -521, 503, -98, 3965, -439, -3998, -29 }, { -44, -336, -9 }, { -51.2f, -314.9f, -10.8f } },
    { { 3909, -624, -374, 497, 1013, 3806, -499, -3812, 1096 }, { -59, -291, -13 }, { -63.3f, -253.1f, 2.8f } },
    { { 3878, -620, -372, 493, 1005, 3776, -496, -3783, 1087 }, { -66, -225, 6 }, { -70.0f, -204.9f, 22.7f } },
    { { -319, -27, 4032, 222, 4030, 31, -4030, 219, -319 }, { 0, -334, -12 }, { 39.8f, -330.8f, -18.7f } },
    { { 3989, 316, 284, -351, 536, 3950, 299, -3969, 539 }, { 43, -334, -16 }, { 49.2f, -312.8f, -15.0f } },
    { { 3952, 430, 206, -357, 1377, 3709, 358, -3723, 1390 }, { 54, -288, -13 }, { 57.0f, -251.6f, 5.6f } },
    { { 1989, 3364, 590, -1618, 314, 3571, 3030, -2045, 1520 }, { 57, -223, 11 }, { 54.7f, -203.3f, 29.0f } },
    { { -4064, 64, -128, 64, 4064, -352, 128, -352, -4032 }, { 0, -247, 0 }, { 0.5f, -217.6f, -1.2f } },
    { { -3937, -871, 301, -846, 3946, 446, -370, 356, -3988 }, { -16, -200, -4 }, { -27.1f, -165.7f, 1.0f } },
    { { -3907, -797, 450, -840, 3933, -274, -368, -369, -3959 }, { -34, -123, 2 }, { -43.4f, -82.0f, -2.8f } },
    { { -3925, 76, 709, -2, 3972, -472, -682, -497, -3885 }, { -50, -43, -14 }, { -54.1f, -25.6f, -1.4f } },
    { { 3968, 660, -334, -670, 3970, -478, 245, 494, 3991 }, { 16, -200, -5 }, { 25.5f, -165.7f, 1.3f } },
    { { 3937, 712, -167, -665, 3926, 471, 243, -461, 3950 }, { 28, -123, 4 }, { 37.3f, -82.0f, -1.6f } },
    { { 3898, -34, 728, 31, 3966, 112, -756, -120, 3874 }, { 42, -44, -14 }, { 46.3f, -28.0f, 0.0f } },
};
/* the ghost doctor (DOC.ILM, its ANM keyframe 0). */
static const PcSkinRestBone s_docRest[PC_SKIN_BONES] = {
    { { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 0 }, { 0, 0, 0 } },
    { { 4064, 32, -96, -32, 4064, 224, 96, -224, 4064 }, { 0, -275, 0 }, { -0.4f, -329.1f, -2.7f } },
    { { 4030, -26, -221, 70, 4018, 477, 217, -478, 4015 }, { -1, -387, 2 }, { -3.1f, -404.6f, 13.8f } },
    { { -31, 28, 4035, -95, 4039, -7, -4043, -96, -34 }, { -1, -355, 4 }, { -39.3f, -352.5f, -3.3f } },
    { { 4004, -98, -166, 180, -630, 3963, -133, -3967, -630 }, { -41, -355, 4 }, { -46.5f, -331.7f, -4.8f } },
    { { 3976, -223, 113, 4, 1169, 3810, -232, -3807, 1136 }, { -44, -305, -4 }, { -48.7f, -272.1f, -0.8f } },
    { { 3692, -1392, 112, 350, 1103, 3780, -1350, -3530, 1127 }, { -47, -246, 13 }, { -50.6f, -223.3f, 23.7f } },
    { { 62, 129, 4031, -318, 4026, -127, -4028, -316, 100 }, { -1, -355, 4 }, { 36.8f, -354.6f, -3.2f } },
    { { 3998, 38, 168, -155, -1003, 3875, 133, -3880, -1001 }, { 38, -357, 4 }, { 43.5f, -334.7f, -7.0f } },
    { { 3973, 138, -86, 51, 800, 3898, 175, -3893, 798 }, { 40, -308, -9 }, { 45.0f, -275.0f, -8.7f } },
    { { 3714, 1309, -86, -190, 771, 3867, 1321, -3629, 791 }, { 42, -248, 3 }, { 46.1f, -224.3f, 11.5f } },
    { { -4064, 64, -192, 64, 4064, 128, 192, 128, -4064 }, { 0, -275, 0 }, { 0.2f, -251.7f, -0.4f } },
    { { -4016, -279, 34, -248, 4001, 314, -44, 301, -4037 }, { -16, -232, 2 }, { -22.2f, -172.8f, 3.1f } },
    { { -3985, -257, 110, -247, 3901, -826, -44, -849, -3933 }, { -23, -143, 8 }, { -29.5f, -86.3f, -12.8f } },
    { { -3912, 133, 676, 167, 3954, 37, -679, 48, -3945 }, { -30, -38, -23 }, { -34.7f, -23.9f, -17.4f } },
    { { 4015, 186, 509, -181, 4033, -37, -512, 25, 4020 }, { 16, -232, 0 }, { 20.3f, -172.6f, -1.9f } },
    { { 3983, 107, 528, -180, 3975, 562, -508, -573, 3960 }, { 20, -142, 0 }, { 23.6f, -84.0f, -17.7f } },
    { { 3926, 86, 722, -86, 3996, -16, -739, 4, 3950 }, { 21, -35, -24 }, { 26.6f, -21.0f, -18.7f } },
};

/* A character the loader can retarget onto: its rest pose, sole height and
 * toe directions at rest (x, z, from the sole of 14LFOOT and 17RFOOT, which
 * splay), and whether its voice is female, which is how lip sync tells the
 * speakers of a voice line apart. */
typedef struct
{
    const char*           name;
    const PcSkinRestBone* rest;
    float                 soleY;
    float                 toe[2][2];
    int                   femaleVoice;
    const char*           alt; /* model whose glTF it wears when it has none */
    const char*           voice; /* speaker name its lines carry, if not its own */
} PcSkinRestDef;

static const PcSkinRestDef s_restDefs[] = {
    { "HERO", s_heroRest, -9.4f, { { -0.3360f, 0.9419f }, { 0.1961f, 0.9806f } }, 0 },
    { "SIBYL", s_sibylRest, -7.5f, { { -0.1840f, 0.9829f }, { 0.1715f, 0.9852f } }, 1 },
    /* Monster Cybil is Cybil's rig and mesh with other textures: MSB.ILM posed
     * by SBL.ANM's rest frame measures exactly s_sibylRest. Her own MSB.ANM
     * has no standing frame to take a rest from. */
    { "MSB", s_sibylRest, -7.5f, { { -0.1840f, 0.9829f }, { 0.1715f, 0.9852f } }, 1, "SIBYL", "SIBYL" },
    { "SRL", s_srlRest, 4.1f, { { 0.2092f, 0.9779f }, { -0.1916f, 0.9815f } }, 1 },
    { "DARIA", s_dariaRest, -11.9f, { { -0.0122f, 0.9999f }, { 0.4101f, 0.9121f } }, 1 },
    { "LISA", s_lisaRest, -14.6f, { { 0.0472f, 0.9989f }, { 0.0967f, 0.9953f } }, 1 },
    { "BLISA", s_blisaRest, -15.3f, { { -0.1031f, 0.9947f }, { -0.8551f, 0.5185f } }, 1, NULL, "LISA" },
    { "AR", s_arRest, -10.1f, { { -0.1909f, 0.9816f }, { 0.2362f, 0.9717f } }, 1 },
    { "TAR", s_tarRest, -3.9f, { { -0.0048f, 1.0000f }, { -0.0874f, 0.9962f } }, 1 },
    { "MAR", s_marRest, -8.9f, { { -0.1363f, 0.9907f }, { 0.3609f, 0.9326f } }, 1 },
    { "BAR", s_barRest, -0.4f, { { -0.7532f, 0.6578f }, { -0.0849f, 0.9964f } }, 1 },
    { "KAU", s_kauRest, -8.1f, { { -0.2550f, 0.9669f }, { 0.2585f, 0.9660f } }, 0 },
    { "DOC", s_docRest, -8.4f, { { -0.2123f, 0.9772f }, { 0.2213f, 0.9752f } }, 0 },
};

/* Where each bone's segment ends: a child joint, the midpoint of two (the
 * pelvis), or -1 for an end bone measured to its geometry's centroid. */
static const s8 s_childA[PC_SKIN_BONES] = { -1, 2, -1, 4, 5, 6, -1, 8, 9, 10, -1, 12, 13, 14, -1, 16, 17, -1 };
static const s8 s_childB[PC_SKIN_BONES] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 15, -1, -1, -1, -1, -1, -1 };
/* Limb segments follow the game's lengths. */
static const u8 s_scaled[PC_SKIN_BONES] = { 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0 };
/* Limb joints move onto the game's, so elbows and knees bend where the
 * animation bends them and a held weapon lands in the hand. The torso, head,
 * clavicles and feet only turn about the game's pivots: their joints sit at
 * different heights on a modern rig (Mixamo's Hips is the pelvis, the game's
 * is the waist), and moving them there lifted the pelvis, stretched the neck
 * and stood the feet off the floor.
 * Upper arms keep the model's shoulder WIDTH: a modern torso is broader than
 * Harry's, and pulling the sleeve onto his narrower shoulder sank it into the
 * coat. The forearm runs from the widened elbow to Harry's wrist, so the hand
 * still lands on a held weapon. Only X: keeping the model's shoulder depth
 * too set the upper arm back and kinked the forearm forward to the wrist. */
static const u8 s_pinned[PC_SKIN_BONES] = { 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 0 };

/* Mixamo base name -> game bone. The game's "L" parts sit at -X, which is the
 * model's own right side, so Mixamo "Right" drives them. */
static const struct { const char* name; s8 bone; } s_jointMap[] = {
    { "Hips", 11 }, { "Spine", 1 }, { "Spine1", 1 }, { "Spine2", 1 }, { "Spine3", 1 },
    { "Neck", 2 }, { "Neck1", 2 }, { "Head", 2 }, { "HeadTop_End", 2 },
    { "RightShoulder", 3 }, { "RightArm", 4 }, { "RightForeArm", 5 }, { "RightHand", 6 },
    { "LeftShoulder", 7 }, { "LeftArm", 8 }, { "LeftForeArm", 9 }, { "LeftHand", 10 },
    { "RightUpLeg", 12 }, { "RightLeg", 13 }, { "RightFoot", 14 }, { "RightToeBase", 14 }, { "RightToe_End", 14 },
    { "LeftUpLeg", 15 }, { "LeftLeg", 16 }, { "LeftFoot", 17 }, { "LeftToeBase", 17 }, { "LeftToe_End", 17 },
};

typedef struct
{
    float l[PC_SKIN_INFL][3]; /* bone-local position per influence */
    float w[PC_SKIN_INFL];
    u8    bone[PC_SKIN_INFL];
    u8    n;
    u8    dom;
    SVECTOR gteNormal;        /* dominant-bone-local, the ILM's inward sign */
    float   tan[4];           /* dominant-bone-local tangent, w = bitangent sign */
} PcSkinVert;

typedef struct
{
    u32   v[3];
    float uv[3][2];
} PcSkinTri;

/* Drawn indexed: one output vertex per (skin vertex, UV) pair, so a dense mesh
 * skins, lights and uploads each vertex once instead of once per corner. */
typedef struct
{
    PcSkinTri*      tris;
    int             triCount;
    u32*            corner;   /* triangle corner -> output vertex */
    u32*            src;      /* output vertex -> skin vertex */
    float         (*uv)[2];
    int             srcCount;
    unsigned char*  rgba;
    int             w, h;
    u32             texture;
    unsigned char*  nrgba;    /* normal map, if the material has one */
    int             nw, nh;
    u32             ntexture;
    /* Occlusion, roughness and metalness packed into R, G and B, the layout
     * glTF's ORM convention already uses, so one sampler serves all three. */
    unsigned char*  orgba;
    int             ow, oh;
    u32             otexture;
    float           metal, rough, aoStrength;
    unsigned char*  ergba;    /* emissive */
    int             ew, eh;
    u32             etexture;
    signed char     texFrom[4]; /* per map: the earlier material of this model that decoded the
                                 * same image, whose texture this one draws with; -1 = its own */
    float           emissive[3];
    GrModernVertex* out[PC_SKIN_INST][2];
    u32*            idx[PC_SKIN_INST][2];
    int             idxCount[PC_SKIN_INST][2];
} PcSkinMat;

/* A shape key the game drives, kept sparse: only the vertices it moves. */
typedef struct
{
    int    count;
    u32*   v;
    float (*d)[3]; /* game-space bind delta */
} PcSkinMorph;

/* GRIP_L/GRIP_R curl the fingers of game hand 6/10 round a held item. BREATH
 * is built from the chest at load, so every model gets it. */
enum { PC_SKIN_MORPH_BLINK, PC_SKIN_MORPH_MOUTH, PC_SKIN_MORPH_GRIP_L, PC_SKIN_MORPH_GRIP_R, PC_SKIN_MORPH_BREATH,
       PC_SKIN_MORPH_COUNT };

/* One frame's lights for the per-pixel path, in view space: the GTE light
 * rows and back colour of the bone that lights the most vertices, and the
 * point light's position so its row can be aimed from each pixel. */
typedef struct
{
    int   on, point, flat;
    float L[3][3], l1Mag, lightPos[3];
    float LC[3][3], BK[3];
    float bright;
} PcSkinLight;

typedef struct
{
    char        name[16]; /* the model it replaces: HERO, SIBYL */
    char        file[32]; /* the glTF it came from: HERO, HERO_CJ */
    char        base[96]; /* its path without the extension, for the ini */
    const PcSkinRestDef* def;
    int         state; /* 0 untried, 1 ready, -1 none */
    const void* instKey[PC_SKIN_INST]; /* the instance's bone coords */
    u32         instUse[PC_SKIN_INST];
    u32         instClock;
    int         inst;
    int         otShift[PC_SKIN_INST][2]; /* the OT shift it was sorted with */
    int         instLogged;
    int         vertCount;
    PcSkinVert* verts;
    int         matCount;
    PcSkinMat   mats[PC_SKIN_MAX_MATS];
    u32*        byBone[PC_SKIN_BONES];
    int         byBoneCount[PC_SKIN_BONES];

    /* frame state */
    float       rv[PC_SKIN_BONES][9];
    float       tv[PC_SKIN_BONES][3];
    u8          have[PC_SKIN_BONES];
    u8          lit[PC_SKIN_BONES];
    float       rw[PC_SKIN_BONES][9]; /* bone -> world, for the ragdoll */
    float       tw[PC_SKIN_BONES][3];
    s16         gteL[PC_SKIN_BONES][9];  /* light rows each bone was lit with */
    s32         gteBK[PC_SKIN_BONES][3];
    s16         gteLC[9];
    PcSkinLight light[PC_SKIN_INST][2];
    int         isPlayer;

    /* retarget, kept for shape keys (delta -> bone-local per influence) */
    float       rt[PC_SKIN_BONES][9]; /* R^T */
    float       ax[PC_SKIN_BONES][3];
    float       kk[PC_SKIN_BONES];
    PcSkinMorph morph[PC_SKIN_MORPH_COUNT];
    float       blinkClock, blinkNext;
    float       breathPhase, breathPeriod;
    float       mouth;
    float       grip[2];
    float       gunOff[3]; /* right-hand bone frame */
    int         lightLogged;
    int         gunRaise, gunOut, gunFwd;    /* ini: mm */
    int         gunTwist, gunPitch, gunYaw; /* ini: degrees */
    int         handRelax; /* ini: percent of the fist held with empty hands */
    int         female;
    int         native; /* built in game space (SH_NATIVE node): the game's own parts */
    u32         nativeBones; /* the bones whose stock parts it replaces */
    int         brightness; /* percent of the lit vertex colour */
    int         fogPct;     /* percent of the game's fog amount */
    int         beamPct;    /* percent of the flashlight beam it takes */
    int         lightOff[3]; /* flashlight origin shift, cm, torso space */
    CVECTOR*    color;
    float*      pos;  /* view x,y,z */
    float*      scr;  /* screen x,y */
    u8*         ok;
} PcSkinChara;

static PcSkinChara s_chara[PC_SKIN_MAX_CHARA];
/* The play-as Harry swap's file base (HERO_CJ), or empty for HERO itself. */
static char        s_heroFile[32];
static int         s_charaCount;
static char        s_missing[PC_SKIN_MAX_MISSING][32];
static int         s_missingCount;

/* ---------------------------------------------------------------- math */

static void M3_FromQ12(const s16* q, float* m)
{
    int i;
    for (i = 0; i < 9; i++)
        m[i] = q[i] / 4096.0f;
}

static void M3_Mul(const float* a, const float* b, float* o)
{
    int r, c;
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

static void M3_MulTV(const float* m, const float* v, float* o) /* m^T v */
{
    o[0] = m[0] * v[0] + m[3] * v[1] + m[6] * v[2];
    o[1] = m[1] * v[0] + m[4] * v[1] + m[7] * v[2];
    o[2] = m[2] * v[0] + m[5] * v[1] + m[8] * v[2];
}

static void M3_MulV(const float* m, const float* v, float* o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[3] * v[0] + m[4] * v[1] + m[5] * v[2];
    o[2] = m[6] * v[0] + m[7] * v[1] + m[8] * v[2];
}

static float V3_Len(const float* v)
{
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* Shortest rotation taking direction a onto direction b. */
static void M3_Align(const float* a0, const float* b0, float* m)
{
    float a[3], b[3], v[3], c, s, k;
    float la = V3_Len(a0), lb = V3_Len(b0);
    int i;

    for (i = 0; i < 9; i++)
        m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    if (la < 1e-6f || lb < 1e-6f)
        return;
    for (i = 0; i < 3; i++)
    {
        a[i] = a0[i] / la;
        b[i] = b0[i] / lb;
    }
    v[0] = a[1] * b[2] - a[2] * b[1];
    v[1] = a[2] * b[0] - a[0] * b[2];
    v[2] = a[0] * b[1] - a[1] * b[0];
    c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    s = V3_Len(v);
    if (s < 1e-6f)
        return; /* parallel; antiparallel never occurs between a rest bone and its model twin */
    k = (1.0f - c) / (s * s);
    m[0] = 1 - k * (v[1] * v[1] + v[2] * v[2]); m[1] = -v[2] + k * v[0] * v[1];      m[2] = v[1] + k * v[0] * v[2];
    m[3] = v[2] + k * v[0] * v[1];              m[4] = 1 - k * (v[0] * v[0] + v[2] * v[2]); m[5] = -v[0] + k * v[1] * v[2];
    m[6] = -v[1] + k * v[0] * v[2];             m[7] = v[0] + k * v[1] * v[2];      m[8] = 1 - k * (v[0] * v[0] + v[1] * v[1]);
}

/* Column-major glTF 4x4 inverse of a rigid-plus-scale bind matrix; only the
 * translation of the inverse is needed (the joint's bind position). */
static int M4_InvTranslation(const float* m, float* t)
{
    float a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float inv[9];
    float tx = m[12], ty = m[13], tz = m[14];

    if (fabsf(det) < 1e-12f)
        return 0;
    inv[0] = (e * i - f * h) / det; inv[1] = (c * h - b * i) / det; inv[2] = (b * f - c * e) / det;
    inv[3] = (f * g - d * i) / det; inv[4] = (a * i - c * g) / det; inv[5] = (c * d - a * f) / det;
    inv[6] = (d * h - e * g) / det; inv[7] = (b * g - a * h) / det; inv[8] = (a * e - b * d) / det;
    t[0] = -(inv[0] * tx + inv[1] * ty + inv[2] * tz);
    t[1] = -(inv[3] * tx + inv[4] * ty + inv[5] * tz);
    t[2] = -(inv[6] * tx + inv[7] * ty + inv[8] * tz);
    return 1;
}

/* ---------------------------------------------------------------- loading */

static void Skin_BaseName(const char* full, char* base, size_t size)
{
    const char* s = strrchr(full, ':');
    size_t n, i;

    s = s ? s + 1 : full;
    snprintf(base, size, "%s", s);
    /* Sketchfab appends "_<index>" to every node name. */
    n = strlen(base);
    i = n;
    while (i > 0 && base[i - 1] >= '0' && base[i - 1] <= '9')
        i--;
    if (i > 0 && i < n && base[i - 1] == '_')
        base[i - 1] = '\0';
}

/* Rigs that are not Mixamo (Blender/Rigify "upper_arm.L", XPS "Arm.001.L",
 * Unreal "upperarm_l", 3ds Max "Bip01 L UpperArm", VRoid "J_Bip_L_UpperArm",
 * DAZ "lShldrBend") are read by side plus a body-part word. A bare "arm" or
 * "leg" is told apart by how many of its sided ancestors are arm or leg too. */
enum { SKIN_CLAV, SKIN_UPARM, SKIN_FOREARM, SKIN_HAND, SKIN_THIGH, SKIN_SHIN, SKIN_FOOT, SKIN_ARM, SKIN_LEG };
static const s8 s_limbBone[2][7] = { { 3, 4, 5, 6, 12, 13, 14 }, { 7, 8, 9, 10, 15, 16, 17 } };
static const struct { const char* key; s8 cls; } s_limbKeys[] = {
    { "armature", -1 },
    { "finger", SKIN_HAND }, { "thumb", SKIN_HAND }, { "index", SKIN_HAND }, { "middle", SKIN_HAND },
    { "ring", SKIN_HAND }, { "pinky", SKIN_HAND }, { "little", SKIN_HAND }, { "hand", SKIN_HAND },
    { "wrist", SKIN_HAND }, { "palm", SKIN_HAND },
    { "forearm", SKIN_FOREARM }, { "lowerarm", SKIN_FOREARM }, { "elbow", SKIN_FOREARM },
    { "upperarm", SKIN_UPARM }, { "uparm", SKIN_UPARM }, { "shldr", SKIN_UPARM },
    { "clavicle", SKIN_CLAV }, { "collar", SKIN_CLAV }, { "shoulder", SKIN_CLAV }, { "arm", SKIN_ARM },
    { "toe", SKIN_FOOT }, { "ball", SKIN_FOOT }, { "foot", SKIN_FOOT }, { "ankle", SKIN_FOOT },
    { "calf", SKIN_SHIN }, { "shin", SKIN_SHIN }, { "knee", SKIN_SHIN }, { "lowerleg", SKIN_SHIN },
    { "thigh", SKIN_THIGH }, { "upleg", SKIN_THIGH }, { "upperleg", SKIN_THIGH }, { "hip", SKIN_THIGH },
    { "leg", SKIN_LEG },
};
static const struct { const char* key; s8 bone; } s_torsoKeys[] = {
    { "pelvis", 11 }, { "hip", 11 }, { "spine", 1 }, { "chest", 1 }, { "torso", 1 }, { "abdomen", 1 },
    { "neck", 2 }, { "head", 2 },
};

/* Returns a game bone (>= 0), -1 for none, or -2 with side and cls set for a limb. */
static int Skin_NameClass(const char* full, int* side, int* cls)
{
    char base[64], comp[64];
    const char* t;
    size_t i, n = 0;

    Skin_BaseName(full, base, sizeof(base));
    for (i = 0; i < sizeof(s_jointMap) / sizeof(s_jointMap[0]); i++)
        if (strcmp(base, s_jointMap[i].name) == 0)
            return s_jointMap[i].bone;
    if (strncmp(base, "RightHand", 9) == 0)
        return 6;
    if (strncmp(base, "LeftHand", 8) == 0)
        return 10;

    *side = -1;
    for (t = base; *t != '\0';)
    {
        char tok[64];
        size_t len = 0, c;
        while (*t != '\0' && !isalnum((unsigned char)*t))
            t++;
        while (isalnum((unsigned char)t[len]))
            len++;
        if (len == 0)
            break;
        for (c = 0; c < len && c < sizeof(tok) - 1; c++)
            tok[c] = (char)tolower((unsigned char)t[c]);
        tok[c] = '\0';
        if (strcmp(tok, "l") == 0 || strcmp(tok, "left") == 0)
            *side = 1;
        else if (strcmp(tok, "r") == 0 || strcmp(tok, "right") == 0)
            *side = 0;
        else
            for (c = 0; c < len && n < sizeof(comp) - 1; c++)
                if (isalpha((unsigned char)tok[c]))
                    comp[n++] = tok[c];
        t += len;
    }
    comp[n] = '\0';
    if (*side < 0)
    {
        if (strncmp(comp, "left", 4) == 0)
            *side = 1, memmove(comp, comp + 4, strlen(comp + 4) + 1);
        else if (strncmp(comp, "right", 5) == 0)
            *side = 0, memmove(comp, comp + 5, strlen(comp + 5) + 1);
        else if ((base[0] == 'l' || base[0] == 'r') && isupper((unsigned char)base[1]))
            *side = (base[0] == 'l'), memmove(comp, comp + 1, strlen(comp + 1) + 1);
    }

    /* Face rigs carry sided joints ("eyeball left") whose words collide with limb ones. */
    if (strstr(comp, "head") || strstr(comp, "neck") || strstr(comp, "eye") || strstr(comp, "face") || strstr(comp, "jaw"))
        return 2;
    if (*side >= 0)
        for (i = 0; i < sizeof(s_limbKeys) / sizeof(s_limbKeys[0]); i++)
            if (strstr(comp, s_limbKeys[i].key))
            {
                if (s_limbKeys[i].cls < 0)
                    return -1;
                *cls = s_limbKeys[i].cls;
                return -2;
            }
    if (strstr(comp, "armature"))
        return -1;
    for (i = 0; i < sizeof(s_torsoKeys) / sizeof(s_torsoKeys[0]); i++)
        if (strstr(comp, s_torsoKeys[i].key))
            return s_torsoKeys[i].bone;
    return -1;
}

/* The node's own game bone, without falling back to an ancestor. */
static int Skin_NodeBone(const cgltf_node* node)
{
    int side, cls, depth = 0, r = node->name ? Skin_NameClass(node->name, &side, &cls) : -1;
    const cgltf_node* p;

    if (r != -2)
        return r;
    if (cls == SKIN_ARM || cls == SKIN_LEG)
    {
        for (p = node->parent; p != NULL; p = p->parent)
        {
            int ps, pc, pr = p->name ? Skin_NameClass(p->name, &ps, &pc) : -1;
            if (pr == -2 && ps == side &&
                (cls == SKIN_ARM ? (pc == SKIN_UPARM || pc == SKIN_ARM) : (pc == SKIN_THIGH || pc == SKIN_LEG)))
                depth++;
            else if (cls == SKIN_ARM ? (pr == 4 || pr == 8) : (pr == 12 || pr == 13 || pr == 15 || pr == 16))
                depth++;
        }
        if (cls == SKIN_ARM)
            cls = depth == 0 ? SKIN_UPARM : SKIN_FOREARM;
        else
            cls = depth == 0 ? SKIN_THIGH : depth == 1 ? SKIN_SHIN : SKIN_FOOT;
    }
    return s_limbBone[side][cls];
}

static int Skin_JointIndex(const cgltf_skin* skin, const cgltf_node* node)
{
    size_t j;
    for (j = 0; j < skin->joints_count; j++)
        if (skin->joints[j] == node)
            return (int)j;
    return -1;
}

/* For each hand (0 = game hand 6), the hand joint and three joints of one
 * finger, for the grip. A finger named "index" wins; otherwise the thumb (by
 * name, or the chain rooted nearest the wrist) is skipped and the middle-most
 * of the rest is used: any long finger gives the curl direction. Slots 3 and 4
 * are the finger root farthest from it and the thumb tip, for a rig modelled
 * with dead-straight fingers. */
/* Horizontal toe direction of the model's foot: the long axis of the
 * sole (the lowest 15 units of the geometry the foot owns), signed away
 * from the ankle. A boot's shaft would pull a plain centroid upward. */
static int Skin_ToeDir(const PcSkinChara* ch, const float* gpos, int bone, const float* ankle, float* dir)
{
    float maxY = -1e30f, mx = 0.0f, mz = 0.0f, sxx = 0.0f, sxz = 0.0f, szz = 0.0f, th;
    int j, n = 0;

    for (j = 0; j < ch->vertCount; j++)
        if (ch->verts[j].dom == bone && gpos[j * 3 + 1] > maxY)
            maxY = gpos[j * 3 + 1];
    for (j = 0; j < ch->vertCount; j++)
        if (ch->verts[j].dom == bone && gpos[j * 3 + 1] > maxY - 15.0f)
        {
            mx += gpos[j * 3];
            mz += gpos[j * 3 + 2];
            n++;
        }
    if (n < 3)
        return 0;
    mx /= n;
    mz /= n;
    for (j = 0; j < ch->vertCount; j++)
        if (ch->verts[j].dom == bone && gpos[j * 3 + 1] > maxY - 15.0f)
        {
            float dx = gpos[j * 3] - mx, dz = gpos[j * 3 + 2] - mz;
            sxx += dx * dx;
            sxz += dx * dz;
            szz += dz * dz;
        }
    th = 0.5f * atan2f(2.0f * sxz, sxx - szz);
    dir[0] = cosf(th);
    dir[1] = 0.0f;
    dir[2] = sinf(th);
    if (dir[0] * (mx - ankle[0]) + dir[2] * (mz - ankle[2]) < 0.0f)
    {
        dir[0] = -dir[0];
        dir[2] = -dir[2];
    }
    return 1;
}

static void Skin_FindGrip(const cgltf_skin* skin, float jointPos[][3], int handJoint[2], int fingerJoint[2][5])
{
    int side;

    for (side = 0; side < 2; side++)
    {
        int roots[16], rootCount = 0, named = -1, thumb = -1, best = -1, i, f;
        float ctr[3] = { 0, 0, 0 }, bestD = 1e30f, thumbD = 1e30f;
        size_t j;
        const cgltf_node* hand;

        for (j = 0; j < skin->joints_count && handJoint[side] < 0; j++)
            if (Skin_NodeBone(skin->joints[j]) == s_limbBone[side][SKIN_HAND] &&
                (skin->joints[j]->parent == NULL || Skin_NodeBone(skin->joints[j]->parent) != s_limbBone[side][SKIN_HAND]))
                handJoint[side] = (int)j;
        if (handJoint[side] < 0)
            continue;
        hand = skin->joints[handJoint[side]];
        for (j = 0; j < hand->children_count && rootCount < 16; j++)
        {
            const cgltf_node* c = hand->children[j];
            if (Skin_JointIndex(skin, c) < 0 || c->children_count == 0 || c->children[0]->children_count == 0)
                continue;
            if (c->name != NULL)
            {
                char lower[64];
                size_t k;
                for (k = 0; c->name[k] != '\0' && k < sizeof(lower) - 1; k++)
                    lower[k] = (char)tolower((unsigned char)c->name[k]);
                lower[k] = '\0';
                if (strstr(lower, "index") && named < 0)
                    named = rootCount;
                if (strstr(lower, "thumb"))
                    thumb = rootCount;
            }
            roots[rootCount++] = Skin_JointIndex(skin, c);
        }
        if (rootCount == 0)
            continue;
        if (named < 0)
        {
            if (thumb < 0)
                for (i = 0; i < rootCount; i++)
                {
                    float d = 0.0f;
                    for (f = 0; f < 3; f++)
                        d += (jointPos[roots[i]][f] - jointPos[handJoint[side]][f]) * (jointPos[roots[i]][f] - jointPos[handJoint[side]][f]);
                    if (d < thumbD)
                        thumbD = d, thumb = i;
                }
            if (rootCount < 2)
                thumb = -1;
            for (i = 0; i < rootCount; i++)
                if (i != thumb)
                    for (f = 0; f < 3; f++)
                        ctr[f] += jointPos[roots[i]][f] / (float)(rootCount - (thumb >= 0));
            for (i = 0; i < rootCount; i++)
            {
                float d = 0.0f;
                if (i == thumb)
                    continue;
                for (f = 0; f < 3; f++)
                    d += (jointPos[roots[i]][f] - ctr[f]) * (jointPos[roots[i]][f] - ctr[f]);
                if (d < bestD)
                    bestD = d, best = i;
            }
            named = best;
        }
        if (named < 0)
            continue;
        {
            const cgltf_node* n = skin->joints[roots[named]];
            float farD = 0.0f;
            for (f = 0; f < 3 && n != NULL; f++)
            {
                fingerJoint[side][f] = Skin_JointIndex(skin, n);
                n = n->children_count ? n->children[0] : NULL;
            }
            for (i = 0; i < rootCount; i++)
            {
                float d = 0.0f;
                if (i == named || i == thumb)
                    continue;
                for (f = 0; f < 3; f++)
                    d += (jointPos[roots[i]][f] - jointPos[roots[named]][f]) * (jointPos[roots[i]][f] - jointPos[roots[named]][f]);
                if (d > farD)
                    farD = d, fingerJoint[side][3] = roots[i];
            }
            if (thumb >= 0)
            {
                n = skin->joints[roots[thumb]];
                while (n->children_count && Skin_JointIndex(skin, n->children[0]) >= 0)
                    n = n->children[0];
                fingerJoint[side][4] = Skin_JointIndex(skin, n);
            }
        }
    }
}

static cgltf_result Skin_FileRead(const struct cgltf_memory_options* mem, const struct cgltf_file_options* file,
                                  const char* path, cgltf_size* size, void** data)
{
    FILE* f = Pc_LooseFOpen(path, "rb");
    long n;
    void* bytes;

    (void)mem;
    (void)file;
    if (f == NULL)
        return cgltf_result_file_not_found;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = (n > 0) ? malloc((size_t)n) : NULL;
    if (bytes == NULL || fread(bytes, 1, (size_t)n, f) != (size_t)n)
    {
        free(bytes);
        fclose(f);
        return cgltf_result_io_error;
    }
    fclose(f);
    *size = (cgltf_size)n;
    *data = bytes;
    return cgltf_result_success;
}

static void Skin_FileRelease(const struct cgltf_memory_options* mem, const struct cgltf_file_options* file, void* data)
{
    (void)mem;
    (void)file;
    free(data);
}

static void Skin_DecodeTexture(const cgltf_texture* tex, const char* gltfPath, unsigned char** rgba, int* w,
                               int* h, const char* what);

/* metallicRoughness (G rough, B metal) and occlusion (R) are separate glTF
 * textures that are often the same image; either may be missing, and they need
 * not share a size, so the other is sampled nearest onto the first's grid. */
static void Skin_PackOrm(PcSkinMat* mat, unsigned char* mr, int mw, int mh, unsigned char* ao, int aw, int ah)
{
    int w = mr ? mw : aw, h = mr ? mh : ah, x, y;

    if ((mr == NULL && ao == NULL) || w <= 0 || h <= 0 || (mat->orgba = (unsigned char*)malloc((size_t)w * h * 4)) == NULL)
        return;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            unsigned char* o = &mat->orgba[((size_t)y * w + x) * 4];
            o[0] = ao ? ao[(((size_t)(y * ah / h)) * aw + (x * aw / w)) * 4] : 255;
            o[1] = mr ? mr[((size_t)y * w + x) * 4 + 1] : 255;
            o[2] = mr ? mr[((size_t)y * w + x) * 4 + 2] : 255;
            o[3] = 255;
        }
    }
    mat->ow = w;
    mat->oh = h;
}

enum { SKIN_MAP_BASE, SKIN_MAP_NORMAL, SKIN_MAP_ORM, SKIN_MAP_EMISSIVE };

static void Skin_MapFields(PcSkinMat* mat, int map, unsigned char*** rgba, int** w, int** h, u32** tex)
{
    switch (map)
    {
        case SKIN_MAP_BASE:   *rgba = &mat->rgba;  *w = &mat->w;  *h = &mat->h;  *tex = &mat->texture;  break;
        case SKIN_MAP_NORMAL: *rgba = &mat->nrgba; *w = &mat->nw; *h = &mat->nh; *tex = &mat->ntexture; break;
        case SKIN_MAP_ORM:    *rgba = &mat->orgba; *w = &mat->ow; *h = &mat->oh; *tex = &mat->otexture; break;
        default:              *rgba = &mat->ergba; *w = &mat->ew; *h = &mat->eh; *tex = &mat->etexture; break;
    }
}

/* The glTF image behind one of a material's maps (ORM is packed from two). */
static const cgltf_image* Skin_MapImage(const cgltf_material* m, int map, const cgltf_image** second)
{
    const cgltf_texture* t = NULL;

    *second = NULL;
    if (m == NULL)
        return NULL;
    switch (map)
    {
        case SKIN_MAP_BASE:
            if (m->has_pbr_metallic_roughness)
                t = m->pbr_metallic_roughness.base_color_texture.texture;
            break;
        case SKIN_MAP_NORMAL:
            t = m->normal_texture.texture;
            break;
        case SKIN_MAP_ORM:
            if (m->has_pbr_metallic_roughness)
                t = m->pbr_metallic_roughness.metallic_roughness_texture.texture;
            if (m->occlusion_texture.texture != NULL)
                *second = m->occlusion_texture.texture->image;
            break;
        default:
            t = m->emissive_texture.texture;
            break;
    }
    return t != NULL ? t->image : NULL;
}

/* An earlier material of this model that already decoded the same image for
 * this map, or -1. Materials often share one texture sheet, and decoding it
 * once per material multiplied a model's texture memory by up to its
 * material count (Heather: 18 materials, 99 MB of maps from 18 MB of images). */
static int Skin_SharedMap(PcSkinChara* ch, int mi, int map)
{
    const cgltf_image* second;
    const cgltf_image* img = Skin_MapImage((const cgltf_material*)(void*)ch->mats[mi].out[0][1], map, &second);
    int j;

    if (img == NULL && second == NULL)
        return -1;
    for (j = 0; j < mi; j++)
    {
        const cgltf_image* js;
        unsigned char** rgba;
        int *w, *h;
        u32* tex;

        if (ch->mats[j].texFrom[map] >= 0 ||
            Skin_MapImage((const cgltf_material*)(void*)ch->mats[j].out[0][1], map, &js) != img || js != second)
            continue;
        Skin_MapFields(&ch->mats[j], map, &rgba, &w, &h, &tex);
        if (*rgba != NULL)
            return j;
    }
    return -1;
}

/* A map's GL texture, made on first draw. The decoded copy is freed once it
 * exists: nothing makes the texture again, and keeping it held a second copy
 * of every map, hundreds of MB for a model with many large maps -- enough for iOS to
 * end the game. */
static u32 Skin_MapTexture(PcSkinChara* ch, int m, int map)
{
    PcSkinMat* mat = &ch->mats[m];
    unsigned char** rgba;
    int *w, *h;
    u32* tex;

    Skin_MapFields(mat, map, &rgba, &w, &h, &tex);
    if (*tex != 0)
        return *tex;
    if (mat->texFrom[map] >= 0)
        *tex = Skin_MapTexture(ch, mat->texFrom[map], map);
    else if (*rgba != NULL && (*tex = HiresOverride_CreateTextureRGBA(*rgba, *w, *h)) != 0)
    {
        free(*rgba);
        *rgba = NULL;
    }
    return *tex;
}

static void Skin_LoadImage(PcSkinChara* ch, int mi, const cgltf_material* m, const char* gltfPath)
{
    PcSkinMat* mat = &ch->mats[mi];
    unsigned char *mr = NULL, *ao = NULL;
    int mw = 0, mh = 0, aw = 0, ah = 0, i, j, shared[4];
    float es;

    memset(mat->texFrom, -1, sizeof(mat->texFrom));

    /* Without a metallic-roughness block glTF means fully metallic and rough,
     * which on a model that simply never set one reads as dull grey metal. */
    mat->metal = 0.0f;
    mat->rough = 0.6f;
    mat->aoStrength = 1.0f;
    if (m == NULL)
        return;
    for (i = 0; i < 4; i++)
        shared[i] = Skin_SharedMap(ch, mi, i);
    if (m->has_pbr_metallic_roughness)
    {
        if (shared[SKIN_MAP_BASE] < 0)
            Skin_DecodeTexture(m->pbr_metallic_roughness.base_color_texture.texture, gltfPath, &mat->rgba, &mat->w,
                               &mat->h, m->name);
        if (shared[SKIN_MAP_ORM] < 0)
            Skin_DecodeTexture(m->pbr_metallic_roughness.metallic_roughness_texture.texture, gltfPath, &mr, &mw,
                               &mh, m->name);
        mat->metal = m->pbr_metallic_roughness.metallic_factor;
        mat->rough = m->pbr_metallic_roughness.roughness_factor;
    }
    if (shared[SKIN_MAP_NORMAL] < 0)
        Skin_DecodeTexture(m->normal_texture.texture, gltfPath, &mat->nrgba, &mat->nw, &mat->nh, m->name);
    if (m->occlusion_texture.texture != NULL)
    {
        if (shared[SKIN_MAP_ORM] < 0)
            Skin_DecodeTexture(m->occlusion_texture.texture, gltfPath, &ao, &aw, &ah, m->name);
        mat->aoStrength = m->occlusion_texture.scale;
    }
    Skin_PackOrm(mat, mr, mw, mh, ao, aw, ah);
    free(mr);
    free(ao);
    es = m->has_emissive_strength ? m->emissive_strength.emissive_strength : 1.0f;
    for (i = 0; i < 3; i++)
        mat->emissive[i] = m->emissive_factor[i] * es;
    if ((mat->emissive[0] > 0.0f || mat->emissive[1] > 0.0f || mat->emissive[2] > 0.0f) &&
        shared[SKIN_MAP_EMISSIVE] < 0)
        Skin_DecodeTexture(m->emissive_texture.texture, gltfPath, &mat->ergba, &mat->ew, &mat->eh, m->name);
    else if (mat->emissive[0] <= 0.0f && mat->emissive[1] <= 0.0f && mat->emissive[2] <= 0.0f)
        shared[SKIN_MAP_EMISSIVE] = -1;
    for (j = 0; j < 4; j++)
        mat->texFrom[j] = (signed char)shared[j];
}

#if defined(__ANDROID__) || defined(SH_IOS) || defined(__EMSCRIPTEN__)
#define SKIN_MAX_MAP 1024

/* A 2x2 box down to half size, in place of the original. */
static void Skin_HalveRGBA(unsigned char** rgba, int* w, int* h)
{
    int nw = *w > 1 ? *w / 2 : 1, nh = *h > 1 ? *h / 2 : 1, x, y, c;
    unsigned char* out = (unsigned char*)malloc((size_t)nw * nh * 4);

    if (out == NULL)
        return;
    for (y = 0; y < nh; y++)
    {
        int y0 = y * 2 < *h ? y * 2 : *h - 1, y1 = y0 + 1 < *h ? y0 + 1 : y0;
        for (x = 0; x < nw; x++)
        {
            int x0 = x * 2 < *w ? x * 2 : *w - 1, x1 = x0 + 1 < *w ? x0 + 1 : x0;
            for (c = 0; c < 4; c++)
                out[((size_t)y * nw + x) * 4 + c] =
                    (unsigned char)(((*rgba)[((size_t)y0 * *w + x0) * 4 + c] + (*rgba)[((size_t)y0 * *w + x1) * 4 + c] +
                                     (*rgba)[((size_t)y1 * *w + x0) * 4 + c] + (*rgba)[((size_t)y1 * *w + x1) * 4 + c] + 2) / 4);
        }
    }
    free(*rgba);
    *rgba = out;
    *w = nw;
    *h = nh;
}
#endif

static void Skin_DecodeTexture(const cgltf_texture* tex, const char* gltfPath, unsigned char** rgba, int* w,
                               int* h, const char* what)
{
    const cgltf_image* img;
    unsigned char* bytes = NULL;
    cgltf_size size = 0;
    int owned = 0;

    if (tex == NULL || (img = tex->image) == NULL)
        return;
    if (img->buffer_view != NULL && img->buffer_view->buffer->data != NULL)
    {
        bytes = (unsigned char*)img->buffer_view->buffer->data + img->buffer_view->offset;
        size = img->buffer_view->size;
    }
    else if (img->uri != NULL && strncmp(img->uri, "data:", 5) != 0)
    {
        char path[512], uri[256];
        const char* slash = strrchr(gltfPath, '/');
        void* data = NULL;
        cgltf_options opt;

        snprintf(uri, sizeof(uri), "%s", img->uri);
        cgltf_decode_uri(uri);
        snprintf(path, sizeof(path), "%.*s%s", slash ? (int)(slash - gltfPath + 1) : 0, gltfPath, uri);
        memset(&opt, 0, sizeof(opt));
        if (Skin_FileRead(&opt.memory, &opt.file, path, &size, &data) != cgltf_result_success)
        {
            SH_DBG("[SKIN] texture %s not found", path);
            return;
        }
        bytes = (unsigned char*)data;
        owned = 1;
    }
    if (bytes != NULL && size > 0 && size < 0xffffffffu &&
        HiresOverride_DecodeToRGBA(bytes, (unsigned)size, rgba, w, h) != 0)
    {
        SH_DBG("[SKIN] texture for material %s did not decode (PNG only)", what ? what : "?");
        *rgba = NULL;
    }
#if defined(__ANDROID__) || defined(SH_IOS) || defined(__EMSCRIPTEN__)
    /* A phone keeps every model it has shown for the session, and the larger
     * ones carry a dozen or more 1-2K maps, ~100 MB of texture each. Past 1024
     * they are halved: a character on a phone screen is too small to show it. */
    while (*rgba != NULL && (*w > SKIN_MAX_MAP || *h > SKIN_MAX_MAP))
        Skin_HalveRGBA(rgba, w, h);
#endif
    if (owned)
        free(bytes);
}


/* Which driven key a target name is, or -1. A sided blink ("eyeBlink_L") only
 * counts when the mesh has no both-eyes one, or both would apply. */
static int Skin_MorphKind(const char* name, int haveBothBlink)
{
    char l[64];
    size_t i;
    int sided;

    if (name == NULL)
        return -1;
    for (i = 0; i + 1 < sizeof(l) && name[i]; i++)
        l[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    l[i] = '\0';
    if (strstr(l, "blink") || strstr(l, "eyesclosed") || strstr(l, "eyes_closed") || strstr(l, "eyeclose"))
    {
        sided = strstr(l, "left") || strstr(l, "right") || (i > 2 && l[i - 2] == '_' && (l[i - 1] == 'l' || l[i - 1] == 'r'));
        return (sided && haveBothBlink) ? -1 : PC_SKIN_MORPH_BLINK;
    }
    if (strstr(l, "jawopen") || strstr(l, "mouthopen") || strstr(l, "mouth_open") || strstr(l, "viseme_aa"))
        return PC_SKIN_MORPH_MOUTH;
    return -1;
}

static void Skin_LoadMorphs(PcSkinChara* ch, const cgltf_mesh* mesh, const cgltf_primitive* p, int vbase, float scale)
{
    int haveBoth = 0;
    size_t t, a, v;

    if (p->targets_count == 0 || mesh->target_names_count != p->targets_count)
        return;
    for (t = 0; t < p->targets_count; t++)
    {
        const char* n = mesh->target_names[t];
        if (Skin_MorphKind(n, 0) == PC_SKIN_MORPH_BLINK && Skin_MorphKind(n, 1) == PC_SKIN_MORPH_BLINK)
            haveBoth = 1;
    }
    for (t = 0; t < p->targets_count; t++)
    {
        int kind = Skin_MorphKind(mesh->target_names[t], haveBoth);
        const cgltf_accessor* acc = NULL;
        PcSkinMorph* m;
        float* all;

        if (kind < 0)
            continue;
        for (a = 0; a < p->targets[t].attributes_count; a++)
            if (p->targets[t].attributes[a].type == cgltf_attribute_type_position)
                acc = p->targets[t].attributes[a].data;
        if (acc == NULL)
            continue;
        m = &ch->morph[kind];
        m->v = (u32*)realloc(m->v, sizeof(u32) * (m->count + acc->count));
        m->d = (float(*)[3])realloc(m->d, sizeof(float[3]) * (m->count + acc->count));
        /* Unpacked whole: Blender writes shape keys as sparse accessors by
         * default, and cgltf_accessor_read_float refuses those, leaving each
         * delta uninitialised. */
        all = (float*)malloc(sizeof(float) * 3 * acc->count);
        if (m->v == NULL || m->d == NULL || all == NULL)
        {
            m->count = 0;
            free(all);
            continue;
        }
        if (cgltf_accessor_unpack_floats(acc, all, acc->count * 3) != acc->count * 3)
            memset(all, 0, sizeof(float) * 3 * acc->count);
        for (v = 0; v < acc->count; v++)
        {
            const float* d = &all[v * 3];
            if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f)
                continue;
            m->v[m->count] = (u32)(vbase + v);
            m->d[m->count][0] = d[0] * scale;
            m->d[m->count][1] = -d[1] * scale;
            m->d[m->count][2] = d[2] * scale;
            m->count++;
        }
        free(all);
        SH_DBG("[SKIN] shape key \"%s\" drives %s", mesh->target_names[t],
               kind == PC_SKIN_MORPH_BLINK ? "blinking" : "the mouth");
    }
}


/* A fist for each hand, as a bind-space shape key: everything past the index
 * knuckle bends round an axis across the knuckles, more the further out it is,
 * in the direction the model's index finger already curls. Many rigs have no
 * bones for the other fingers (or none at all), so this works on the mesh. */
#define PC_SKIN_GRIP_CURL 3.3f

/* Breathing: the ribcage between waist and neck swells forward (the back
 * barely), widens a little and lifts, peaking mid-chest and fading out at
 * both ends so the belt and collar stay put. About 5 mm at the sternum on a
 * person of Harry's height: enough to read in a close-up, not in a walk. */
static void Skin_BuildBreath(PcSkinChara* ch, const float* gpos, const float* waist, const float* neck)
{
    PcSkinMorph* m = &ch->morph[PC_SKIN_MORPH_BREATH];
    float span = waist[1] - neck[1], zc = 0.0f;
    int i, n = 0, zn = 0;

    if (span < 1e-3f)
        return;
    for (i = 0; i < ch->vertCount; i++)
    {
        float t = (waist[1] - gpos[i * 3 + 1]) / span;
        if (ch->verts[i].dom == 1 && t > 0.3f && t < 0.9f)
        {
            zc += gpos[i * 3 + 2];
            zn++;
        }
    }
    if (zn == 0)
        return;
    zc /= (float)zn;
    m->v = (u32*)malloc(sizeof(u32) * (size_t)ch->vertCount);
    m->d = (float(*)[3])malloc(sizeof(float[3]) * (size_t)ch->vertCount);
    if (m->v == NULL || m->d == NULL)
    {
        free(m->v);
        free(m->d);
        m->v = NULL;
        m->d = NULL;
        return;
    }
    for (i = 0; i < ch->vertCount; i++)
    {
        const PcSkinVert* sv = &ch->verts[i];
        const float* v = &gpos[i * 3];
        float t = ((waist[1] - v[1]) / span - 0.2f) / 0.8f, w1 = 0.0f, k, dz;
        int q;

        if (t <= 0.0f || t >= 1.0f)
            continue;
        for (q = 0; q < sv->n; q++)
            if (sv->bone[q] == 1)
                w1 = sv->w[q];
        k = sinf(3.14159265f * t);
        k *= k * w1;
        if (k < 1e-3f)
            continue;
        dz = v[2] - zc;
        m->v[n] = (u32)i;
        m->d[n][0] = 0.02f * k * (v[0] - waist[0]);
        m->d[n][1] = -1.0f * k;
        m->d[n][2] = (dz > 0.0f ? 0.07f : 0.02f) * k * dz;
        n++;
    }
    m->count = n;
    ch->breathPeriod = 4.0f + (float)(rand() % 1000) / 1000.0f;
    ch->breathPhase = (float)(rand() % 1000) / 1000.0f;
}

/* A hand with no finger bones: the knuckle row and bend axis from the mesh.
 * The hand points from the wrist to its centroid, the knuckles sit halfway
 * out, and the fingers spread widest across the bend axis. Rigs are bound
 * with the palm facing down or toward the leg, which picks the fold side. */
static int Skin_MeshKnuckles(const PcSkinChara* ch, const float* gpos, int bone, const float* wrist, float* knuckle,
                             float* a, float* c)
{
    float ctr[3] = { 0, 0, 0 }, e1[3], e2[3], len, smax = 0.0f, sxx = 0.0f, sxy = 0.0f, syy = 0.0f, th, q[3];
    int i, f, n = 0;

    for (i = 0; i < ch->vertCount; i++)
        if (ch->verts[i].dom == bone)
        {
            for (f = 0; f < 3; f++)
                ctr[f] += gpos[i * 3 + f];
            n++;
        }
    if (n < 16)
        return 0;
    for (f = 0; f < 3; f++)
        a[f] = ctr[f] / (float)n - wrist[f];
    len = V3_Len(a);
    if (len < 1e-3f)
        return 0;
    for (f = 0; f < 3; f++) a[f] /= len;
    for (i = 0; i < ch->vertCount; i++)
        if (ch->verts[i].dom == bone)
        {
            float t = (gpos[i * 3] - wrist[0]) * a[0] + (gpos[i * 3 + 1] - wrist[1]) * a[1] +
                      (gpos[i * 3 + 2] - wrist[2]) * a[2];
            if (t > smax) smax = t;
        }
    for (f = 0; f < 3; f++)
        knuckle[f] = wrist[f] + a[f] * 0.5f * smax;
    e1[0] = (fabsf(a[0]) < 0.9f) ? 1.0f : 0.0f;
    e1[1] = (fabsf(a[0]) < 0.9f) ? 0.0f : 1.0f;
    e1[2] = 0.0f;
    th = e1[0] * a[0] + e1[1] * a[1];
    for (f = 0; f < 3; f++) e1[f] -= th * a[f];
    len = V3_Len(e1);
    for (f = 0; f < 3; f++) e1[f] /= len;
    e2[0] = a[1] * e1[2] - a[2] * e1[1];
    e2[1] = a[2] * e1[0] - a[0] * e1[2];
    e2[2] = a[0] * e1[1] - a[1] * e1[0];
    for (i = 0; i < ch->vertCount; i++)
        if (ch->verts[i].dom == bone)
        {
            float r[3], x, y;
            for (f = 0; f < 3; f++) r[f] = gpos[i * 3 + f] - knuckle[f];
            if (r[0] * a[0] + r[1] * a[1] + r[2] * a[2] <= 0.0f)
                continue;
            x = r[0] * e1[0] + r[1] * e1[1] + r[2] * e1[2];
            y = r[0] * e2[0] + r[1] * e2[1] + r[2] * e2[2];
            sxx += x * x;
            sxy += x * y;
            syy += y * y;
        }
    th = 0.5f * atan2f(2.0f * sxy, sxx - syy);
    for (f = 0; f < 3; f++)
        c[f] = cosf(th) * e1[f] + sinf(th) * e2[f];
    q[0] = c[1] * a[2] - c[2] * a[1];
    q[1] = c[2] * a[0] - c[0] * a[2];
    q[2] = c[0] * a[1] - c[1] * a[0];
    /* +y is down in game space; the body's midline is x = 0. */
    if (q[1] + (wrist[0] > 0.0f ? -q[0] : q[0]) < 0.0f)
        for (f = 0; f < 3; f++) c[f] = -c[f];
    return 1;
}

static void Skin_BuildGrip(PcSkinChara* ch, const float* gpos, float jointPos[][3], const int* handJoint,
                           int indexJoint[][5], float scale, float offX, float offY, float offZ)
{
    int side;

    for (side = 0; side < 2; side++)
    {
        int bone = side ? 10 : 6, f, i, n = 0;
        float P[6][3], a[3], c[3], q[3], d1[3], d2[3], len, ca, L = 0.0f, kap, rad;
        PcSkinMorph* m = &ch->morph[PC_SKIN_MORPH_GRIP_L + side];

        if (handJoint[side] < 0)
            continue;
        if (indexJoint[side][0] < 0 || indexJoint[side][1] < 0 || indexJoint[side][2] < 0)
        {
            const float* src = jointPos[handJoint[side]];
            P[0][0] = src[0] * scale + offX;
            P[0][1] = -src[1] * scale + offY;
            P[0][2] = src[2] * scale + offZ;
            if (!Skin_MeshKnuckles(ch, gpos, bone, P[0], P[1], a, c))
                continue;
            SH_DBG("[SKIN] game hand %d: no finger bones, curl taken from the mesh", bone);
            goto curl;
        }
        for (f = 0; f < 6; f++)
        {
            int jt = f ? indexJoint[side][f - 1] : handJoint[side];
            const float* src;
            if (jt < 0)
            {
                P[f][0] = P[f][1] = P[f][2] = 0.0f;
                continue;
            }
            src = jointPos[jt];
            P[f][0] = src[0] * scale + offX;
            P[f][1] = -src[1] * scale + offY;
            P[f][2] = src[2] * scale + offZ;
        }
        for (i = 0; i < 3; i++)
        {
            a[i] = P[1][i] - P[0][i];
            d1[i] = P[2][i] - P[1][i];
            d2[i] = P[3][i] - P[2][i];
        }
        len = V3_Len(a);
        if (len < 1e-4f)
            continue;
        for (i = 0; i < 3; i++) a[i] /= len;
        c[0] = d1[1] * d2[2] - d1[2] * d2[1];
        c[1] = d1[2] * d2[0] - d1[0] * d2[2];
        c[2] = d1[0] * d2[1] - d1[1] * d2[0];
        ca = c[0] * a[0] + c[1] * a[1] + c[2] * a[2];
        for (i = 0; i < 3; i++) c[i] -= ca * a[i];
        len = V3_Len(c);
        /* A dead-straight index finger gives no curl direction. The knuckle
         * row still gives the bend axis, and the thumb tip rests on the palm
         * side of it, which picks the way the fingers fold. */
        if (len < 0.05f * V3_Len(d1) * V3_Len(d2))
        {
            float t;
            if (indexJoint[side][3] < 0 || indexJoint[side][4] < 0)
                continue;
            for (i = 0; i < 3; i++)
                c[i] = P[4][i] - P[1][i];
            ca = c[0] * a[0] + c[1] * a[1] + c[2] * a[2];
            for (i = 0; i < 3; i++) c[i] -= ca * a[i];
            len = V3_Len(c);
            if (len < 1e-4f)
                continue;
            for (i = 0; i < 3; i++) c[i] /= len;
            q[0] = c[1] * a[2] - c[2] * a[1];
            q[1] = c[2] * a[0] - c[0] * a[2];
            q[2] = c[0] * a[1] - c[1] * a[0];
            t = (P[5][0] - P[1][0]) * q[0] + (P[5][1] - P[1][1]) * q[1] + (P[5][2] - P[1][2]) * q[2];
            if (t < 0.0f)
                for (i = 0; i < 3; i++) c[i] = -c[i];
            SH_DBG("[SKIN] game hand %d: straight fingers, curl taken from the knuckles and thumb", bone);
        }
        else
            for (i = 0; i < 3; i++) c[i] /= len;
    curl:
        q[0] = c[1] * a[2] - c[2] * a[1];
        q[1] = c[2] * a[0] - c[0] * a[2];
        q[2] = c[0] * a[1] - c[1] * a[0];

        for (i = 0; i < ch->vertCount; i++)
            if (ch->verts[i].dom == bone)
            {
                const float* v = &gpos[i * 3];
                float s = (v[0] - P[1][0]) * a[0] + (v[1] - P[1][1]) * a[1] + (v[2] - P[1][2]) * a[2];
                if (s > 0.0f)
                {
                    n++;
                    if (s > L) L = s;
                }
            }
        if (n == 0 || L < 1e-3f)
            continue;
        kap = PC_SKIN_GRIP_CURL / L;
        rad = 1.0f / kap;
        m->v = (u32*)malloc(sizeof(u32) * (size_t)n);
        m->d = (float(*)[3])malloc(sizeof(float[3]) * (size_t)n);
        if (m->v == NULL || m->d == NULL)
        {
            free(m->v);
            free(m->d);
            m->v = NULL;
            m->d = NULL;
            continue;
        }
        for (i = 0; i < ch->vertCount; i++)
        {
            const float* v = &gpos[i * 3];
            float r[3], s, h, w, t, k;
            if (ch->verts[i].dom != bone)
                continue;
            r[0] = v[0] - P[1][0]; r[1] = v[1] - P[1][1]; r[2] = v[2] - P[1][2];
            s = r[0] * a[0] + r[1] * a[1] + r[2] * a[2];
            if (s <= 0.0f)
                continue;
            h = r[0] * q[0] + r[1] * q[1] + r[2] * q[2];
            w = r[0] * c[0] + r[1] * c[1] + r[2] * c[2];
            t = s * kap;
            k = rad - h;
            m->v[m->count] = (u32)i;
            for (f = 0; f < 3; f++)
                m->d[m->count][f] = P[1][f] + c[f] * w + q[f] * rad + (a[f] * sinf(t) - q[f] * cosf(t)) * k - v[f];
            m->count++;
        }
        SH_DBG("[SKIN] grip for game hand %d: %d vertices past the knuckles", bone, m->count);
    }
}

/* Per-triangle UV wrapping (Skin_Load) can give one skin vertex two UVs in the
 * same material, so output vertices are keyed by the pair, not the vertex. */
static int Skin_BuildCorners(PcSkinMat* mat, int vertCount)
{
    int* first = (int*)malloc(sizeof(int) * (size_t)vertCount);
    int* next = (int*)malloc(sizeof(int) * (size_t)mat->triCount * 3);
    int t, c, k;

    mat->corner = (u32*)malloc(sizeof(u32) * (size_t)mat->triCount * 3);
    mat->src = (u32*)malloc(sizeof(u32) * (size_t)mat->triCount * 3);
    mat->uv = (float(*)[2])malloc(sizeof(float[2]) * (size_t)mat->triCount * 3);
    if (first == NULL || next == NULL || mat->corner == NULL || mat->src == NULL || mat->uv == NULL)
    {
        free(first);
        free(next);
        return 0;
    }
    memset(first, 0xFF, sizeof(int) * (size_t)vertCount);
    mat->srcCount = 0;
    for (t = 0; t < mat->triCount; t++)
        for (c = 0; c < 3; c++)
        {
            const PcSkinTri* tri = &mat->tris[t];
            u32 v = tri->v[c];
            for (k = first[v]; k >= 0; k = next[k])
                if (mat->uv[k][0] == tri->uv[c][0] && mat->uv[k][1] == tri->uv[c][1])
                    break;
            if (k < 0)
            {
                k = mat->srcCount++;
                mat->src[k] = v;
                mat->uv[k][0] = tri->uv[c][0];
                mat->uv[k][1] = tri->uv[c][1];
                next[k] = first[v];
                first[v] = k;
            }
            mat->corner[t * 3 + c] = (u32)k;
        }
    free(first);
    free(next);
    return 1;
}

/* Tangents for the vertices the file gave none, from each triangle's UV
 * gradient. Image V runs down in glTF while a normal map's green points up the
 * image, so the bitangent is the direction of decreasing V. */
static void Skin_FillTangents(PcSkinChara* ch, const float* gpos, const float* gnrm, float* gtan)
{
    float* bit = (float*)calloc((size_t)ch->vertCount * 3, sizeof(float));
    int m, t, c, j;

    if (bit == NULL)
        return;
    for (m = 0; m < ch->matCount; m++)
    {
        const PcSkinMat* mat = &ch->mats[m];
        for (t = 0; t < mat->triCount; t++)
        {
            const PcSkinTri* tri = &mat->tris[t];
            const float* p0 = &gpos[tri->v[0] * 3];
            const float* p1 = &gpos[tri->v[1] * 3];
            const float* p2 = &gpos[tri->v[2] * 3];
            float e1[3], e2[3], du1, dv1, du2, dv2, det, tg[3], bt[3];

            for (c = 0; c < 3; c++)
            {
                e1[c] = p1[c] - p0[c];
                e2[c] = p2[c] - p0[c];
            }
            du1 = tri->uv[1][0] - tri->uv[0][0];
            dv1 = tri->uv[1][1] - tri->uv[0][1];
            du2 = tri->uv[2][0] - tri->uv[0][0];
            dv2 = tri->uv[2][1] - tri->uv[0][1];
            det = du1 * dv2 - du2 * dv1;
            if (fabsf(det) < 1e-12f)
                continue;
            for (c = 0; c < 3; c++)
            {
                tg[c] = (e1[c] * dv2 - e2[c] * dv1) / det;
                bt[c] = -(e2[c] * du1 - e1[c] * du2) / det;
            }
            for (j = 0; j < 3; j++)
            {
                float* g = &gtan[tri->v[j] * 4];
                if (g[3] == 1.0f || g[3] == -1.0f)
                    continue;
                g[3] = 2.0f;
                for (c = 0; c < 3; c++)
                {
                    g[c] += tg[c];
                    bit[tri->v[j] * 3 + c] += bt[c];
                }
            }
        }
    }
    for (j = 0; j < ch->vertCount; j++)
    {
        float* g = &gtan[j * 4];
        const float* n = &gnrm[j * 3];
        const float* b = &bit[j * 3];
        float nt, cx, cy, cz;

        if (g[3] != 2.0f)
            continue;
        nt = g[0] * n[0] + g[1] * n[1] + g[2] * n[2];
        for (c = 0; c < 3; c++)
            g[c] -= n[c] * nt;
        cx = n[1] * g[2] - n[2] * g[1];
        cy = n[2] * g[0] - n[0] * g[2];
        cz = n[0] * g[1] - n[1] * g[0];
        g[3] = (cx * b[0] + cy * b[1] + cz * b[2]) < 0.0f ? -1.0f : 1.0f;
    }
    free(bit);
}

static int Skin_Load(PcSkinChara* ch, const char* path)
{
    cgltf_options opt;
    cgltf_data* data = NULL;
    const cgltf_skin* skin;
    float jointPos[PC_SKIN_MAX_JOINTS][3];
    s8 jointBone[PC_SKIN_MAX_JOINTS];
    float minY = 1e30f, maxY = -1e30f, scale, offX, offY, offZ;
    float hipsPos[3] = { 0, 0, 0 };
    int hipsFound = 0;
    float armY = 0.0f;
    int armCount = 0;
    size_t ni, pi, j;
    int totalVerts = 0;
    float anchor[PC_SKIN_BONES][3];
    int anchorSet[PC_SKIN_BONES];
    float dirSum[PC_SKIN_BONES][3];
    int dirCount[PC_SKIN_BONES];
    float R[PC_SKIN_BONES][9], axis[PC_SKIN_BONES][3], k[PC_SKIN_BONES];
    float foreSwing[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    float* gpos = NULL;
    float* gnrm = NULL;
    float* gtan = NULL;
    int handJoint[2] = { -1, -1 }, indexJoint[2][5] = { { -1, -1, -1, -1, -1 }, { -1, -1, -1, -1, -1 } };
    int b;

    memset(&opt, 0, sizeof(opt));
    opt.file.read = Skin_FileRead;
    opt.file.release = Skin_FileRelease;
    if (cgltf_parse_file(&opt, path, &data) != cgltf_result_success)
        return 0;
    if (cgltf_load_buffers(&opt, data, path) != cgltf_result_success || data->skins_count == 0)
    {
        SH_DBG("[SKIN] %s: buffers missing or no skin", path);
        cgltf_free(data);
        return 0;
    }
    skin = &data->skins[0];
    /* A model built from the game's own parts, posed at the rest frame and in
     * its units, is marked by a node named SH_NATIVE. It covers only some
     * bones (the face and chest), so it is neither fitted to Harry's height
     * nor reweighted, and the stock parts it does not cover keep drawing. */
    ch->native = 0;
    ch->nativeBones = 0;
    for (ni = 0; ni < data->nodes_count; ni++)
    {
        const char* nn = data->nodes[ni].name;
        if (nn == NULL || strncmp(nn, "SH_NATIVE", 9) != 0)
            continue;
        ch->native = 1;
        /* "SH_NATIVE 1 2": the bones it replaces. Shared seam vertices bound
         * to other bones (the shoulders' edge of the chest) do not count. */
        for (nn += 9; *nn != '\0';)
        {
            char* end;
            long b = strtol(nn, &end, 10);
            if (end == nn)
                break;
            if (b >= 0 && b < PC_SKIN_BONES)
                ch->nativeBones |= 1u << b;
            nn = end;
        }
    }
    if (skin->joints_count == 0 || skin->joints_count > PC_SKIN_MAX_JOINTS || skin->inverse_bind_matrices == NULL)
    {
        SH_DBG("[SKIN] %s: skin has %u joints / no inverse bind matrices", path, (unsigned)skin->joints_count);
        cgltf_free(data);
        return 0;
    }

    /* Joints: game bone by name, falling back to the nearest mapped ancestor. */
    for (j = 0; j < skin->joints_count; j++)
    {
        float ibm[16];
        const cgltf_node* n = skin->joints[j];
        int bone = -1;

        cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibm, 16);
        if (!M4_InvTranslation(ibm, jointPos[j]))
            jointPos[j][0] = jointPos[j][1] = jointPos[j][2] = 0.0f;
        for (; n != NULL && bone < 0; n = n->parent)
            bone = Skin_NodeBone(n);
        jointBone[j] = (s8)bone;
        {
            int own = Skin_NodeBone(skin->joints[j]);
            const cgltf_node* par = skin->joints[j]->parent;
            if ((own == 4 || own == 8) && (par == NULL || Skin_NodeBone(par) != own))
            {
                armY += jointPos[j][1];
                armCount++;
            }
        }
        if (!hipsFound && Skin_NodeBone(skin->joints[j]) == 11)
        {
            memcpy(hipsPos, jointPos[j], sizeof(hipsPos));
            hipsFound = 1;
        }
    }
    if (!hipsFound)
    {
        SH_DBG("[SKIN] %s: no Hips joint -- not a humanoid rig this loader knows", path);
        cgltf_free(data);
        return 0;
    }

    /* Count vertices and measure height. */
    for (ni = 0; ni < data->nodes_count; ni++)
    {
        const cgltf_node* node = &data->nodes[ni];
        if (node->mesh == NULL || node->skin != skin)
            continue;
        for (pi = 0; pi < node->mesh->primitives_count; pi++)
        {
            const cgltf_primitive* p = &node->mesh->primitives[pi];
            size_t a;
            for (a = 0; a < p->attributes_count; a++)
                if (p->attributes[a].type == cgltf_attribute_type_position)
                {
                    const cgltf_accessor* acc = p->attributes[a].data;
                    totalVerts += (int)acc->count;
                    if (acc->has_min && acc->has_max)
                    {
                        if (acc->min[1] < minY) minY = acc->min[1];
                        if (acc->max[1] > maxY) maxY = acc->max[1];
                    }
                }
        }
    }
    if (totalVerts == 0 || maxY <= minY || (!ch->native && (armCount == 0 || armY / armCount <= minY)))
    {
        SH_DBG("[SKIN] %s: no skinned geometry", path);
        cgltf_free(data);
        return 0;
    }

    /* glTF (x, y, z) -> PSX model space: Y down, the same mirror the ILM
     * converter's OBJs use; soles on the character's soles, hips over its hips,
     * and sized by SHOULDER height rather than overall height. A modern head
     * and neck are taller than the PSX ones, so height-matching left the arms
     * a few inches low and pinning them raised the shoulders into peaks; at
     * shoulder height the wrists already sit where the game holds weapons. */
    if (ch->native)
    {
        scale = 1.0f;
        offX = offY = offZ = 0.0f;
    }
    else
    {
        scale = (ch->def->soleY - (float)ch->def->rest[4].T[1]) / (armY / armCount - minY);
        offX = -hipsPos[0] * scale;
        offZ = -hipsPos[2] * scale;
        offY = ch->def->soleY + minY * scale;
    }
#define TO_GAME(src, dst) do { (dst)[0] = (src)[0] * scale + offX; (dst)[1] = -(src)[1] * scale + offY; (dst)[2] = (src)[2] * scale + offZ; } while (0)

    ch->verts = (PcSkinVert*)calloc((size_t)totalVerts, sizeof(PcSkinVert));
    gpos = (float*)calloc((size_t)totalVerts * 3, sizeof(float));
    gnrm = (float*)calloc((size_t)totalVerts * 3, sizeof(float));
    gtan = (float*)calloc((size_t)totalVerts * 4, sizeof(float));
    if (ch->verts == NULL || gpos == NULL || gnrm == NULL || gtan == NULL)
        goto fail;

    /* Geometry. */
    {
        int vbase = 0;
        for (ni = 0; ni < data->nodes_count; ni++)
        {
            const cgltf_node* node = &data->nodes[ni];
            if (node->mesh == NULL || node->skin != skin)
                continue;
            for (pi = 0; pi < node->mesh->primitives_count; pi++)
            {
                const cgltf_primitive* p = &node->mesh->primitives[pi];
                const cgltf_accessor *aPos = NULL, *aNrm = NULL, *aTan = NULL, *aUv = NULL, *aJ = NULL, *aW = NULL;
                size_t a, vc, ic, t;
                int mi;
                PcSkinMat* mat;

                if (p->type != cgltf_primitive_type_triangles)
                    continue;
                for (a = 0; a < p->attributes_count; a++)
                {
                    const cgltf_attribute* at = &p->attributes[a];
                    if (at->type == cgltf_attribute_type_position) aPos = at->data;
                    else if (at->type == cgltf_attribute_type_normal) aNrm = at->data;
                    else if (at->type == cgltf_attribute_type_tangent) aTan = at->data;
                    else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) aUv = at->data;
                    else if (at->type == cgltf_attribute_type_joints && at->index == 0) aJ = at->data;
                    else if (at->type == cgltf_attribute_type_weights && at->index == 0) aW = at->data;
                }
                if (aPos == NULL || aJ == NULL || aW == NULL)
                    continue;
                vc = aPos->count;

                /* Material slot. */
                mi = -1;
                for (a = 0; a < (size_t)ch->matCount; a++)
                    if ((const void*)ch->mats[a].out[0][1] == (const void*)p->material)
                        mi = (int)a;
                if (mi < 0)
                {
                    if (ch->matCount >= PC_SKIN_MAX_MATS)
                        continue;
                    mi = ch->matCount++;
                    /* out[0][1] doubles as the material key until frame buffers exist. */
                    ch->mats[mi].out[0][1] = (GrModernVertex*)(void*)p->material;
                    Skin_LoadImage(ch, mi, p->material, path);
                }
                mat = &ch->mats[mi];

                for (t = 0; t < vc; t++)
                {
                    PcSkinVert* sv = &ch->verts[vbase + t];
                    float pos[3], nrm[3] = { 0, 1, 0 }, w[4];
                    cgltf_uint jt[4];
                    int q, r;

                    cgltf_accessor_read_float(aPos, t, pos, 3);
                    if (aNrm != NULL)
                        cgltf_accessor_read_float(aNrm, t, nrm, 3);
                    cgltf_accessor_read_uint(aJ, t, jt, 4);
                    cgltf_accessor_read_float(aW, t, w, 4);
                    TO_GAME(pos, &gpos[(vbase + t) * 3]);
                    gnrm[(vbase + t) * 3 + 0] = nrm[0];
                    gnrm[(vbase + t) * 3 + 1] = -nrm[1];
                    gnrm[(vbase + t) * 3 + 2] = nrm[2];
                    /* The Y flip into game space mirrors the frame, so the
                     * bitangent sign flips with it. */
                    if (aTan != NULL)
                    {
                        float tg[4] = { 0, 0, 0, 1 };
                        cgltf_accessor_read_float(aTan, t, tg, 4);
                        gtan[(vbase + t) * 4 + 0] = tg[0];
                        gtan[(vbase + t) * 4 + 1] = -tg[1];
                        gtan[(vbase + t) * 4 + 2] = tg[2];
                        gtan[(vbase + t) * 4 + 3] = tg[3] < 0.0f ? 1.0f : -1.0f;
                    }
                    sv->n = 0;
                    for (q = 0; q < 4; q++)
                    {
                        int bone = (jt[q] < skin->joints_count) ? jointBone[jt[q]] : -1;
                        if (w[q] <= 0.0f)
                            continue;
                        if (bone < 0)
                            bone = 11;
                        for (r = 0; r < sv->n; r++)
                            if (sv->bone[r] == bone)
                                break;
                        if (r == sv->n)
                        {
                            sv->bone[r] = (u8)bone;
                            sv->w[r] = 0.0f;
                            sv->n++;
                        }
                        sv->w[r] += w[q];
                    }
                    if (sv->n == 0)
                    {
                        sv->n = 1;
                        sv->bone[0] = 11;
                        sv->w[0] = 1.0f;
                    }
                    {
                        float sum = 0.0f;
                        for (r = 0; r < sv->n; r++) sum += sv->w[r];
                        sv->dom = 0;
                        for (r = 0; r < sv->n; r++)
                        {
                            sv->w[r] /= sum;
                            if (sv->w[r] > sv->w[sv->dom]) sv->dom = (u8)r;
                        }
                        sv->dom = sv->bone[sv->dom];
                    }
                }

                ic = p->indices ? p->indices->count : vc;
                mat->tris = (PcSkinTri*)realloc(mat->tris, sizeof(PcSkinTri) * (size_t)(mat->triCount + ic / 3));
                if (mat->tris == NULL)
                    goto fail;
                for (t = 0; t + 2 < ic; t += 3)
                {
                    PcSkinTri* tri = &mat->tris[mat->triCount++];
                    float cu = 0.0f, cv = 0.0f;
                    int c;
                    for (c = 0; c < 3; c++)
                    {
                        size_t idx = p->indices ? cgltf_accessor_read_index(p->indices, t + c) : t + c;
                        tri->v[c] = (u32)(vbase + idx);
                        tri->uv[c][0] = tri->uv[c][1] = 0.0f;
                        if (aUv != NULL)
                            cgltf_accessor_read_float(aUv, idx, tri->uv[c], 2);
                        cu += tri->uv[c][0];
                        cv += tri->uv[c][1];
                    }
                    /* Tiled UVs (the coat at v 2..3) wrap per triangle: the modern
                     * sampler clamps, which turned them into black patches. */
                    cu = floorf(cu / 3.0f);
                    cv = floorf(cv / 3.0f);
                    for (c = 0; c < 3; c++)
                    {
                        tri->uv[c][0] -= cu;
                        tri->uv[c][1] -= cv;
                    }
                }
                Skin_LoadMorphs(ch, node->mesh, p, vbase, scale);
                vbase += (int)vc;
            }
        }
        ch->vertCount = vbase;
    }
    for (b = 0; b < ch->matCount; b++)
    {
        ch->mats[b].out[0][1] = NULL;
        if (!Skin_BuildCorners(&ch->mats[b], ch->vertCount))
            goto fail;
    }

    Skin_FindGrip(skin, jointPos, handJoint, indexJoint);
    Skin_BuildGrip(ch, gpos, jointPos, handJoint, indexJoint, scale, offX, offY, offZ);

    /* Anchors: each bone's root-most mapped joint. */
    memset(anchorSet, 0, sizeof(anchorSet));
    for (j = 0; j < skin->joints_count; j++)
    {
        int bone = Skin_NodeBone(skin->joints[j]);
        const cgltf_node* par = skin->joints[j]->parent;
        int parBone = -1;
        for (; par != NULL && parBone < 0; par = par->parent)
            parBone = Skin_NodeBone(par);
        if (bone >= 0 && parBone != bone && !anchorSet[bone])
        {
            TO_GAME(jointPos[j], anchor[bone]);
            anchorSet[bone] = 1;
        }
    }
    for (b = 1; b < PC_SKIN_BONES; b++)
        if (!anchorSet[b])
        {
            SH_DBG("[SKIN] %s: no joint drives game bone %d", path, b);
            goto fail;
        }

    /* Harry's chest and hips turn independently about one waist point, and a
     * game rig weights the belly almost wholly to the spine above a single
     * blended ring, so every chest lean sheared the torso off the belt. The
     * chest's share now fades in over the lowest part of the torso instead.
     * Weight shared by both thighs (a seat plate or crotch split evenly)
     * landed halfway between two legs that move apart and crumpled, so that
     * share rides the hips. */
    if (!ch->native)
    {
        float y0 = anchor[1][1], span = (anchor[1][1] - anchor[2][1]) * 0.35f;

        for (j = 0; j < (size_t)ch->vertCount; j++)
        {
            PcSkinVert* sv = &ch->verts[j];
            float t = span > 1e-3f ? (y0 - gpos[j * 3 + 1]) / span : 1.0f, toHips = 0.0f;
            int qc = -1, qh = -1, ql = -1, qr = -1, q;

            for (q = 0; q < sv->n; q++)
            {
                if (sv->bone[q] == 1) qc = q;
                if (sv->bone[q] == 11) qh = q;
                if (sv->bone[q] == 12) qr = q;
                if (sv->bone[q] == 15) ql = q;
            }
            if (qc >= 0 && t < 1.0f)
            {
                t = t < 0.0f ? 0.0f : t;
                toHips = sv->w[qc] * (1.0f - t * t * (3.0f - 2.0f * t));
            }
            if ((toHips <= 0.0f && (ql < 0 || qr < 0)) || (qh < 0 && sv->n >= PC_SKIN_INFL))
                continue;
            if (qh < 0)
            {
                qh = sv->n++;
                sv->bone[qh] = 11;
                sv->w[qh] = 0.0f;
            }
            if (qc >= 0)
                sv->w[qc] -= toHips;
            sv->w[qh] += toHips;
            if (ql >= 0 && qr >= 0)
            {
                float shared = sv->w[ql] < sv->w[qr] ? sv->w[ql] : sv->w[qr];
                sv->w[ql] -= shared;
                sv->w[qr] -= shared;
                sv->w[qh] += 2.0f * shared;
            }
            qc = 0;
            for (q = 1; q < sv->n; q++)
                if (sv->w[q] > sv->w[qc])
                    qc = q;
            sv->dom = sv->bone[qc];
        }
    }
    Skin_BuildBreath(ch, gpos, anchor[1], anchor[2]);

    /* Model-side direction of end bones: centroid of the geometry they own. */
    memset(dirSum, 0, sizeof(dirSum));
    memset(dirCount, 0, sizeof(dirCount));
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        int d = ch->verts[j].dom;
        dirSum[d][0] += gpos[j * 3]; dirSum[d][1] += gpos[j * 3 + 1]; dirSum[d][2] += gpos[j * 3 + 2];
        dirCount[d]++;
    }

    /* Retarget: R = swing(rest dir -> model dir) * restR; joint -> model joint. */
    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        const PcSkinRestBone* rb = &ch->def->rest[b];
        float restR[9], swing[9], gd[3], md[3], gEnd[3], mEnd[3], lg, lm;
        int i;

        M3_FromQ12(rb->R, restR);
        k[b] = 1.0f;
        axis[b][0] = 1.0f; axis[b][1] = axis[b][2] = 0.0f;
        if (b == 0)
        {
            memcpy(R[b], restR, sizeof(restR));
            anchor[0][0] = anchor[0][1] = anchor[0][2] = 0.0f;
            continue;
        }
        if (b == 5 || b == 9)
            anchor[b][0] -= anchor[b - 1][0] - ch->def->rest[b - 1].T[0];
        if (s_childA[b] >= 0)
        {
            const PcSkinRestBone* ca = &ch->def->rest[(int)s_childA[b]];
            for (i = 0; i < 3; i++)
            {
                gEnd[i] = ca->T[i];
                mEnd[i] = anchor[(int)s_childA[b]][i];
            }
            if (s_childB[b] >= 0)
                for (i = 0; i < 3; i++)
                {
                    gEnd[i] = 0.5f * (gEnd[i] + ch->def->rest[(int)s_childB[b]].T[i]);
                    mEnd[i] = 0.5f * (mEnd[i] + anchor[(int)s_childB[b]][i]);
                }
        }
        else
        {
            for (i = 0; i < 3; i++)
            {
                gEnd[i] = rb->C[i];
                mEnd[i] = dirCount[b] ? dirSum[b][i] / dirCount[b] : anchor[b][i];
            }
        }
        for (i = 0; i < 3; i++)
        {
            gd[i] = gEnd[i] - rb->T[i];
            md[i] = mEnd[i] - anchor[b][i];
        }
        /* Clavicles keep the model's own slope. Turning a sloped modern
         * clavicle onto Harry's level one rotated the whole shoulder top up
         * about the chest centre, a hump on whichever side the clavicle
         * owns more of the shoulder. Neither end is pinned, so the turn
         * bought nothing. */
        /* The hips keep Harry's frame too: a rig's pelvis joint sits wherever
         * its author put it (level with the thighs on the Halo rig, behind
         * them on GTA's), so the turn onto his thigh midpoint was 41 degrees
         * or noise, and every hip rotation then ran about the wrong axes. */
        /* So do the head and chest. The head's segment runs from the neck
         * joint to its centroid, and Harry's leans well forward of a modern
         * upright head, so the turn tipped every head down. The chest's turn
         * onto his waist-to-neck line set back a chest modelled standing
         * proud (the James reskin's by 5 cm), which bared the neck and left
         * the hip-weighted coat front standing off it. */
        lg = V3_Len(gd);
        lm = V3_Len(md);
        if (b == 1 || b == 2 || b == 3 || b == 7 || b == 11)
            memcpy(R[b], restR, sizeof(restR));
        /* Legs keep the game's rotation unless the model's leg points well
         * away from it. Harry's rest is a step, so his left shin and both
         * feet lean off the model's straight legs, and the swing that
         * closes that gap runs about a tilted axis: it rolled the knee
         * hinge and turned the boot inward. */
        /* Feet turn about the vertical only, so the toe points where
         * Harry's does. A modern boot is modelled straight and his feet
         * splay (20 and 11 degrees), and his standing poses turn the left
         * foot further in: kept as modelled, that toe crossed toward the
         * other foot. */
        else if ((b == 14 || b == 17) && Skin_ToeDir(ch, gpos, b, anchor[b], md))
        {
            gd[0] = ch->def->toe[b == 17][0];
            gd[1] = 0.0f;
            gd[2] = ch->def->toe[b == 17][1];
            M3_Align(gd, md, swing);
            M3_Mul(swing, restR, R[b]);
        }
        else if (b >= 12 && lg > 1e-3f && lm > 1e-3f &&
                 (gd[0] * md[0] + gd[1] * md[1] + gd[2] * md[2]) > 0.866f * lg * lm)
            memcpy(R[b], restR, sizeof(restR));
        /* The empty hand turns with its forearm, so it keeps the wrist the
         * model was built with. Turned onto Harry's own hand, whose centroid
         * sits well off his forearm's line, every open hand came out bent out
         * at the wrist with the palm forward. The gun hand still takes the
         * turn: held weapons are placed in that frame. */
        else if (b == 6)
            M3_Mul(foreSwing, restR, R[b]);
        else
        {
            M3_Align(gd, md, swing);
            M3_Mul(swing, restR, R[b]);
            if (b == 5)
                memcpy(foreSwing, swing, sizeof(foreSwing));
        }
        if (s_scaled[b] && lm > 1e-3f && s_childA[b] >= 0)
        {
            float mdu[3];
            k[b] = lg / lm;
            if (k[b] < 0.5f) k[b] = 0.5f;
            if (k[b] > 2.0f) k[b] = 2.0f;
            for (i = 0; i < 3; i++) mdu[i] = md[i] / lm;
            M3_MulTV(R[b], mdu, axis[b]);
        }
    }

    for (b = 1; b < PC_SKIN_BONES; b++)
    {
        if (!s_pinned[b])
        {
            anchor[b][0] = ch->def->rest[b].T[0];
            anchor[b][1] = ch->def->rest[b].T[1];
            anchor[b][2] = ch->def->rest[b].T[2];
        }
        /* Hips and knees keep the model's own spacing: pulling a knee onto the
         * game's (narrower) knee while the foot stayed put bowed the shins
         * outward. A leg swings about X, so the X offset costs nothing. */
        else if (b == 4 || b == 8 || b == 12 || b == 13 || b == 15 || b == 16)
            anchor[b][0] = ch->def->rest[b].T[0];
    }
    /* Native parts are posed by the rest frame itself, so every bone keeps it
     * and each vertex's bone-local position is the ILM's own. Keyframe
     * rotations are 7-bit, far from orthonormal, so the transpose that undoes
     * a modern rig's rotation would land these vertices half a unit off the
     * stock parts beside them: R holds the inverse's transpose instead, which
     * the bone-local transform below and the shape keys' rt then apply. */
    if (ch->native)
        for (b = 0; b < PC_SKIN_BONES; b++)
        {
            float m[9], det;
            M3_FromQ12(ch->def->rest[b].R, m);
            det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                  m[2] * (m[3] * m[7] - m[4] * m[6]);
            if (det > 1e-6f || det < -1e-6f)
            {
                R[b][0] = (m[4] * m[8] - m[5] * m[7]) / det;
                R[b][1] = (m[5] * m[6] - m[3] * m[8]) / det;
                R[b][2] = (m[3] * m[7] - m[4] * m[6]) / det;
                R[b][3] = (m[2] * m[7] - m[1] * m[8]) / det;
                R[b][4] = (m[0] * m[8] - m[2] * m[6]) / det;
                R[b][5] = (m[1] * m[6] - m[0] * m[7]) / det;
                R[b][6] = (m[1] * m[5] - m[2] * m[4]) / det;
                R[b][7] = (m[2] * m[3] - m[0] * m[5]) / det;
                R[b][8] = (m[0] * m[4] - m[1] * m[3]) / det;
            }
            else
                memcpy(R[b], m, sizeof(m));
            anchor[b][0] = ch->def->rest[b].T[0];
            anchor[b][1] = ch->def->rest[b].T[1];
            anchor[b][2] = ch->def->rest[b].T[2];
            k[b] = 1.0f;
        }

    Skin_FillTangents(ch, gpos, gnrm, gtan);

    /* Bone-local positions per influence, lighting normal per vertex. */
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        PcSkinVert* sv = &ch->verts[j];
        float d[3], nl[3], len;
        int q;

        for (q = 0; q < sv->n; q++)
        {
            int bb = sv->bone[q];
            float l[3], s;
            d[0] = gpos[j * 3] - anchor[bb][0];
            d[1] = gpos[j * 3 + 1] - anchor[bb][1];
            d[2] = gpos[j * 3 + 2] - anchor[bb][2];
            M3_MulTV(R[bb], d, l);
            s = (k[bb] - 1.0f) * (l[0] * axis[bb][0] + l[1] * axis[bb][1] + l[2] * axis[bb][2]);
            sv->l[q][0] = l[0] + s * axis[bb][0];
            sv->l[q][1] = l[1] + s * axis[bb][1];
            sv->l[q][2] = l[2] + s * axis[bb][2];
        }
        M3_MulTV(R[sv->dom], &gnrm[j * 3], nl);
        len = V3_Len(nl);
        if (len < 1e-6f) { nl[0] = 0; nl[1] = -1; nl[2] = 0; len = 1; }
        sv->gteNormal.vx = (s16)(-nl[0] / len * 4064.0f);
        sv->gteNormal.vy = (s16)(-nl[1] / len * 4064.0f);
        sv->gteNormal.vz = (s16)(-nl[2] / len * 4064.0f);
        M3_MulTV(R[sv->dom], &gtan[j * 4], sv->tan);
        sv->tan[3] = gtan[j * 4 + 3];
    }
#undef TO_GAME

    /* Held weapons are modelled round Harry's 10RHAND3 fist (box centre
     * below, hand-bone frame); a shorter hand left the grip hanging below the
     * fingers. The fist measured is the closed one the weapon is shown with. */
    {
        static const float harryFist[3] = { 0.8f, -2.4f, 22.1f };
        const PcSkinMorph* gm = &ch->morph[PC_SKIN_MORPH_GRIP_R];
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        int q = 0, i, n = 0;

        for (j = 0; j < (size_t)ch->vertCount; j++)
        {
            float d[3], l[3], s;
            if (ch->verts[j].dom != 10)
                continue;
            for (i = 0; i < 3; i++)
                d[i] = gpos[j * 3 + i] - anchor[10][i];
            while (q < gm->count && gm->v[q] < (u32)j)
                q++;
            if (q < gm->count && gm->v[q] == (u32)j)
                for (i = 0; i < 3; i++)
                    d[i] += gm->d[q][i];
            M3_MulTV(R[10], d, l);
            s = (k[10] - 1.0f) * (l[0] * axis[10][0] + l[1] * axis[10][1] + l[2] * axis[10][2]);
            for (i = 0; i < 3; i++)
            {
                l[i] += s * axis[10][i];
                if (l[i] < lo[i]) lo[i] = l[i];
                if (l[i] > hi[i]) hi[i] = l[i];
            }
            n++;
        }
        for (i = 0; i < 3; i++)
            ch->gunOff[i] = n ? 0.5f * (lo[i] + hi[i]) - harryFist[i] : 0.0f;
        SH_DBG("[SKIN] right fist offset (%.1f %.1f %.1f)", ch->gunOff[0], ch->gunOff[1], ch->gunOff[2]);
    }

    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        int i;
        for (i = 0; i < 9; i++)
            ch->rt[b][i] = R[b][(i % 3) * 3 + i / 3];
        memcpy(ch->ax[b], axis[b], sizeof(axis[b]));
        ch->kk[b] = k[b];
    }
    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        ch->byBone[b] = (u32*)malloc(sizeof(u32) * (size_t)(dirCount[b] + 1));
        ch->byBoneCount[b] = 0;
    }
    for (j = 0; j < (size_t)ch->vertCount; j++)
    {
        int d = ch->verts[j].dom;
        ch->byBone[d][ch->byBoneCount[d]++] = (u32)j;
    }
    ch->color = (CVECTOR*)calloc((size_t)ch->vertCount, sizeof(CVECTOR));
    ch->pos = (float*)calloc((size_t)ch->vertCount * 3, sizeof(float));
    ch->scr = (float*)calloc((size_t)ch->vertCount * 2, sizeof(float));
    ch->ok = (u8*)calloc((size_t)ch->vertCount, 1);

    SH_DBG("[SKIN] %s: %d verts, %d materials, %u joints, scale %.1f", path, ch->vertCount, ch->matCount,
           (unsigned)skin->joints_count, scale);
    for (b = 1; b < PC_SKIN_BONES; b++)
        SH_DBG("[SKIN]   bone %2d anchor (%.0f %.0f %.0f) k %.2f", b, anchor[b][0], anchor[b][1], anchor[b][2], k[b]);
    free(gpos);
    free(gnrm);
    free(gtan);
    cgltf_free(data);
    return 1;

fail:
    free(gpos);
    free(gnrm);
    free(gtan);
    cgltf_free(data);
    return 0;
}

/* ---------------------------------------------------------------- frame */

static int Skin_ModelPath(const s_LmHeader* lm, char* name, size_t nameSize, char* path, size_t pathSize)
{
    s_CharaModel* models[CHARA_GROUP_COUNT + 1];
    int i;

    models[0] = &g_WorldGfxWork.harryModel;
    for (i = 0; i < CHARA_GROUP_COUNT; i++)
        models[i + 1] = &g_WorldGfxWork.charaModels[i];
    for (i = 0; i <= CHARA_GROUP_COUNT; i++)
    {
        const s_CharaModel* cm = models[i];
        char file[32], folder[16];
        const char* f;
        char* dot;
        size_t len;
        s32 idx;

        if (cm->lmHdr != lm || cm->charaId >= Chara_Count)
            continue;
        idx = CHARA_FILE_INFOS[cm->charaId].modelFileIdx;
        if (idx <= 0 || idx >= FS_FILE_COUNT)
            return 0;
        Fs_GetFileName(file, idx);
        dot = strrchr(file, '.');
        if (dot != NULL)
            *dot = '\0';
        f = g_FilePaths[g_FileTable[idx].pathIdx];
        if (f[0] == '\\')
            f++;
        len = strlen(f);
        while (len > 0 && f[len - 1] == '\\')
            len--;
        if (len >= sizeof(folder))
            len = sizeof(folder) - 1;
        memcpy(folder, f, len);
        folder[len] = '\0';
        snprintf(name, nameSize, "%s", file);
        snprintf(path, pathSize, "gamedata/load/%s/%s", folder, file);
        return 1;
    }
    return 0;
}

/* <NAME>.ini beside the model holds what the mesh cannot say about itself. */
static void Skin_LoadSettings(PcSkinChara* ch, const char* base)
{
    char path[112], line[128];
    FILE* f;

    ch->brightness = 100;
    ch->fogPct = 100;
    ch->beamPct = 100;
    memset(ch->lightOff, 0, sizeof(ch->lightOff));
    ch->gunRaise = ch->gunOut = ch->gunFwd = 0;
    ch->gunTwist = ch->gunPitch = ch->gunYaw = 0;
    ch->handRelax = 0;
    snprintf(path, sizeof(path), "%s.ini", base);
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char key[32];
        int val;
        if (sscanf(line, " %31[A-Za-z_] = %d", key, &val) != 2)
            continue;
        if (strcmp(key, "female") == 0)
            ch->female = val != 0;
        else if (strcmp(key, "brightness") == 0)
            ch->brightness = val < 0 ? 0 : (val > 200 ? 200 : val);
        else if (strcmp(key, "fog") == 0)
            ch->fogPct = val < 0 ? 0 : (val > 400 ? 400 : val);
        else if (strcmp(key, "flashlight") == 0)
            ch->beamPct = val < 0 ? 0 : (val > 100 ? 100 : val);
        else if (strcmp(key, "flashlight_x") == 0)
            ch->lightOff[0] = val;
        else if (strcmp(key, "flashlight_y") == 0)
            ch->lightOff[1] = val;
        else if (strcmp(key, "flashlight_z") == 0)
            ch->lightOff[2] = val;
        else if (strcmp(key, "gun_raise") == 0)
            ch->gunRaise = val;
        else if (strcmp(key, "gun_out") == 0)
            ch->gunOut = val;
        else if (strcmp(key, "gun_forward") == 0)
            ch->gunFwd = val;
        else if (strcmp(key, "gun_twist") == 0)
            ch->gunTwist = val;
        else if (strcmp(key, "gun_pitch") == 0)
            ch->gunPitch = val;
        else if (strcmp(key, "gun_yaw") == 0)
            ch->gunYaw = val;
        else if (strcmp(key, "hand_relax") == 0)
            ch->handRelax = val < 0 ? 0 : (val > 100 ? 100 : val);
    }
    fclose(f);
    SH_DBG("[SKIN] %s: female=%d brightness=%d fog=%d flashlight=%d at (%d %d %d)", path, ch->female,
           ch->brightness, ch->fogPct, ch->beamPct, ch->lightOff[0], ch->lightOff[1], ch->lightOff[2]);
    SH_DBG("[SKIN] %s: gun raise=%d out=%d forward=%d twist=%d pitch=%d yaw=%d hand_relax=%d", path, ch->gunRaise,
           ch->gunOut, ch->gunFwd, ch->gunTwist, ch->gunPitch, ch->gunYaw, ch->handRelax);
}

int Pc_SkinChara_ReloadSettings(void)
{
    int i, n = 0;

    for (i = 0; i < s_charaCount; i++)
        if (s_chara[i].state == 1)
        {
            Skin_LoadSettings(&s_chara[i], s_chara[i].base);
            n++;
        }
    return n;
}

/* The HERO entry Begin would draw: the selected swap when it loaded, else
 * HERO.glb. */
static const PcSkinChara* Skin_CurrentHero(void)
{
    const PcSkinChara* hero = NULL;
    int i;

    for (i = 0; i < s_charaCount; i++)
    {
        if (s_chara[i].state != 1 || strcmp(s_chara[i].name, "HERO") != 0)
            continue;
        if (s_heroFile[0] != '\0' && strcmp(s_chara[i].file, s_heroFile) == 0)
            return &s_chara[i];
        if (strcmp(s_chara[i].file, "HERO") == 0)
            hero = &s_chara[i];
    }
    return hero;
}

int Pc_SkinChara_HeroLightOffset(s32 offQ12[3])
{
    const PcSkinChara* hero = g_PcConfig.allowLooseFiles ? Skin_CurrentHero() : NULL;
    int k;

    if (hero == NULL)
        return 0;
    for (k = 0; k < 3; k++)
        offQ12[k] = hero->lightOff[k] * 4096 / 100;
    return hero->lightOff[0] | hero->lightOff[1] | hero->lightOff[2];
}

int Pc_SkinChara_HeroGunAdjust(MATRIX* adj)
{
    /* The twist turns about the middle of the handgun's grip, so the weapon
     * stays in the fist. In the hand frame +z runs down the forearm and +x
     * away from the body; 2.55 units to the centimetre at Harry's height. */
    static const float pivot[3] = { 3.5f, 4.0f, 29.0f };
    const PcSkinChara* hero = g_PcConfig.allowLooseFiles ? Skin_CurrentHero() : NULL;
    float o[3], r[3][3], rz[3][3], ry[3][3], rx[3][3], t2[3][3];
    float tw, pt, yw;
    int i, k;

    if (hero == NULL)
        return 0;
    tw = (float)hero->gunTwist * 3.14159265f / 180.0f;
    pt = (float)hero->gunPitch * 3.14159265f / 180.0f;
    yw = (float)hero->gunYaw * 3.14159265f / 180.0f;
    memset(rz, 0, sizeof(rz));
    memset(ry, 0, sizeof(ry));
    memset(rx, 0, sizeof(rx));
    rz[0][0] = cosf(tw); rz[0][1] = -sinf(tw);
    rz[1][0] = sinf(tw); rz[1][1] = cosf(tw);
    rz[2][2] = 1.0f;
    ry[0][0] = cosf(yw);  ry[0][2] = sinf(yw);
    ry[1][1] = 1.0f;
    ry[2][0] = -sinf(yw); ry[2][2] = cosf(yw);
    rx[0][0] = 1.0f;
    rx[1][1] = cosf(pt); rx[1][2] = -sinf(pt);
    rx[2][1] = sinf(pt); rx[2][2] = cosf(pt);
    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++)
            t2[i][k] = ry[i][0] * rx[0][k] + ry[i][1] * rx[1][k] + ry[i][2] * rx[2][k];
    for (i = 0; i < 3; i++)
        for (k = 0; k < 3; k++)
            r[i][k] = rz[i][0] * t2[0][k] + rz[i][1] * t2[1][k] + rz[i][2] * t2[2][k];
    o[0] = hero->gunOff[0] + (float)hero->gunOut * 0.255f;
    o[1] = hero->gunOff[1] + (float)hero->gunFwd * 0.255f;
    o[2] = hero->gunOff[2] - (float)hero->gunRaise * 0.255f;
    for (i = 0; i < 3; i++)
    {
        float t = pivot[i] + o[i];
        for (k = 0; k < 3; k++)
        {
            t -= r[i][k] * pivot[k];
            adj->m[i][k] = (short)(r[i][k] * 4096.0f + (r[i][k] < 0.0f ? -0.5f : 0.5f));
        }
        adj->t[i] = (s32)(t + (t < 0.0f ? -0.5f : 0.5f));
    }
    return hero->gunTwist != 0 || hero->gunPitch != 0 || hero->gunYaw != 0 || adj->t[0] != 0 || adj->t[1] != 0 ||
           adj->t[2] != 0;
}

int Pc_SkinChara_HeroFemale(void)
{
    const PcSkinChara* hero = g_PcConfig.allowLooseFiles ? Skin_CurrentHero() : NULL;

    return hero != NULL ? hero->female : 0;
}

/* The output slot for the instance drawn from these bone coords; a new one
 * takes a free slot or the one drawn longest ago. Buffers come on first use. */
static int Skin_PickInstance(PcSkinChara* ch, const void* boneCoords)
{
    int i, slot = -1, m, b;

    ch->instClock++;
    for (i = 0; i < PC_SKIN_INST && slot < 0; i++)
        if (ch->instKey[i] == boneCoords)
            slot = i;
    if (slot < 0)
    {
        slot = 0;
        for (i = 0; i < PC_SKIN_INST; i++)
        {
            if (ch->instKey[i] == NULL)
            {
                slot = i;
                break;
            }
            if (ch->instUse[i] < ch->instUse[slot])
                slot = i;
        }
        ch->instKey[slot] = boneCoords;
        if (slot > 0 && !ch->instLogged)
        {
            ch->instLogged = 1;
            SH_DBG("[SKIN] %s: more than one drawn at once, each keeps its own output", ch->name);
        }
    }
    ch->instUse[slot] = ch->instClock;
    for (m = 0; m < ch->matCount; m++)
        for (b = 0; b < 2; b++)
        {
            PcSkinMat* mat = &ch->mats[m];
            int k;
            if (mat->out[slot][b] != NULL)
                continue;
            mat->out[slot][b] = (GrModernVertex*)calloc((size_t)mat->srcCount, sizeof(GrModernVertex));
            mat->idx[slot][b] = (u32*)malloc(sizeof(u32) * (size_t)mat->triCount * 3);
            if (mat->out[slot][b] == NULL || mat->idx[slot][b] == NULL)
            {
                free(mat->out[slot][b]);
                free(mat->idx[slot][b]);
                mat->out[slot][b] = NULL;
                mat->idx[slot][b] = NULL;
                return 0;
            }
            for (k = 0; k < mat->srcCount; k++)
            {
                GrModernVertex* o = &mat->out[slot][b][k];
                float u = mat->uv[k][0], vv = mat->uv[k][1];
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                if (vv < 0.0f) vv = 0.0f;
                if (vv > 1.0f) vv = 1.0f;
                o->u = u * 256.0f;
                o->v = vv * 256.0f;
                o->a = 255;
            }
        }
    ch->inst = slot;
    return 1;
}

/* The entry for one glTF, loaded on first use. */
static PcSkinChara* Skin_Entry(const char* name, const char* file, const char* base, const PcSkinRestDef* def)
{
    char path[128];
    PcSkinChara* ch = NULL;
    int i;

    for (i = 0; i < s_charaCount && ch == NULL; i++)
        if (strcmp(s_chara[i].file, file) == 0)
            ch = &s_chara[i];
    for (i = 0; ch == NULL && i < s_missingCount; i++)
        if (strcmp(s_missing[i], file) == 0)
            return NULL;
    if (ch == NULL)
    {
        /* Full, a model is refused and the character draws stock -- which
         * play-as swaps did once every stock character looked for a
         * replacement and each miss kept a slot. */
        if (s_charaCount >= PC_SKIN_MAX_CHARA)
        {
            SH_DBG("[SKIN] no room for %s (%d models loaded)", file, s_charaCount);
            return NULL;
        }
        ch = &s_chara[s_charaCount++];
        memset(ch, 0, sizeof(*ch));
        snprintf(ch->name, sizeof(ch->name), "%s", name);
        snprintf(ch->file, sizeof(ch->file), "%s", file);
        snprintf(ch->base, sizeof(ch->base), "%s", base);
        ch->def = def;
    }
    if (ch->state == 0)
    {
        ch->state = -1;
        snprintf(path, sizeof(path), "%s.glb", base);
        if (Skin_Load(ch, path))
            ch->state = 1;
        else
        {
            snprintf(path, sizeof(path), "%s.gltf", base);
            if (Skin_Load(ch, path))
                ch->state = 1;
        }
        if (ch->state == 1)
            Skin_LoadSettings(ch, base);
        else
        {
            /* Only the slot just taken can fail here: hand it back and
             * remember the file instead. */
            if (s_missingCount < PC_SKIN_MAX_MISSING)
                snprintf(s_missing[s_missingCount++], sizeof(s_missing[0]), "%s", file);
            if (ch == &s_chara[s_charaCount - 1])
                s_charaCount--;
            return NULL;
        }
    }
    return ch;
}

/* Character Face Animation's models, read up front: parsing a glTF and its
 * textures the first time a character appears hitched the scene it entered. */
void Pc_SkinChara_PreloadBuiltins(void)
{
    char file[20], path[112];
    int  i;

    if (!g_PcConfig.charaFaceAnim)
        return;
    for (i = 0; i < (int)(sizeof(s_restDefs) / sizeof(s_restDefs[0])); i++)
    {
        snprintf(file, sizeof(file), "@%s", s_restDefs[i].name);
        snprintf(path, sizeof(path), "gamedata/builtin/CHARA/%s", s_restDefs[i].name);
        Skin_Entry(s_restDefs[i].name, file, path, &s_restDefs[i]);
    }
}

void Pc_SkinChara_SetHeroFile(const char* file)
{
    snprintf(s_heroFile, sizeof(s_heroFile), "%s", file != NULL ? file : "");
}

void* Pc_SkinChara_Begin(s_Skeleton* skel, const void* boneCoords)
{
    const s_ModelHeader* hdr;
    const s_LmHeader* lm = NULL;
    char name[16], base[96], path[112];
    PcSkinChara* ch = NULL;
    const PcSkinRestDef* def = NULL;
    int i;

    if ((!g_PcConfig.allowLooseFiles && !g_PcConfig.charaFaceAnim) || skel == NULL || skel->bones_4 == NULL)
        return NULL;
    hdr = skel->bones_4->bone.modelInfo.modelHdr;
    if (hdr == NULL)
        return NULL;
    if (g_WorldGfxWork.harryModel.lmHdr != NULL &&
        hdr >= g_WorldGfxWork.harryModel.lmHdr->modelHdrs &&
        hdr < g_WorldGfxWork.harryModel.lmHdr->modelHdrs + g_WorldGfxWork.harryModel.lmHdr->modelCount)
        lm = g_WorldGfxWork.harryModel.lmHdr;
    for (i = 0; lm == NULL && i < CHARA_GROUP_COUNT; i++)
    {
        const s_LmHeader* c = g_WorldGfxWork.charaModels[i].lmHdr;
        if (c != NULL && hdr >= c->modelHdrs && hdr < c->modelHdrs + c->modelCount)
            lm = c;
    }
    if (lm == NULL || !Skin_ModelPath(lm, name, sizeof(name), base, sizeof(base)))
        return NULL;
    for (i = 0; i < (int)(sizeof(s_restDefs) / sizeof(s_restDefs[0])); i++)
        if (strcmp(name, s_restDefs[i].name) == 0)
            def = &s_restDefs[i];
    if (def == NULL)
        return NULL;

    if (g_PcConfig.allowLooseFiles)
    {
        /* A play-as Harry swap loads its own file; one that is gone or broken
         * falls back to HERO.glb, then to the stock model. */
        if (strcmp(name, "HERO") == 0 && s_heroFile[0] != '\0')
        {
            char dir[96], *slash;
            snprintf(dir, sizeof(dir), "%s", base);
            slash = strrchr(dir, '/');
            if (slash != NULL)
            {
                *slash = '\0';
                snprintf(path, sizeof(path), "%s/%s", dir, s_heroFile);
                ch = Skin_Entry(name, s_heroFile, path, def);
                if (ch != NULL && ch->state != 1)
                    ch = NULL;
            }
        }
        if (ch == NULL)
            ch = Skin_Entry(name, name, base, def);
        if ((ch == NULL || ch->state != 1) && def->alt != NULL)
        {
            char altBase[96], *slash;
            snprintf(altBase, sizeof(altBase), "%s", base);
            slash = strrchr(altBase, '/');
            if (slash != NULL)
            {
                snprintf(slash + 1, sizeof(altBase) - (size_t)(slash + 1 - altBase), "%s", def->alt);
                ch = Skin_Entry(def->alt, def->alt, altBase, def);
            }
        }
    }
    /* No replacement: the game's own parts with a face that blinks and speaks
     * and a chest that breathes (Character Face Animation), from the port's
     * own folder rather than load/, so a mod still wins. */
    if ((ch == NULL || ch->state != 1) && g_PcConfig.charaFaceAnim)
    {
        char file[20];
        snprintf(file, sizeof(file), "@%s", name);
        snprintf(path, sizeof(path), "gamedata/builtin/CHARA/%s", name);
        ch = Skin_Entry(name, file, path, def);
    }
    if (ch == NULL)
        return NULL;
    if (ch->state != 1 || !Skin_PickInstance(ch, boneCoords))
        return NULL;
    memset(ch->have, 0, sizeof(ch->have));
    memset(ch->lit, 0, sizeof(ch->lit));
    ch->isPlayer = (lm == g_WorldGfxWork.harryModel.lmHdr);
    return ch;
}

int Pc_SkinChara_KeepsStockPart(void* chara, const char partName[8], int bone)
{
    const PcSkinChara* ch = (const PcSkinChara*)chara;
    char n[9];
    memcpy(n, partName, 8);
    n[8] = 0;
    /* A native model replaces only the bones it names. */
    if (ch != NULL && ch->native)
        return bone < 0 || bone >= PC_SKIN_BONES ||
               !(ch->nativeBones ? (ch->nativeBones >> bone) & 1 : ch->byBoneCount[bone] != 0);
    return strstr(n, "GUN") != NULL;
}

void Pc_SkinChara_Bone(void* chara, int boneIdx, MATRIX* worldMat, MATRIX* viewMat)
{
    PcSkinChara* ch = (PcSkinChara*)chara;
    s_GteScratchData* sd = PSX_SCRATCH_ADDR(0);
    int i, r, c;

    if (ch == NULL || boneIdx < 0 || boneIdx >= PC_SKIN_BONES || ch->have[boneIdx])
        return;
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
            ch->rv[boneIdx][r * 3 + c] = viewMat->m[r][c] / 4096.0f;
        ch->tv[boneIdx][r] = (float)viewMat->t[r];
    }
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
            ch->rw[boneIdx][r * 3 + c] = worldMat->m[r][c] / 4096.0f;
        ch->tw[boneIdx][r] = (float)worldMat->t[r];
    }
    ch->have[boneIdx] = 1;

    /* The stock part's own lighting prelude (func_80057090 -> func_8005A21C),
     * with the shader owning fog as there. */
    if (worldMat != NULL && g_WorldEnvWork.field_0 != 0)
        func_80057228(worldMat, g_WorldEnvWork.field_54, &g_WorldEnvWork.field_58, &g_WorldEnvWork.field_60);
    switch (g_WorldEnvWork.field_0)
    {
        case 0:
            func_8005A42C(sd, 256 << 4);
            break;
        case 1:
            func_8005A478(sd, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
        case 2:
            func_8005A838(sd, 256 << 4);
            SetColorMatrix(&g_WorldEnvWork.field_2C);
            gte_lddqa(g_WorldEnvWork.field_4C);
            gte_lddqb_0();
            break;
    }
    if (g_WorldEnvWork.field_0 != 0)
    {
        const s32 L[9] = { C2_L11, C2_L12, C2_L13, C2_L21, C2_L22, C2_L23, C2_L31, C2_L32, C2_L33 };
        const s32 LC[9] = { C2_LR1, C2_LR2, C2_LR3, C2_LG1, C2_LG2, C2_LG3, C2_LB1, C2_LB2, C2_LB3 };
        for (i = 0; i < 9; i++)
        {
            ch->gteL[boneIdx][i] = (s16)L[i];
            ch->gteLC[i] = (s16)LC[i];
        }
        ch->gteBK[boneIdx][0] = C2_RBK;
        ch->gteBK[boneIdx][1] = C2_GBK;
        ch->gteBK[boneIdx][2] = C2_BBK;
    }
    if (g_WorldEnvWork.field_0 == 0)
    {
        for (i = 0; i < ch->byBoneCount[boneIdx]; i++)
            ch->color[ch->byBone[boneIdx][i]] = sd->field_3D8;
    }
    else
    {
        /* gte_ncs (sf=1, lm=1) on the registers the prelude above loaded, done
         * natively: one emulated op per vertex cost milliseconds on a dense mesh. */
        const s32 L[9] = { C2_L11, C2_L12, C2_L13, C2_L21, C2_L22, C2_L23, C2_L31, C2_L32, C2_L33 };
        const s32 LC[9] = { C2_LR1, C2_LR2, C2_LR3, C2_LG1, C2_LG2, C2_LG3, C2_LB1, C2_LB2, C2_LB3 };
        const s64 bk[3] = { (s64)C2_RBK << 12, (s64)C2_GBK << 12, (s64)C2_BBK << 12 };

        for (i = 0; i < ch->byBoneCount[boneIdx]; i++)
        {
            u32 v = ch->byBone[boneIdx][i];
            const SVECTOR* n = &ch->verts[v].gteNormal;
            s32 ir[3], rgb[3];
            int k;

            for (k = 0; k < 3; k++)
            {
                s32 mac = (s32)(((s64)L[k * 3] * n->vx + (s64)L[k * 3 + 1] * n->vy + (s64)L[k * 3 + 2] * n->vz) >> 12);
                ir[k] = mac < 0 ? 0 : (mac > 0x7FFF ? 0x7FFF : mac);
            }
            for (k = 0; k < 3; k++)
            {
                s32 mac = (s32)((bk[k] + (s64)LC[k * 3] * ir[0] + (s64)LC[k * 3 + 1] * ir[1] + (s64)LC[k * 3 + 2] * ir[2]) >> 12);
                mac >>= 4;
                rgb[k] = mac < 0 ? 0 : (mac > 255 ? 255 : mac);
            }
            ch->color[v].r = (u8)rgb[0];
            ch->color[v].g = (u8)rgb[1];
            ch->color[v].b = (u8)rgb[2];
            ch->color[v].cd = 0;
        }
    }
    ch->lit[boneIdx] = 1;
}

static u8 Skin_Fog(s32 z)
{
    s32 fa;

    if (!g_WorldEnvWork.isFogEnabled)
        return 0;
    if (z < (1 << g_WorldEnvWork.fog.depthShift))
    {
        s32 idx = (z << 7) >> g_WorldEnvWork.fog.depthShift;
        if (idx < 0) idx = 0;
        if (idx > 127) idx = 127;
        fa = g_WorldEnvWork.fogRamp[idx];
    }
    else
    {
        fa = 255;
    }
    fa = fa * 16 + g_WorldEnvWork.fog.intensity;
    if (fa > 0x1000) fa = 0x1000;
    if (fa < 0) fa = 0;
    return (u8)((fa * 127 + 2048) >> 12);
}

static u8 Skin_Scale8(int v, int pct, int max)
{
    v = v * pct / 100;
    return (u8)(v > max ? max : v);
}

/* ---------------------------------------------------------------- ragdoll
 *
 * Config `ragdoll`: when the player's replacement dies, the pose it died in
 * becomes a particle skeleton -- joints plus hand, foot and head tips, a rigid
 * torso, stiff wrists and ankles -- integrated with Verlet under gravity onto
 * the floor Harry stood on, and every bone is rebuilt from it. It starts with
 * the animation's own motion plus a push along the last hit's knockback. World
 * units, Y down, 256 to the metre. Walls are not collided with. */

enum
{
    RD_WAIST, RD_CHEST, RD_NECK, RD_HEAD,
    RD_SHL, RD_ELL, RD_WRL, RD_HANDL,
    RD_SHR, RD_ELR, RD_WRR, RD_HANDR,
    RD_HIPL, RD_KNL, RD_ANL, RD_TOEL,
    RD_HIPR, RD_KNR, RD_ANR, RD_TOER,
    RD_COUNT
};

#define RD_MAX_STICKS 64
#define RD_GRAVITY    (9.8f * 256.0f)
#define RD_RADIUS     6.0f
/* Fixed step: Verlet velocity is per step, so it must not follow the frame rate. */
#define RD_STEP       (1.0f / 120.0f)

/* Particle -> (bone, tip?) it is read from. */
static const s8 s_rdBone[RD_COUNT] = { 1, 3, 2, 2, 4, 5, 6, 6, 8, 9, 10, 10, 12, 13, 14, 14, 15, 16, 17, 17 };
static const u8 s_rdTip[RD_COUNT]  = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 };

/* Bone -> joint particle, end particle, parent bone (-1: a torso frame). */
static const s8 s_rdJoint[PC_SKIN_BONES] = { -1, RD_WAIST, RD_NECK, RD_CHEST, RD_SHL, RD_ELL, RD_WRL, RD_CHEST, RD_SHR, RD_ELR, RD_WRR,
                                              RD_WAIST, RD_HIPL, RD_KNL, RD_ANL, RD_HIPR, RD_KNR, RD_ANR };
static const s8 s_rdEnd[PC_SKIN_BONES]   = { -1, -1, RD_HEAD, RD_SHL, RD_ELL, RD_WRL, RD_HANDL, RD_SHR, RD_ELR, RD_WRR, RD_HANDR,
                                              -1, RD_KNL, RD_ANL, RD_TOEL, RD_KNR, RD_ANR, RD_TOER };
static const s8 s_rdParent[PC_SKIN_BONES] = { -1, -1, 1, 1, 3, 4, 5, 1, 7, 8, 9, -1, 11, 12, 13, 11, 15, 16 };

typedef struct
{
    int   active;
    float p[RD_COUNT][3], pp[RD_COUNT][3];
    u8    sa[RD_MAX_STICKS], sb[RD_MAX_STICKS];
    float len[RD_MAX_STICKS];
    int   stickCount;
    float floorY;
    float R0[PC_SKIN_BONES][9];
    float torso0[9], hip0[9];
    float dir0[PC_SKIN_BONES][3];
    float anim[RD_COUNT][3];
    float animPrev[RD_COUNT][3];
    float animDt;
    int   haveAnim;
    float hit[3];
    float hitAmount;
    float acc;
    int   minFrom; /* sticks from here on only push apart, down to len */
} PcRagdoll;

static PcRagdoll s_rd;

void Pc_SkinChara_NoteHit(const VECTOR3* knock, const VECTOR3* from, s32 amount)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float x = 0.0f, z = 0.0f;

    if (knock != NULL && (knock->vx != 0 || knock->vz != 0))
    {
        x = (float)knock->vx;
        z = (float)knock->vz;
    }
    else if (from != NULL)
    {
        /* No knockback vector: away from whoever hit him. */
        x = (float)(pl->position.vx - from->vx);
        z = (float)(pl->position.vz - from->vz);
    }
    if (x == 0.0f && z == 0.0f)
        return;
    s_rd.hit[0] = x;
    s_rd.hit[1] = 0.0f;
    s_rd.hit[2] = z;
    s_rd.hitAmount = amount / 4096.0f;
}

static void RD_TipLocal(int bone, float* out)
{
    const PcSkinRestBone* rb = &s_heroRest[bone];
    float R[9], d[3];
    M3_FromQ12(rb->R, R);
    d[0] = rb->C[0] - rb->T[0];
    d[1] = rb->C[1] - rb->T[1];
    d[2] = rb->C[2] - rb->T[2];
    M3_MulTV(R, d, out);
}

static void RD_Points(const PcSkinChara* ch, float out[RD_COUNT][3])
{
    int i;
    for (i = 0; i < RD_COUNT; i++)
    {
        int b = s_rdBone[i];
        if (s_rdTip[i])
        {
            float l[3], w[3];
            RD_TipLocal(b, l);
            M3_MulV(ch->rw[b], l, w);
            out[i][0] = w[0] + ch->tw[b][0];
            out[i][1] = w[1] + ch->tw[b][1];
            out[i][2] = w[2] + ch->tw[b][2];
        }
        else
        {
            out[i][0] = ch->tw[b][0];
            out[i][1] = ch->tw[b][1];
            out[i][2] = ch->tw[b][2];
        }
    }
}

/* Orthonormal frame, columns: a, b made perpendicular to a, a x b. */
static void RD_Frame(const float* from, const float* to, const float* l, const float* r, float* F)
{
    float a[3], b[3], c[3], la, lb, d;
    int i;
    for (i = 0; i < 3; i++) { a[i] = to[i] - from[i]; b[i] = r[i] - l[i]; }
    la = V3_Len(a); if (la < 1e-6f) la = 1;
    for (i = 0; i < 3; i++) a[i] /= la;
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    for (i = 0; i < 3; i++) b[i] -= d * a[i];
    lb = V3_Len(b); if (lb < 1e-6f) lb = 1;
    for (i = 0; i < 3; i++) b[i] /= lb;
    c[0] = a[1] * b[2] - a[2] * b[1];
    c[1] = a[2] * b[0] - a[0] * b[2];
    c[2] = a[0] * b[1] - a[1] * b[0];
    for (i = 0; i < 3; i++) { F[i * 3] = a[i]; F[i * 3 + 1] = b[i]; F[i * 3 + 2] = c[i]; }
}

static void RD_Stick(int a, int b)
{
    if (s_rd.stickCount >= RD_MAX_STICKS)
        return;
    s_rd.sa[s_rd.stickCount] = (u8)a;
    s_rd.sb[s_rd.stickCount] = (u8)b;
    s_rd.len[s_rd.stickCount] = 0.0f;
    s_rd.stickCount++;
}

static void RD_Start(const PcSkinChara* ch)
{
    static const u8 torso[] = { RD_WAIST, RD_CHEST, RD_NECK, RD_SHL, RD_SHR, RD_HIPL, RD_HIPR };
    float vel[3] = { 0, 0, 0 }, speed, hl;
    int i, j, b;

    memset(s_rd.p, 0, sizeof(s_rd.p));
    s_rd.stickCount = 0;
    for (i = 0; i < (int)sizeof(torso); i++)
        for (j = i + 1; j < (int)sizeof(torso); j++)
            RD_Stick(torso[i], torso[j]);
    RD_Stick(RD_NECK, RD_HEAD); RD_Stick(RD_CHEST, RD_HEAD);
    RD_Stick(RD_SHL, RD_ELL); RD_Stick(RD_ELL, RD_WRL); RD_Stick(RD_WRL, RD_HANDL); RD_Stick(RD_ELL, RD_HANDL);
    RD_Stick(RD_SHR, RD_ELR); RD_Stick(RD_ELR, RD_WRR); RD_Stick(RD_WRR, RD_HANDR); RD_Stick(RD_ELR, RD_HANDR);
    RD_Stick(RD_HIPL, RD_KNL); RD_Stick(RD_KNL, RD_ANL); RD_Stick(RD_ANL, RD_TOEL); RD_Stick(RD_KNL, RD_TOEL);
    RD_Stick(RD_HIPR, RD_KNR); RD_Stick(RD_KNR, RD_ANR); RD_Stick(RD_ANR, RD_TOER); RD_Stick(RD_KNR, RD_TOER);
    /* Head held on the shoulders, not just the neck, or it lolls through the chest. */
    RD_Stick(RD_SHL, RD_HEAD); RD_Stick(RD_SHR, RD_HEAD);
    s_rd.minFrom = s_rd.stickCount;
    /* Knees and elbows may bend but not fold flat: the body crumpled into a pile. */
    RD_Stick(RD_HIPL, RD_ANL); RD_Stick(RD_HIPR, RD_ANR);
    RD_Stick(RD_SHL, RD_WRL); RD_Stick(RD_SHR, RD_WRR);

    RD_Points(ch, s_rd.p);
    for (i = 0; i < s_rd.stickCount; i++)
    {
        float d[3];
        for (j = 0; j < 3; j++) d[j] = s_rd.p[s_rd.sb[i]][j] - s_rd.p[s_rd.sa[i]][j];
        s_rd.len[i] = V3_Len(d);
        if (i >= s_rd.minFrom)
            s_rd.len[i] *= 0.6f;
    }
    s_rd.floorY = (float)Q12_TO_Q8(g_SysWork.playerWork.player.position.vy) - RD_RADIUS;

    /* Push along the last hit, harder for heavier hits. It goes into the upper
     * body only, so the planted feet turn it into a topple; a body pushed
     * evenly just dropped where it stood. With no hit on record (a fall, a
     * scripted death) he goes over backwards. */
    hl = sqrtf(s_rd.hit[0] * s_rd.hit[0] + s_rd.hit[2] * s_rd.hit[2]);
    if (hl < 1e-6f)
    {
        q3_12 ry = g_SysWork.playerWork.player.rotation.vy;
        s_rd.hit[0] = -Math_Sin(ry);
        s_rd.hit[2] = -Math_Cos(ry);
        hl = sqrtf(s_rd.hit[0] * s_rd.hit[0] + s_rd.hit[2] * s_rd.hit[2]);
    }
    speed = (2.2f + 3.0f * (s_rd.hitAmount / 30.0f)) * 256.0f;
    if (speed > 5.5f * 256.0f) speed = 5.5f * 256.0f;
    for (i = 0; i < RD_COUNT; i++)
    {
        float w = (i <= RD_HEAD) ? 1.0f : (i == RD_SHL || i == RD_SHR) ? 0.9f :
                  (i == RD_HIPL || i == RD_HIPR) ? 0.35f : (i >= RD_HIPL) ? 0.0f : 0.6f;
        float v[3] = { 0, 0, 0 };
        if (s_rd.haveAnim && s_rd.animDt > 0.0f)
            for (j = 0; j < 3; j++) v[j] = (s_rd.anim[i][j] - s_rd.animPrev[i][j]) / s_rd.animDt;
        if (hl > 1e-6f)
        {
            v[0] += s_rd.hit[0] / hl * speed * w;
            v[2] += s_rd.hit[2] / hl * speed * w;
            v[1] -= 0.8f * 256.0f * w;
        }
        for (j = 0; j < 3; j++)
            s_rd.pp[i][j] = s_rd.p[i][j] - v[j] * RD_STEP;
    }
    (void)vel;

    RD_Frame(s_rd.p[RD_WAIST], s_rd.p[RD_NECK], s_rd.p[RD_SHL], s_rd.p[RD_SHR], s_rd.torso0);
    {
        float mid[3];
        for (j = 0; j < 3; j++) mid[j] = 0.5f * (s_rd.p[RD_HIPL][j] + s_rd.p[RD_HIPR][j]);
        RD_Frame(s_rd.p[RD_WAIST], mid, s_rd.p[RD_HIPL], s_rd.p[RD_HIPR], s_rd.hip0);
    }
    for (b = 0; b < PC_SKIN_BONES; b++)
    {
        memcpy(s_rd.R0[b], ch->rw[b], sizeof(s_rd.R0[b]));
        if (s_rdEnd[b] >= 0)
            for (j = 0; j < 3; j++)
                s_rd.dir0[b][j] = s_rd.p[(int)s_rdEnd[b]][j] - s_rd.p[(int)s_rdJoint[b]][j];
    }
    s_rd.acc = 0.0f;
    s_rd.active = 1;
    /* Spent: a later death without a recorded hit must not reuse this one. */
    s_rd.hit[0] = s_rd.hit[2] = 0.0f;
    s_rd.hitAmount = 0.0f;
}

static void RD_Step(float dt)
{
    int i, j, it;
    for (i = 0; i < RD_COUNT; i++)
    {
        for (j = 0; j < 3; j++)
        {
            float x = s_rd.p[i][j];
            float v = (x - s_rd.pp[i][j]) * 0.995f;
            s_rd.pp[i][j] = x;
            s_rd.p[i][j] = x + v + ((j == 1) ? RD_GRAVITY * dt * dt : 0.0f);
        }
    }
    for (it = 0; it < 10; it++)
    {
        for (i = 0; i < s_rd.stickCount; i++)
        {
            float* a = s_rd.p[s_rd.sa[i]];
            float* b = s_rd.p[s_rd.sb[i]];
            float d[3], l, f;
            for (j = 0; j < 3; j++) d[j] = b[j] - a[j];
            l = V3_Len(d);
            if (l < 1e-6f || (i >= s_rd.minFrom && l >= s_rd.len[i]))
                continue;
            f = 0.5f * (l - s_rd.len[i]) / l;
            for (j = 0; j < 3; j++)
            {
                a[j] += d[j] * f;
                b[j] -= d[j] * f;
            }
        }
        for (i = 0; i < RD_COUNT; i++)
            if (s_rd.p[i][1] > s_rd.floorY)
            {
                s_rd.p[i][1] = s_rd.floorY;
                /* Ground friction: bleed the sliding velocity. */
                s_rd.pp[i][0] += (s_rd.p[i][0] - s_rd.pp[i][0]) * 0.35f;
                s_rd.pp[i][2] += (s_rd.p[i][2] - s_rd.pp[i][2]) * 0.35f;
            }
    }
}

/* Rebuild the bones' world matrices from the particles, then hand them to the
 * skinning as view matrices through this frame's camera (any bone's animated
 * view * inverse(world)). */
static void RD_Pose(PcSkinChara* ch)
{
    float V[9], Vt[3], torso[9], hip[9], F[9], T[9];
    float Rw[PC_SKIN_BONES][9], tw[PC_SKIN_BONES][3], delta[PC_SKIN_BONES][9];
    int b, j, ref = 1;

    /* camera: view = V * world */
    {
        float Rt[9];
        for (j = 0; j < 9; j++) Rt[j] = ch->rw[ref][(j % 3) * 3 + j / 3];
        M3_Mul(ch->rv[ref], Rt, V);
        M3_MulV(V, ch->tw[ref], Vt);
        for (j = 0; j < 3; j++) Vt[j] = ch->tv[ref][j] - Vt[j];
    }

    RD_Frame(s_rd.p[RD_WAIST], s_rd.p[RD_NECK], s_rd.p[RD_SHL], s_rd.p[RD_SHR], F);
    for (j = 0; j < 9; j++) T[j] = s_rd.torso0[(j % 3) * 3 + j / 3];
    M3_Mul(F, T, torso);
    {
        float mid[3];
        for (j = 0; j < 3; j++) mid[j] = 0.5f * (s_rd.p[RD_HIPL][j] + s_rd.p[RD_HIPR][j]);
        RD_Frame(s_rd.p[RD_WAIST], mid, s_rd.p[RD_HIPL], s_rd.p[RD_HIPR], F);
    }
    for (j = 0; j < 9; j++) T[j] = s_rd.hip0[(j % 3) * 3 + j / 3];
    M3_Mul(F, T, hip);

    for (b = 1; b < PC_SKIN_BONES; b++)
    {
        float* d = delta[b];
        if (b == 1)
            memcpy(d, torso, sizeof(torso));
        else if (b == 11)
            memcpy(d, hip, sizeof(hip));
        else
        {
            const float* pd = delta[(int)s_rdParent[b]];
            float carried[3], now[3], sw[9];
            M3_MulV(pd, s_rd.dir0[b], carried);
            for (j = 0; j < 3; j++)
                now[j] = s_rd.p[(int)s_rdEnd[b]][j] - s_rd.p[(int)s_rdJoint[b]][j];
            M3_Align(carried, now, sw);
            M3_Mul(sw, pd, d);
        }
        M3_Mul(d, s_rd.R0[b], Rw[b]);
        for (j = 0; j < 3; j++)
            tw[b][j] = s_rd.p[(int)s_rdJoint[b]][j];
    }
    for (b = 1; b < PC_SKIN_BONES; b++)
    {
        float t[3];
        if (!ch->have[b])
            continue;
        M3_Mul(V, Rw[b], ch->rv[b]);
        M3_MulV(V, tw[b], t);
        for (j = 0; j < 3; j++)
            ch->tv[b][j] = t[j] + Vt[j];
    }
}

static void Skin_Ragdoll(PcSkinChara* ch)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float dt = g_DeltaTime / 4096.0f;
    int i;

    if (!g_PcConfig.ragdoll || ch->native || !ch->isPlayer || !ch->have[1] || !ch->have[11])
    {
        s_rd.active = 0;
        return;
    }
    if (pl->health > Q12(0.0f))
    {
        /* Alive: remember how the pose is moving, for the moment it stops. */
        if (dt > 0.0f)
        {
            memcpy(s_rd.animPrev, s_rd.anim, sizeof(s_rd.anim));
            RD_Points(ch, s_rd.anim);
            s_rd.animDt = dt;
            s_rd.haveAnim = (s_rd.haveAnim < 2) ? s_rd.haveAnim + 1 : 2;
        }
        s_rd.active = 0;
        return;
    }
    if (!s_rd.active)
    {
        if (s_rd.haveAnim < 2)
            s_rd.haveAnim = 0;
        RD_Start(ch);
    }
    if (dt > 1.0f / 15.0f)
        dt = 1.0f / 15.0f;
    s_rd.acc += dt;
    for (i = 0; s_rd.acc >= RD_STEP && i < 16; i++)
    {
        RD_Step(RD_STEP);
        s_rd.acc -= RD_STEP;
    }
    RD_Pose(ch);
}

/* Blink weight: closed in 70 ms, open again over 130 ms, every 2..6 s; shut
 * for good once the ragdoll has him. */
static float Skin_BlinkWeight(PcSkinChara* ch)
{
    float dt = g_DeltaTime / 4096.0f, t;

    if (s_rd.active && ch->isPlayer)
        return 1.0f;
    if (ch->blinkNext <= 0.0f)
        ch->blinkNext = 2.0f + (float)(rand() % 4000) / 1000.0f;
    ch->blinkClock += dt;
    t = ch->blinkClock - ch->blinkNext;
    if (t < 0.0f)
        return 0.0f;
    /* Held shut for a moment: the peak alone lasted no time, so at 30 fps the
     * closest frame landed anywhere short of it and the lids never met. */
    if (t < 0.07f)
        return t / 0.07f;
    if (t < 0.13f)
        return 1.0f;
    if (t < 0.26f)
        return 1.0f - (t - 0.13f) / 0.13f;
    ch->blinkClock = 0.0f;
    ch->blinkNext = 2.0f + (float)(rand() % 4000) / 1000.0f;
    return 0.0f;
}

/* Inhale over the first 40% of a 4-5 s cycle, exhale over the rest; still
 * once the ragdoll has him. */
static float Skin_BreathWeight(PcSkinChara* ch)
{
    float dt = g_DeltaTime / 4096.0f, ph;

    if (s_rd.active && ch->isPlayer)
        return 0.0f;
    ch->breathPhase += dt / ch->breathPeriod;
    ch->breathPhase -= floorf(ch->breathPhase);
    ph = ch->breathPhase < 0.4f ? ch->breathPhase / 0.4f : 1.0f - (ch->breathPhase - 0.4f) / 0.6f;
    return 0.5f - 0.5f * cosf(3.14159265f * ph);
}

/* The mouth follows the voice line's loudness when this character speaks it:
 * by the speaker table, or for an untabled line, when its pitch matches this
 * character's voice. Opening is quick and closing slower, so syllables read
 * as movement rather than a flicker. */
static float Skin_MouthWeight(PcSkinChara* ch)
{
    float dt = g_DeltaTime / 4096.0f, target = 0.0f, rate;
    const char* speaker;
    int female, mine;
    float level = Pc_Lip_Level(&speaker, &female);

    /* A line with no speaker on record falls back to the voice's pitch. Every
     * stock character now has a face, so for them only the player takes that
     * guess; two women in a scene would otherwise both mouth one line. */
    mine = speaker != NULL ? strcmp(speaker, ch->def->voice != NULL ? ch->def->voice : ch->name) == 0
                           : female == ch->def->femaleVoice && (!ch->native || ch->isPlayer);
    if (level > 0.0f && mine && !(s_rd.active && ch->isPlayer))
        target = level;
    rate = target > ch->mouth ? 30.0f : 14.0f;
    ch->mouth += (target - ch->mouth) * (dt * rate > 1.0f ? 1.0f : dt * rate);
    return ch->mouth;
}

/* Fingers close round whatever the game puts in that hand (the phone goes in
 * the left, everything else in the right) and open again when it is put away,
 * a cutscene hides it, or the ragdoll takes over. */
static void Skin_GripWeights(PcSkinChara* ch, float* w)
{
    float dt = g_DeltaTime / 4096.0f;
    int side;

    for (side = 0; side < 2; side++)
    {
        int held = g_WorldGfxWork.heldItem.itemId != NO_VALUE &&
                   g_WorldGfxWork.heldItem.bone.modelInfo.field_0 >= 0 &&
                   (g_WorldGfxWork.heldItem.itemId == InvItemId_CutscenePhone) == (side == 0);
        float target = s_rd.active ? 0.0f : (ch->isPlayer && held) ? 1.0f : (float)ch->handRelax / 100.0f;
        float step = dt / 0.15f;

        if (ch->grip[side] < target)
            ch->grip[side] = (ch->grip[side] + step > target) ? target : ch->grip[side] + step;
        else if (ch->grip[side] > target)
            ch->grip[side] = (ch->grip[side] - step < target) ? target : ch->grip[side] - step;
        w[side] = ch->morph[PC_SKIN_MORPH_GRIP_L + side].count ? ch->grip[side] : 0.0f;
    }
}

/* Shape keys move the bind pose, so each delta goes through the same
 * retarget and bone blend as the vertex it moves. */
static void Skin_ApplyMorphs(PcSkinChara* ch, float h, float ofx, float ofy, float offX, float offY)
{
    float w[PC_SKIN_MORPH_COUNT];
    int k, i;

    w[PC_SKIN_MORPH_BLINK] = ch->morph[PC_SKIN_MORPH_BLINK].count ? Skin_BlinkWeight(ch) : 0.0f;
    w[PC_SKIN_MORPH_MOUTH] = ch->morph[PC_SKIN_MORPH_MOUTH].count ? Skin_MouthWeight(ch) : 0.0f;
    Skin_GripWeights(ch, &w[PC_SKIN_MORPH_GRIP_L]);
    w[PC_SKIN_MORPH_BREATH] = ch->morph[PC_SKIN_MORPH_BREATH].count ? Skin_BreathWeight(ch) : 0.0f;
    for (k = 0; k < PC_SKIN_MORPH_COUNT; k++)
    {
        const PcSkinMorph* m = &ch->morph[k];
        if (w[k] <= 0.0f)
            continue;
        for (i = 0; i < m->count; i++)
        {
            u32 v = m->v[i];
            const PcSkinVert* sv = &ch->verts[v];
            float add[3] = { 0, 0, 0 }, ws = 0.0f, z;
            int q;

            if (!ch->ok[v])
                continue;
            for (q = 0; q < sv->n; q++)
            {
                int b = sv->bone[q];
                float l[3], o[3], sc;
                if (!ch->have[b])
                    continue;
                M3_MulV(ch->rt[b], m->d[i], l);
                sc = (ch->kk[b] - 1.0f) * (l[0] * ch->ax[b][0] + l[1] * ch->ax[b][1] + l[2] * ch->ax[b][2]);
                l[0] += sc * ch->ax[b][0]; l[1] += sc * ch->ax[b][1]; l[2] += sc * ch->ax[b][2];
                M3_MulV(ch->rv[b], l, o);
                add[0] += sv->w[q] * o[0]; add[1] += sv->w[q] * o[1]; add[2] += sv->w[q] * o[2];
                ws += sv->w[q];
            }
            if (ws <= 0.0f)
                continue;
            ch->pos[v * 3]     += w[k] * add[0] / ws;
            ch->pos[v * 3 + 1] += w[k] * add[1] / ws;
            ch->pos[v * 3 + 2] += w[k] * add[2] / ws;
            z = ch->pos[v * 3 + 2];
            if (z < 16.0f)
                continue;
            ch->scr[v * 2]     = ofx + ch->pos[v * 3] * h / z + offX;
            ch->scr[v * 2 + 1] = ofy + ch->pos[v * 3 + 1] * h / z + offY;
        }
    }
}

/* Mirror sign of the view matrices: the tangent frame's handedness follows it. */
static float s_rvSign = 1.0f;

static void Skin_FrameLight(PcSkinChara* ch, int buf)
{
    PcSkinLight* lt = &ch->light[ch->inst][buf];
    const float* rv;
    const float* rw;
    float l[3], d[3], w[3];
    int b, ref = -1, r, c;

    memset(lt, 0, sizeof(*lt));
    /* A native face sits on the stock body, so it is lit the way the body is. */
    if (!(ch->native ? g_PcConfig.perPixelLighting : g_PcConfig.charaPerPixelLight) || !Pc_ModernShader_SkinLit())
        return;
    /* Mode 0 is one flat colour for the whole body; only the hero light has
     * anything to add to it. */
    if (g_WorldEnvWork.field_0 == 0 && g_PsyX_HeroLight[3] <= 0.0f)
        return;
    for (b = 0; b < PC_SKIN_BONES; b++)
        if (ch->have[b] && ch->lit[b] && (ref < 0 || ch->byBoneCount[b] > ch->byBoneCount[ref]))
            ref = b;
    if (ref < 0)
        return;
    rv = ch->rv[ref];
    rw = ch->rw[ref];
    s_rvSign = (rv[0] * (rv[4] * rv[8] - rv[5] * rv[7]) - rv[1] * (rv[3] * rv[8] - rv[5] * rv[6]) +
                rv[2] * (rv[3] * rv[7] - rv[4] * rv[6])) < 0.0f ? -1.0f : 1.0f;
    lt->bright = ch->brightness / 100.0f;
    if (g_WorldEnvWork.field_0 == 0)
    {
        lt->flat = 1;
        lt->on = 1;
        return;
    }
    for (r = 0; r < 3; r++)
    {
        for (c = 0; c < 3; c++)
        {
            l[c] = ch->gteL[ref][r * 3 + c] / 4096.0f;
            lt->LC[r][c] = ch->gteLC[r * 3 + c] / (16.0f * 255.0f);
        }
        M3_MulV(rv, l, lt->L[r]);
        lt->BK[r] = ch->gteBK[ref][r] / (16.0f * 255.0f);
    }
    /* Mode 1 aims row 1 at the point light from each bone's origin
     * (func_8005A478); its length is the distance falloff, kept as is. */
    lt->point = (g_WorldEnvWork.field_0 == 1);
    if (lt->point)
    {
        int k = 0;
        for (r = 1; r < 3; r++)
            if (V3_Len(lt->L[r]) > V3_Len(lt->L[k]))
                k = r;
        /* func_8005A478 writes the point light into row 0 (CR8/CR9) and the
         * shader re-aims row 1; the sum is the same with rows and colour
         * columns swapped together. */
        if (k != 1)
        {
            for (c = 0; c < 3; c++)
            {
                float t = lt->L[k][c];
                lt->L[k][c] = lt->L[1][c];
                lt->L[1][c] = t;
                t = lt->LC[c][k];
                lt->LC[c][k] = lt->LC[c][1];
                lt->LC[c][1] = t;
            }
        }
    }
    lt->l1Mag = V3_Len(lt->L[1]);
    d[0] = g_WorldEnvWork.field_60.vx / 16.0f - ch->tw[ref][0];
    d[1] = g_WorldEnvWork.field_60.vy / 16.0f - ch->tw[ref][1];
    d[2] = g_WorldEnvWork.field_60.vz / 16.0f - ch->tw[ref][2];
    M3_MulTV(rw, d, w);
    M3_MulV(rv, w, lt->lightPos);
    for (c = 0; c < 3; c++)
        lt->lightPos[c] += ch->tv[ref][c];
    lt->bright = ch->brightness / 100.0f;
    lt->on = 1;
    if (!ch->lightLogged && lt->point)
    {
        float toL[3], len, cosA = 0.0f;
        for (c = 0; c < 3; c++)
            toL[c] = lt->lightPos[c] - ch->tv[ref][c];
        len = V3_Len(toL) * lt->l1Mag;
        if (len > 0.0f)
            cosA = (lt->L[1][0] * toL[0] + lt->L[1][1] * toL[1] + lt->L[1][2] * toL[2]) / len;
        ch->lightLogged = 1;
        SH_DBG("[SKIN] %s per-pixel light: ref bone %d, cos(row1, to light) %.2f (expect -1), |row1| %.2f",
               ch->file, ref, cosA, lt->l1Mag);
    }
}

void Pc_SkinChara_End(void* chara, void* otp, int shift, int hideHead)
{
    PcSkinChara* ch = (PcSkinChara*)chara;
    GsOT* ot = (GsOT*)otp;
    float offX = 0.0f, offY = 0.0f, beam;
    float h = (float)C2_H, ofx = (float)C2_OFX / 65536.0f, ofy = (float)C2_OFY / 65536.0f;
    float minZ = 1e30f, maxZ = 0.0f;
    int buf = g_ActiveBufferIdx & 1;
    int v, m, bucket;

    if (ch == NULL || ot == NULL)
        return;
    Skin_Ragdoll(ch);
    beam = 1.0f - ch->beamPct / 100.0f;
    PsyX_GetDrawEnvOffset(&offX, &offY);
    for (v = 0; v < ch->vertCount; v++)
    {
        const PcSkinVert* sv = &ch->verts[v];
        float p[3] = { 0, 0, 0 }, ws = 0.0f;
        int q;

        for (q = 0; q < sv->n; q++)
        {
            int b = sv->bone[q];
            float o[3];
            if (!ch->have[b])
                continue;
            M3_MulV(ch->rv[b], sv->l[q], o);
            p[0] += sv->w[q] * (o[0] + ch->tv[b][0]);
            p[1] += sv->w[q] * (o[1] + ch->tv[b][1]);
            p[2] += sv->w[q] * (o[2] + ch->tv[b][2]);
            ws += sv->w[q];
        }
        ch->ok[v] = 0;
        if (ws < 0.5f)
            continue;
        p[0] /= ws; p[1] /= ws; p[2] /= ws;
        if (p[2] < 16.0f || p[2] > 65535.0f)
            continue;
        ch->pos[v * 3] = p[0]; ch->pos[v * 3 + 1] = p[1]; ch->pos[v * 3 + 2] = p[2];
        ch->scr[v * 2] = ofx + p[0] * h / p[2] + offX;
        ch->scr[v * 2 + 1] = ofy + p[1] * h / p[2] + offY;
        if (!ch->lit[sv->dom])
            ch->color[v].r = ch->color[v].g = ch->color[v].b = 128;
        ch->ok[v] = 1;
        if (p[2] < minZ) minZ = p[2];
        if (p[2] > maxZ) maxZ = p[2];
    }
    Skin_ApplyMorphs(ch, h, ofx, ofy, offX, offY);
    if (maxZ <= 0.0f)
        return;
    Skin_FrameLight(ch, buf);
    PsyX_NoteItemDepthSz((unsigned)maxZ);

    ch->otShift[ch->inst][buf] = shift;
    bucket = PC_SKIN_OT_BASE + ((((int)minZ) >> shift) >> 2);
    if (bucket < 1)
        bucket = 1;
    if (bucket > ORDERING_TABLE_SIZE - 1)
        bucket = ORDERING_TABLE_SIZE - 1;

    for (m = 0; m < ch->matCount; m++)
    {
        PcSkinMat* mat = &ch->mats[m];
        GrModernVertex* out = mat->out[ch->inst][buf];
        u32* idx = mat->idx[ch->inst][buf];
        int t, k, n = 0;

        mat->idxCount[ch->inst][buf] = 0;
        if (out == NULL || idx == NULL)
            continue;
        for (t = 0; t < mat->triCount; t++)
        {
            const PcSkinTri* tri = &mat->tris[t];
            if (!ch->ok[tri->v[0]] || !ch->ok[tri->v[1]] || !ch->ok[tri->v[2]])
                continue;
            if (hideHead && ch->verts[tri->v[0]].dom == 2 && ch->verts[tri->v[1]].dom == 2 &&
                ch->verts[tri->v[2]].dom == 2)
                continue;
            idx[n++] = mat->corner[t * 3];
            idx[n++] = mat->corner[t * 3 + 1];
            idx[n++] = mat->corner[t * 3 + 2];
        }
        mat->idxCount[ch->inst][buf] = n;
        if (n < 3)
            continue;
        for (k = 0; k < mat->srcCount; k++)
        {
            u32 vi = mat->src[k];
            GrModernVertex* o = &out[k];
            float z = ch->pos[vi * 3 + 2];

            if (!ch->ok[vi])
                continue;
            o->x = ch->scr[vi * 2];
            o->y = ch->scr[vi * 2 + 1];
            o->r = Skin_Scale8(ch->color[vi].r, ch->brightness, 255);
            o->g = Skin_Scale8(ch->color[vi].g, ch->brightness, 255);
            o->b = Skin_Scale8(ch->color[vi].b, ch->brightness, 255);
            o->fog = (char)Skin_Scale8(Skin_Fog((s32)z), ch->fogPct, 127);
            o->nz = beam;
            if (ch->light[ch->inst][buf].on)
            {
                const PcSkinVert* sv = &ch->verts[vi];
                const float* r = ch->rv[sv->dom];
                float n[3] = { -sv->gteNormal.vx / 4064.0f, -sv->gteNormal.vy / 4064.0f, -sv->gteNormal.vz / 4064.0f };
                M3_MulV(r, n, &o->snx);
                M3_MulV(r, sv->tan, &o->stx);
                o->stw = sv->tan[3] * s_rvSign;
            }
            o->ppx = o->x;
            o->ppy = o->y;
            o->ppw = z;
            o->vsx = ch->pos[vi * 3];
            o->vsy = ch->pos[vi * 3 + 1];
            o->vsz = z;
        }
        {
            DR_PSYX_MODERN_MESH* packet = (DR_PSYX_MODERN_MESH*)GsOUT_PACKET_P;
            setlen(packet, 2);
            packet->code[0] = 0xB3000000;
            packet->code[1] = PC_SKIN_HANDLE_FLAG | ((u32)(ch - s_chara) << 8) | ((u32)ch->inst << 6) |
                              ((u32)buf << 5) | (u32)m;
            addPrim(&ot->org[bucket], packet);
            GsOUT_PACKET_P = (PACKET*)((unsigned char*)packet + sizeof(*packet));
        }
    }
}

int Pc_SkinChara_PrepareDraw(u32 handle, PcModernDrawBinding* binding)
{
    int ci = (int)((handle >> 8) & 0xFF), inst = (int)((handle >> 6) & 3), buf = (int)((handle >> 5) & 1),
        m = (int)(handle & 0x1F);
    PcSkinChara* ch;
    PcSkinMat* mat;
    GrModernVertex* out;
    float szMax;
    int k;

    if (ci >= s_charaCount)
        return 0;
    ch = &s_chara[ci];
    if (ch->state != 1 || m >= ch->matCount)
        return 0;
    mat = &ch->mats[m];
    out = mat->out[inst][buf];
    if (mat->idxCount[inst][buf] < 3 || out == NULL)
        return 0;
    if (Skin_MapTexture(ch, m, SKIN_MAP_BASE) == 0)
    {
        static const unsigned char white[4] = { 255, 255, 255, 255 };
        if (mat->rgba != NULL || mat->texFrom[SKIN_MAP_BASE] >= 0)
            return 0;
        mat->texture = HiresOverride_CreateTextureRGBA(white, 1, 1);
        if (mat->texture == 0)
            return 0;
    }
    Skin_MapTexture(ch, m, SKIN_MAP_NORMAL);
    Skin_MapTexture(ch, m, SKIN_MAP_ORM);
    Skin_MapTexture(ch, m, SKIN_MAP_EMISSIVE);
    /* Depth is normalized here, at draw time, as the stock prims are. With PGXP
     * off the divisor is the SZ maximum that each GsDrawOt swaps in; read at
     * sort time it came from the frame's last OT -- the 2D one, near zero --
     * and clamped every vertex to the far plane, behind the world. */
    szMax = PGXP_GetSzMax();
    if (szMax < 1.0f)
        szMax = 1.0f;
    if (!g_PsxUsePgxp && g_currentOTBucketCount > 1)
    {
        /* With PGXP off every stock prim, world and character alike, keeps the
         * depth of its OT bucket (the SZ table is wiped before the parse), a
         * ramp over the bucket index rather than SZ / szMax. On the SZ scale
         * the body landed behind props it stands in front of: the cafe pinball
         * cut his torso. Each vertex takes the depth of the bucket a stock prim
         * at its SZ would be in, one bucket nearer, so it wins its own bucket as
         * the later-drawn prim would. */
        float per = 1.0f / (float)(4 << ch->otShift[inst][buf]);
        float step = 2.0f / (float)(g_currentOTBucketCount - 1);
        for (k = 0; k < mat->srcCount; k++)
        {
            float z = 1.0f - (PC_SKIN_OT_BASE - 1 + out[k].vsz * per) * step;
            out[k].z = z < -1.0f ? -1.0f : (z > 1.0f ? 1.0f : z);
        }
    }
    else
        for (k = 0; k < mat->srcCount; k++)
        {
            float z = 1.0f - 2.0f * out[k].vsz / szMax;
            out[k].z = z < -1.0f ? -1.0f : (z > 1.0f ? 1.0f : z);
        }
    Pc_ModernVertex_Upload(out, mat->srcCount);
    Pc_ModernVertex_UploadIndices(mat->idx[inst][buf], mat->idxCount[inst][buf]);
    binding->textureId = mat->texture;
    binding->texFormat = TF_32_BIT_RGBA;
    binding->vertexCount = mat->srcCount;
    binding->indexCount = mat->idxCount[inst][buf];
    binding->nativeWidth = binding->nativeHeight = 256;
    return 1;
}

void Pc_SkinChara_ApplyLighting(u32 handle, unsigned int program)
{
    enum { U_LIT, U_NMAP, U_NTEX, U_POINT, U_L0, U_L1, U_L2, U_L1MAG, U_LPOS, U_LC0, U_LC1, U_LC2, U_BK, U_BRIGHT,
           U_FLAT, U_ORM, U_ORMTEX, U_MAT, U_EMIS, U_EMISON, U_EMISTEX, U_COUNT };
    static const char* const names[U_COUNT] = { "u_skinLit", "u_skinNormalMap", "u_skinNormalTex", "u_skinPoint",
                                                "u_skinL0", "u_skinL1", "u_skinL2", "u_skinL1Mag", "u_skinLightPos",
                                                "u_skinLC0", "u_skinLC1", "u_skinLC2", "u_skinBK", "u_skinBright",
                                                "u_skinFlat", "u_skinOrm", "u_skinOrmTex", "u_skinMat",
                                                "u_skinEmissive", "u_skinEmissiveOn", "u_skinEmissiveTex" };
    static unsigned int s_program;
    static GLint loc[U_COUNT];
    int ci = (int)((handle >> 8) & 0xFF), inst = (int)((handle >> 6) & 3), buf = (int)((handle >> 5) & 1),
        m = (int)(handle & 0x1F);
    const PcSkinChara* ch;
    const PcSkinLight* lt;
    const PcSkinMat* mat;
    int i;

    if (program != s_program)
    {
        s_program = program;
        for (i = 0; i < U_COUNT; i++)
            loc[i] = glGetUniformLocation(program, names[i]);
    }
    if (loc[U_LIT] < 0)
        return;
    if (!(handle & PC_SKIN_HANDLE_FLAG) || ci >= s_charaCount || s_chara[ci].state != 1 ||
        m >= s_chara[ci].matCount)
    {
        glUniform1i(loc[U_LIT], 0);
        glUniform1i(loc[U_EMISON], 0);
        return;
    }
    ch = &s_chara[ci];
    lt = &ch->light[inst][buf];
    mat = &ch->mats[m];
    /* Glow does not depend on the light, so it shows in per-vertex mode too.
     * Units 0-2 are the scene texture, the shadow map and the decode table;
     * 3 to 5 are this material's normal, ORM and emissive maps. */
    glUniform1i(loc[U_EMISON], mat->etexture != 0);
    if (mat->etexture != 0)
    {
        glUniform3fv(loc[U_EMIS], 1, mat->emissive);
        glUniform1i(loc[U_EMISTEX], 5);
        glActiveTexture(GL_TEXTURE5);
        glBindTexture(GL_TEXTURE_2D, mat->etexture);
        glActiveTexture(GL_TEXTURE0);
    }
    if (!lt->on)
    {
        glUniform1i(loc[U_LIT], 0);
        return;
    }
    glUniform1i(loc[U_LIT], 1);
    glUniform1i(loc[U_FLAT], lt->flat);
    glUniform1i(loc[U_POINT], lt->point);
    glUniform3fv(loc[U_L0], 1, lt->L[0]);
    glUniform3fv(loc[U_L1], 1, lt->L[1]);
    glUniform3fv(loc[U_L2], 1, lt->L[2]);
    glUniform1f(loc[U_L1MAG], lt->l1Mag);
    glUniform3fv(loc[U_LPOS], 1, lt->lightPos);
    glUniform3fv(loc[U_LC0], 1, lt->LC[0]);
    glUniform3fv(loc[U_LC1], 1, lt->LC[1]);
    glUniform3fv(loc[U_LC2], 1, lt->LC[2]);
    glUniform3fv(loc[U_BK], 1, lt->BK);
    glUniform1f(loc[U_BRIGHT], lt->bright);
    glUniform1i(loc[U_NMAP], mat->ntexture != 0);
    if (mat->ntexture != 0)
    {
        glUniform1i(loc[U_NTEX], 3);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, mat->ntexture);
        glActiveTexture(GL_TEXTURE0);
    }
    {
        const float pbr[3] = { mat->metal, mat->rough, mat->aoStrength };
        glUniform3fv(loc[U_MAT], 1, pbr);
    }
    glUniform1i(loc[U_ORM], mat->otexture != 0);
    if (mat->otexture != 0)
    {
        glUniform1i(loc[U_ORMTEX], 4);
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, mat->otexture);
        glActiveTexture(GL_TEXTURE0);
    }
}
