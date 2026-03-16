/*
 * Cheryl animation info table for PC port.
 * Extracted from PSX disc at SILENT offset 0x0494D3FC (PSX addr 0x800DF174).
 *
 * 8 entries indexed by packed anim status: (animIdx << 1) | isActive
 *   [0,1] CherylAnim_Still (0)  — still pose
 *   [2,3] CherylAnim_1 (1)      — idle
 *   [4,5] CherylAnim_2 (2)      — walk (footsteps at kf 16, 28)
 *   [6,7] CherylAnim_3 (3)      — run  (footsteps at kf 42, 53)
 */
#include "game.h"
#include "bodyprog/bodyprog.h"

s_AnimInfo CHERYL_ANIM_INFOS[] = {
    /* [0] Still blend  */ { Anim_BlendLinear,  0, 0, 0,        {      0 }, NO_VALUE, 0  },
    /* [1] Still play   */ { Anim_PlaybackLoop, 1, 0, NO_VALUE, { 122880 }, NO_VALUE, 1  },
    /* [2] Idle blend   */ { Anim_BlendLinear,  2, 0, 3,        {  20480 }, NO_VALUE, 0  },
    /* [3] Idle play    */ { Anim_PlaybackLoop, 3, 0, NO_VALUE, {  20480 }, 0,        5  },
    /* [4] Walk blend   */ { Anim_BlendLinear,  4, 0, 5,        {  20480 }, NO_VALUE, 6  },
    /* [5] Walk play    */ { Anim_PlaybackLoop, 5, 0, NO_VALUE, { 122880 }, 6,        32 },
    /* [6] Run blend    */ { Anim_BlendLinear,  6, 0, 7,        {  20480 }, NO_VALUE, 34 },
    /* [7] Run play     */ { Anim_PlaybackLoop, 7, 0, NO_VALUE, { 122880 }, 34,       55 },
};
