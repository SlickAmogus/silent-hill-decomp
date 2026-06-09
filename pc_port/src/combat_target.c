/* func_8005CD38 - target acquisition + vertical aim angle.
 *
 * Original was MIPS asm (INCLUDE_ASM at bodyprog_combat_8005BF38.c:169).
 * Without this, bullets fire horizontally and miss flying enemies (Air
 * Screamer) every shot. Result: zero damage on AS.
 *
 * Implementation: scan g_SysWork.npcs[], find the nearest active enemy
 * within a forward cone, and write its index + vertical pitch back via
 * the out-pointers. The original takes additional shape/cone params we
 * don't fully understand yet (arg3/arg4 are sometimes Q12 distances and
 * sometimes Q12_ANGLE half-cones depending on caller); we use a fixed
 * 45deg half-cone here, which is wide enough for the AS room.
 */
#include <stdio.h>
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "sh_log.h"

void func_8005CD38(s32* outTargetIdx, s32* outVerticalAngle, s_PlayerCombat* combat,
                   s32 arg3, s32 arg4, s32 mode)
{
    (void)combat; (void)arg3; (void)arg4; (void)mode;

    s_SubCharacter* player    = &g_SysWork.playerWork.player;
    VECTOR3*        bulletPos = &g_SysWork.playerCombat.attackPosition;
    q3_12           playerYaw = (q3_12)player->rotation.vy;
    s32             nearestIdx       = NO_VALUE;
    q19_12          nearestHorizDist = 0x7FFFFFFF;

    for (s32 i = 0; i < (s32)ARRAY_SIZE(g_SysWork.npcs); i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        if (npc->model.charaId == Chara_None) continue;
        if (npc->collision.state == 0) continue;
        if (npc->collision.state == 1) continue;
        if (npc->health <= Q12(0.0f)) continue;

        q19_12 dx = (npc->position.vx + npc->collision.shapeOffsets.box.vx) - bulletPos->vx;
        q19_12 dz = (npc->position.vz + npc->collision.shapeOffsets.box.vz) - bulletPos->vz;

        q3_12 npcYaw  = (q3_12)ratan2(dx, dz);
        q3_12 yawDiff = Math_AngleNormalizeSigned(npcYaw - playerYaw);
        if (ABS(yawDiff) > Q12_ANGLE(45.0f)) continue;

        q19_12 horizDist = Math_Vector2MagCalc(dx, dz);
        if (horizDist < nearestHorizDist)
        {
            nearestHorizDist = horizDist;
            nearestIdx       = i;
        }
    }

    if (outTargetIdx) *outTargetIdx = nearestIdx;

    if (outVerticalAngle)
    {
        if (nearestIdx >= 0)
        {
            s_SubCharacter* npc = &g_SysWork.npcs[nearestIdx];
            q19_12 dx = (npc->position.vx + npc->collision.shapeOffsets.box.vx) - bulletPos->vx;
            q19_12 dy = (npc->position.vy + npc->collision.box.offsetY)   - bulletPos->vy;
            q19_12 dz = (npc->position.vz + npc->collision.shapeOffsets.box.vz) - bulletPos->vz;
            q19_12 horizDist = Math_Vector2MagCalc(dx, dz);
            *outVerticalAngle = (s32)ratan2(horizDist, dy);
        }
        else
        {
            *outVerticalAngle = Q12_ANGLE(90.0f);
        }
    }

    SH_DBG("[TGTACQ] arg3=%d arg4=%d mode=%d nearestIdx=%d outAngle=%d (npcs[0] charaId=%d e1=%d hp=%d)",
           (int)arg3, (int)arg4, (int)mode, (int)nearestIdx,
           outVerticalAngle ? (int)*outVerticalAngle : -1,
           (int)g_SysWork.npcs[0].model.charaId,
           (int)g_SysWork.npcs[0].collision.state,
           (int)g_SysWork.npcs[0].health);
}
