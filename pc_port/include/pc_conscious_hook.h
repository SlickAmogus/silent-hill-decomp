/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_hook.h - what the enemy AI files see of the talking-monster
 * feature (silenthill-conscious branch). Include after bodyprog.h.
 */
#ifndef PC_CONSCIOUS_HOOK_H
#define PC_CONSCIOUS_HOOK_H

/* 1 = this monster is listening to Harry (or let him go): hold still and face
 * him instead of running the retail AI. */
int Pc_Conscious_MonsterCalm(s_SubCharacter* chara);

/* Turns toward Harry at degPerSec, the same stepping the retail AI uses. */
static inline void Pc_ConsciousFaceHarry(s_SubCharacter* chara, q19_12 degPerSec)
{
    q3_12 delta = Math_AngleNormalizeSigned(
        ratan2(g_SysWork.playerWork.player.position.vx - chara->position.vx,
               g_SysWork.playerWork.player.position.vz - chara->position.vz) - chara->rotation.vy);
    q3_12 step = Q12_MULT_PRECISE(g_DeltaTime, degPerSec);

    if (ABS(delta) <= step)
    {
        chara->rotation.vy += delta;
    }
    else if (delta > Q12_ANGLE(0.0f))
    {
        chara->rotation.vy += step;
    }
    else
    {
        chara->rotation.vy -= step;
    }
    chara->headingAngle = chara->rotation.vy;
}

#endif
