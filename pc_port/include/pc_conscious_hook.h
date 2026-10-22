/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_hook.h - what the enemy AI files see of the talking-monster
 * feature (silenthill-conscious branch). Include after bodyprog.h.
 */
#ifndef PC_CONSCIOUS_HOOK_H
#define PC_CONSCIOUS_HOOK_H

typedef enum
{
    PcConOrder_Free = 0, /* run the retail AI */
    PcConOrder_Hold,     /* listening (or let him go): stand still, face *foe or Harry */
    PcConOrder_Fight     /* Harry turned it on *foe: go for it, attack while *striking */
} e_PcConOrder;

/* Asked every frame. The AI file decides itself whether it can obey right now
 * (not mid-pounce), and lets go whenever retail changes its state (a hit). */
int Pc_Conscious_MonsterOrder(s_SubCharacter* chara, s_SubCharacter** foe, s32* striking);

/* Turns toward (x, z) at degPerSec, the same stepping the retail AI uses. */
static inline void Pc_ConsciousFace(s_SubCharacter* chara, q19_12 x, q19_12 z, q19_12 degPerSec)
{
    q3_12 delta = Math_AngleNormalizeSigned(ratan2(x - chara->position.vx, z - chara->position.vz) -
                                            chara->rotation.vy);
    q3_12 step  = Q12_MULT_PRECISE(g_DeltaTime, degPerSec);

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

static inline void Pc_ConsciousFaceHarry(s_SubCharacter* chara, q19_12 degPerSec)
{
    Pc_ConsciousFace(chara, g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vz,
                     degPerSec);
}

/* Faces whoever it is listening to or arguing with. */
static inline void Pc_ConsciousFaceOrder(s_SubCharacter* chara, s_SubCharacter* foe, q19_12 degPerSec)
{
    if (foe)
    {
        Pc_ConsciousFace(chara, foe->position.vx, foe->position.vz, degPerSec);
    }
    else
    {
        Pc_ConsciousFaceHarry(chara, degPerSec);
    }
}

static inline q19_12 Pc_ConsciousDist(s_SubCharacter* a, s_SubCharacter* b)
{
    return Math_Vector2MagCalc(b->position.vx - a->position.vx, b->position.vz - a->position.vz);
}

#endif
