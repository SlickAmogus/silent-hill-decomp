#include "bodyprog/bodyprog.h"
#include "bodyprog/events/npc_main.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "main/rng.h"
#include "maps/shared.h"
#include "maps/characters/groaner.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#endif

#define groanerProps groaner->properties.groaner

// @hack Needed to fix mismatch in `sharedFunc_800E39D8_2_s00`.
// Same as version in `rng.h` but without parenthesess around it.
// TODO: Changing `rng.h` version still doesn't let it match though?
#define Rng_GenerateIntFromInput(rand, low, high) \
    (s32)((rand) % (((high) - (low)) + 1)) + (low)

void Groaner_Update(s_SubCharacter* groaner, s_AnmHeader* anmHdr, GsCOORDINATE2* coords)
{
    u8 prevControlState;

#ifdef SH_PC_PORT
    /* The dispatch table sharedData_800EEE14_2_s00 and the per-keyframe
     * data tables are now populated by pc_port/src/groaner_rodata.c
     * (extracted from disc_extract/VIN/MAP2_S00.BIN). AI re-enabled —
     * if a Groaner crash recurs, capture a fresh dump for the next
     * call site we missed. */
    static int s_loggedOn = 0;
    if (!s_loggedOn) {
        s_loggedOn = 1;
    }
#endif

    // Initialize.
    if (groaner->model.controlState == 0)
    {
        Ai_Groaner_Init(groaner);
    }

    prevControlState = groaner->model.controlState;

    if (g_DeltaTime != Q12(0.0f))
    {
        sharedFunc_800E33DC_2_s00(groaner);
        sharedFunc_800E384C_2_s00(groaner);
        sharedFunc_800E5AA4_2_s00(groaner);
    }

    sharedFunc_800E5EC4_2_s00(groaner, anmHdr, coords);

    if (g_DeltaTime != Q12(0.0f))
    {
        sharedFunc_800E6338_2_s00(groaner); // Control func with state machine?
        sharedFunc_800E71E8_2_s00(groaner);

        if (groaner->model.controlState != prevControlState)
        {
            groanerProps.flags.val16[0] &= ~GroanerFlag_8;
        }

        if (groanerProps.flags.val16[0] & GroanerFlag_7)
        {
            groaner->flags |= CharaFlag_Hit;
        }
    }
}

void Ai_Groaner_Init(s_SubCharacter* groaner)
{
    #define HEALTH_BASE      Q12(210.0f)
    #define HEALTH_BONUS_MAX Q12(70.0f)

    s32 i;

    Chara_PropertiesClear(groaner);
    groanerProps.flags.val16[0]  = 0;
    groaner->model.anim.alpha = Q12(0.0f);

    groaner->health = HEALTH_BASE + (Rng_Rand16() % HEALTH_BONUS_MAX);
    if (g_SavegamePtr->gameDifficulty == GameDifficulty_Normal &&
        Rng_GenerateInt(0, 7) == 0) // 1 in 8 chance.
    {
        groaner->health *= 2;
    }

    if (g_SavegamePtr->gameDifficulty == GameDifficulty_Hard &&
        Rng_GenerateInt(0, 3) == 0)// 1 in 4 chance.
    {
        groaner->health *= 2;
    }

    groaner->moveSpeed    = Q12(0.0f);
    groaner->collision.state      = 3;
    groaner->headingAngle = groaner->rotation.vy;

    groanerProps.field_114 = Rng_GenerateUInt(Q12(0.9375f), Q12(1.0625f) - 1);

    if (g_SavegamePtr->gameDifficulty == GameDifficulty_Hard)
    {
        groanerProps.field_114 += (u32)groanerProps.field_114 / 8;
    }

    if (g_SavegamePtr->gameDifficulty == GameDifficulty_Easy)
    {
        groanerProps.field_114 -= ((s32)((u16)groanerProps.field_114 << 16) >> 20); // (value * 0x10000) / 0x100000
    }

    ModelAnim_AnimInfoSet(&groaner->model.anim, GROANER_ANIM_INFOS);
    Chara_DamageClear(groaner);

    if (groaner->model.stateStep == 3)
    {
        groaner->model.controlState = 1;
        Character_AnimSet(groaner, ANIM_STATUS(GroanerAnim_WalkForward, true), 371);
        groanerProps.flags.val16[0] |= GroanerFlag_5;
    }

    groaner->model.stateStep = 0;

    groanerProps.targetPositionX = groaner->position.vx;
    groanerProps.targetPositionZ = groaner->position.vz;

#ifdef MAP2_S00
    if (g_SavegamePtr->gameDifficulty != GameDifficulty_Hard)
    {
        if (!Savegame_EventFlagGet(EventFlag_134))
        {
            groanerProps.field_114 = Q12_MULT_FLOAT_PRECISE(groanerProps.field_114, 0.8f);
        }
        else
        {
            groanerProps.field_114 = Q12_MULT_FLOAT_PRECISE(groanerProps.field_114, 0.9f);
        }
    }
#endif

    #undef HEALTH_BASE
    #undef HEALTH_BONUS_MAX
}

void sharedFunc_800E33DC_2_s00(s_SubCharacter* groaner)
{
    s_CollisionSurface coll;
    q3_12       newHeadingAngle;
    q3_12       unkAngle;
    q19_12      unkDist;
    s32         temp_v0;
    s32         prevControlState;

    if (groaner->health > Q12(0.0f))
    {
        Collision_SurfaceGet(&coll, groaner->position.vx, groaner->position.vz);

        if (coll.groundType == 12 && Rng_GenerateInt(Q12(4.0f), Q12(7.0f) - 1) < groaner->position.vy)
        {
            groaner->damage.amount = Q12(1000.0f);
        }
    }

    if (groaner->damage.amount == Q12(0.0f))
    {
        return;
    }

    if (groaner->health < Q12(0.0f))
    {
        return;
    }

    prevControlState = groaner->model.controlState;

    if (groaner->health > Q12(0.0f))
    {
        groanerProps.field_10F = 4;
    }

    groanerProps.targetPositionX     = g_SysWork.playerWork.player.position.vx;
    groanerProps.targetPositionZ     = g_SysWork.playerWork.player.position.vz;
    groanerProps.flags.val16[0]     |= GroanerFlag_7;

    groaner->health = MAX(groaner->health - groaner->damage.amount, Q12(0.0f));
    unkDist         = Math_Vector2MagCalc(groaner->damage.position.vx, groaner->damage.position.vz);

    // Set new heading angle.
    if (groaner->damage.position.vx != Q12(0.0f) ||
        groaner->damage.position.vz != Q12(0.0f))
    {
        newHeadingAngle = ratan2(groaner->damage.position.vx, groaner->damage.position.vz);
    }
    else
    {
        newHeadingAngle = g_SysWork.playerWork.player.rotation.vy;
    }

    unkAngle = Math_AngleNormalizeSigned((newHeadingAngle - groaner->rotation.vy) + Q12_ANGLE(180.0f));

    // TODO: Unsure if `field_FE` is an angle.
    groanerProps.field_FE = CLAMP_LOW(unkDist >> 2, Q12_ANGLE(90.0f));

    temp_v0 = FP_TO(groaner->damage.position.vy, Q12_SHIFT);

    groaner->headingAngle = newHeadingAngle;

    Chara_DamageClear(groaner);

    groanerProps.flags.val16[0] |= GroanerFlag_1;
    groaner->moveSpeed           = FP_TO(unkDist, Q12_SHIFT) / Q12(1.2f);
    groaner->fallSpeed          += temp_v0 / Q12(1.2f);

    if (groanerProps.flags.val16[0] & GroanerFlag_3)
    {
        groanerProps.field_FE = 0;

        if (groaner->health != Q12(0.0f))
        {
            if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == ANIM_STATUS(GroanerAnim_StandRecoilFront, false))
            {
                if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_StunFromJump, true))
                {
                    groanerProps.relKeyframeIdx_100 = 0;
                }
                else
                {
                    groanerProps.relKeyframeIdx_100 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 78;
                }

                groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StunFromJumpRecoil, false);
            }
            else if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_StunFromStandRight)
            {
                if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_StunFromStandRight, true))
                {
                    groanerProps.relKeyframeIdx_100 = 0;
                }
                else
                {
                    groanerProps.relKeyframeIdx_100 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 165;
                }

                groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StunFromStandRightRecoil, false);
            }
            else if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_StunFromStandLeft)
            {
                if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_StunFromStandLeft, true))
                {
                    groanerProps.relKeyframeIdx_100 = 0;
                }
                else
                {
                    groanerProps.relKeyframeIdx_100 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 319;
                }

                groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StunFromStandLeftRecoil, false);
            }
        }
    }
    else if (groaner->health > Q12(100.0f))
    {
        if (!(groanerProps.flags.val16[0] & GroanerFlag_10))
        {
            if (ABS(unkAngle) < Q12_ANGLE(45.0f))
            {
                if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StandRecoilFront)
                {
                    groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandRecoilFront, false);
                }
            }
            else
            {
                if (unkAngle > Q12_ANGLE(0.0f))
                {
                    if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StandRecoilRight)
                    {
                        groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandRecoilRight, false);
                    }
                }
                else if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StandRecoilLeft)
                {
                    groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandRecoilLeft, false);
                }
            }

            groaner->model.controlState = GroanerControl_StandRecoil;
        }
    }
    else
    {
        Savegame_EnemyStateUpdate(groaner);

        groaner->flags                                      |= CharaFlag_Unk2;
        groanerProps.flags.val16[0]                         |= GroanerFlag_3 | GroanerFlag_12;

        if (ABS(unkAngle) < Q12_ANGLE(45.0f))
        {
            groaner->model.anim.status = ANIM_STATUS(GroanerAnim_JumpToStun, false);
            groaner->model.controlState  = GroanerControl_StunFromJump;
        }
        else if (unkAngle > Q12_ANGLE(0.0f))
        {
            groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandToStunRight, false);
            groaner->model.controlState  = GroanerControl_StunFromStandRight;
        }
        else
        {
            groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandToStunLeft, false);
            groaner->model.controlState  = GroanerControl_StunFromStandLeft;
        }
    }

    if (prevControlState != groaner->model.controlState && prevControlState == GroanerControl_JumpAttack)
    {
        g_SysWork.charaGroupFlags[3] &= ~CharaGroupFlag_1;
    }
}

typedef struct
{
    s32 field_0;
    s32 field_4;
} s_800EEE3C;

extern s_800EEE3C sharedData_800EEE3C_2_s00[];

#if defined(MAP2_S00)
    #define UNK_VAL Q12(0.6f)
#else
    #define UNK_VAL Q12(1.0f)
#endif

void sharedFunc_800E384C_2_s00(s_SubCharacter* groaner)
{
    extern void (*sharedData_800EEE14_2_s00[])(s_SubCharacter* chara);

    /* Dispatch table now populated from disc (groaner_rodata.c). Entry 0
     * is intentionally NULL on PSX — Ai_Groaner_Init handles
     * controlState==0 before this dispatch ever runs. Guard PC anyway
     * so a stray init-state Groaner doesn't NULL-call. */

    #define getIndex() \
        ((((g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 3) == 0) * 2) + ((g_SysWork.field_2388.field_154.effectsInfo_0.field_0.field_0 & 0x3) == 2))

#ifdef SH_PC_PORT
    /* MIPS-style left-to-right argument evaluation is unspecified on
     * x86-64. The PSX source calls getIndex() twice in the func_8006FD90
     * arg list below; cache the result so it's only computed once and
     * we don't depend on evaluation order. */
    s32 _gIdx = getIndex();
    #undef getIndex
    #define getIndex() (_gIdx)
#endif

#ifdef SH_PC_PORT
    /* [GROANER] wake probe: lying dogs ("playing dead") reportedly never
     * wake. Log the two alert gates once per second for the nearest
     * Flag_5 dog so a street log shows which gate fails (hearing =
     * noise*coef - dist via func_80070360; sound query = func_8006FD90). */
    if (groanerProps.flags.val16[0] & GroanerFlag_5)
    {
        static int s_wakeLogTick = 0;
        s32 _dist = Math_Vector2MagCalcSafeQ6(g_SysWork.playerWork.player.position.vx - groaner->position.vx,
                                        g_SysWork.playerWork.player.position.vz - groaner->position.vz);
        if (_dist < Q12(8.0f) && ++s_wakeLogTick >= 60)
        {
            s_wakeLogTick = 0;
            /* dist in Q12 milli-units (dist*1000>>12): nxdk printf drops %f and
             * misaligns the following varargs — integers only on Xbox. */
            SH_DBG("[GROANER] lying dist=%d noise=%d hear=%d losq=%d flags=0x%X anim=%d ctl=%d",
                   (int)(((long long)_dist * 1000) >> 12),
                   (int)g_SysWork.playerWork.player.properties.player.field_10C,
                   (int)func_80070360(groaner, _dist, UNK_VAL),
                   (int)func_8006FD90(groaner, 1, sharedData_800EEE3C_2_s00[getIndex()].field_0, sharedData_800EEE3C_2_s00[getIndex()].field_4),
                   (unsigned)groanerProps.flags.val16[0],
                   (int)groaner->model.anim.status,
                   (int)groaner->model.controlState);
        }
    }
#endif

    if (func_80070360(groaner, Math_Vector2MagCalcSafeQ6(g_SysWork.playerWork.player.position.vx - groaner->position.vx,
            g_SysWork.playerWork.player.position.vz - groaner->position.vz), UNK_VAL) != 0 ||
        func_8006FD90(groaner, 1, sharedData_800EEE3C_2_s00[getIndex()].field_0, sharedData_800EEE3C_2_s00[getIndex()].field_4))
    {
        groanerProps.flags.val16[0] |= GroanerFlag_7;

        if (groanerProps.flags.val16[0] & GroanerFlag_5)
        {
            groanerProps.targetPositionX = g_SysWork.playerWork.player.position.vx;
            groanerProps.targetPositionZ = g_SysWork.playerWork.player.position.vz;
        }
    }
    else
    {
        groanerProps.flags.val16[0] &= ~GroanerFlag_7;
    }

    if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_JumpAttack, true))
    {
        groaner->field_44.field_0 = 0;
    }

    /* Entry 0 is NULL on PSX (the init state, handled by Ai_Groaner_Init
     * before this dispatch). On x86-64 calling NULL is a segfault; on
     * PSX MIPS it's a fast trap that the original execution never
     * actually hits because controlState advances to >=1 in Init. Guard
     * just in case. */
    if (sharedData_800EEE14_2_s00[groaner->model.controlState] != NULL)
    {
        sharedData_800EEE14_2_s00[groaner->model.controlState](groaner);
    }
}

void sharedFunc_800E39D8_2_s00(s_SubCharacter* groaner)
{
    q3_12  unkAngleDelta;
    q19_12 targetPosX;
    q19_12 targetPosZ;
    q19_12 unkRand;
    q19_12 randTargetPosX;
    q19_12 posX;
    q19_12 posZ;

    Chara_MoveSpeedUpdate4(groaner, Q12(6.0f), Q12_MULT_PRECISE(groanerProps.field_114, Q12(0.5f)));

    if (groanerProps.flags.val16[0] & GroanerFlag_0)
    {
        unkAngleDelta = Math_AngleNormalizeSigned(groanerProps.angleToTarget - groaner->rotation.vy);
        if (TIMESTEP_ANGLE(3, 2) < ABS(unkAngleDelta))
        {
            if (unkAngleDelta > Q12_ANGLE(0.0f))
            {
                groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(60.0f));
            }
            else
            {
                groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(60.0f));
            }
        }
        else
        {
            groaner->rotation.vy = groanerProps.angleToTarget;
            groanerProps.flags.val16[0] &= ~GroanerFlag_0;
        }
    }
    else
    {
        if (Los_CharaHitCheck(groaner, Q12_ANGLE(306.0f), groaner->rotation.vy))
        {
            groanerProps.angleToTarget = func_8006F99C(groaner, Q12_ANGLE(306.0f), groaner->rotation.vy);
            if (groanerProps.angleToTarget == Q12_ANGLE(360.0f))
            {
                groanerProps.angleToTarget = -groaner->rotation.vy;
            }

            groanerProps.flags.val16[0] |= GroanerFlag_0;
        }
        else
        {
            unkRand = Rng_GenerateInt(Q12(2.0f), Q12(6.0f) - 1);

            posX       = groaner->position.vx;
            targetPosX = groanerProps.targetPositionX;
            posZ       = groaner->position.vz;
            targetPosZ = groanerProps.targetPositionZ;

            // TODO: Demangle condition.
            if (unkRand < (((posX - targetPosX) > (posZ - targetPosZ)) ? ABS(posX - targetPosX) + ABS((posZ - targetPosZ) >> 1) :
                                                                         ABS((posX - targetPosX) >> 1) + ABS(posZ - targetPosZ)))
            {
                // TODO: Cleaner random angle generation.
                if (ABS(Math_AngleNormalizeSigned(groaner->rotation.vy - ratan2(groaner->position.vx - groanerProps.targetPositionX,
                                                                       groaner->position.vz - groanerProps.targetPositionZ))) < (Rng_GenerateUInt(Q12_ANGLE(45.0f), Q12_ANGLE(50.7f) - 1)))
                {
                    randTargetPosX        = groanerProps.targetPositionX + Rng_GenerateInt(Q12(-0.5f), Q12(0.5f) - 1);
                    groanerProps.angleToTarget = Chara_HeadingAngleGet(groaner, Q12_ANGLE(306.0f),
                                                                  randTargetPosX, groanerProps.targetPositionZ + Rng_GenerateInt(Q12(-0.5f), Q12(0.5f) - 1),
                                                                  Q12_ANGLE(360.0f), true);
                    if (groanerProps.angleToTarget == Q12_ANGLE(360.0f))
                    {
                        groanerProps.angleToTarget = -groaner->rotation.vy;
                    }

                    groanerProps.flags.val16[0] |= GroanerFlag_0;
                }
            }
        }
    }

    if (groaner->model.anim.status == ANIM_STATUS(GroanerAnim_WalkForward, true) &&
        (groanerProps.flags.val32 & (GroanerFlag_5 | GroanerFlag_7)) == (GroanerFlag_5 | GroanerFlag_7))
    {
        groaner->model.controlState = GroanerControl_RunForward;
        groaner->model.anim.status = ANIM_STATUS(GroanerAnim_RunForward, false);
        groanerProps.flags.val16[0] &= ~GroanerFlag_0;
        groanerProps.flags.val16[0] |= GroanerFlag_10;
    }
}

void sharedFunc_800E3E94_2_s00(s_SubCharacter* groaner)
{
    s16           temp_v0_10;
    q3_12         angle0;
    q3_12         angleDeltaToTarget;
    q19_12        headingAngle;
    s32           temp_s2;
    s32           temp_s4;
    q19_12        distToPlayer;
    bool          temp_s6;
    bool          cond;
    s32           var_a1;
    s32           i;
    q19_12        distToPlayerMax;

    if (groanerProps.flags.val16[0] & GroanerFlag_1)
    {
        return;
    }

    if (groanerProps.flags.val16[0] & GroanerFlag_8)
    {
        groanerProps.timer_104          = Q12(0.0f);
        groanerProps.flags.val16[0] |= GroanerFlag_8;
    }

    distToPlayer = Math_Vector2MagCalcSafeQ6(g_SysWork.playerWork.player.position.vx - groaner->position.vx,
                                       g_SysWork.playerWork.player.position.vz - groaner->position.vz);
    distToPlayerMax = Rng_GenerateInt(Q12(4.0f), Q12(8.0f) - 1);
    temp_s6 = Los_NpcToPlayerHitCheck(groaner, &g_SysWork.playerWork.player);

    if (distToPlayer < distToPlayerMax && !temp_s6)
    {
        groanerProps.angleToTarget = Math_AngleBetweenPositionsGet(groaner->position, g_SysWork.playerWork.player.position);
    }
    else
    {
        temp_s2 = Q12(0.9f) - Rng_GenerateInt(0, 920);
        temp_s4 = Los_CharaHitCheck(groaner, temp_s2, groaner->rotation.vy);

        if ((groanerProps.timer_104 >= Q12(0.0f) && ((temp_s4 != 0 && (temp_s6 || distToPlayer < temp_s2)) ||
             groanerProps.timer_104 == Q12(0.0f))) ||
            !Rng_GenerateUInt(0, 31))
        {
            if (temp_s4 != 0)
            {
                var_a1 = temp_s2 + Rng_GenerateInt(0, (temp_s2 >> 2) - 1);
            }
            else
            {
                var_a1 = Rng_GenerateInt(Q12(1.5f), Q12(1.875f) - 1);
            }

            headingAngle = Chara_HeadingAngleGet(groaner, var_a1, groanerProps.targetPositionX, groanerProps.targetPositionZ,
                                                 Q12_ANGLE(360.0f), true);
            if (headingAngle == Q12_ANGLE(360.0f))
            {
                headingAngle = func_8006F99C(groaner, (temp_s2 * 3) >> 2, groaner->rotation.vy);
            }

            if (temp_s4 != 0)
            {
                groanerProps.timer_104 = -(Rng_GenerateInt(Q12(0.375f), Q12(0.75f) - 1));
            }
            else
            {
                groanerProps.timer_104 = Rng_GenerateInt(Q12(0.375f), Q12(1.5f) - 1);
            }

            groanerProps.angleToTarget = headingAngle;
        }
    }

    if (groanerProps.timer_104 > Q12(0.0f))
    {
        groanerProps.timer_104 -= g_DeltaTime;
        if (groanerProps.timer_104 < Q12(0.0f))
        {
            groanerProps.timer_104 = Q12(0.0f);
        }
    }
    else
    {
        groanerProps.timer_104 += g_DeltaTime;
        if (groanerProps.timer_104 > Q12(0.0f))
        {
            groanerProps.timer_104 = Q12(0.0f);
        }
    }

    for (i = 0; i < 3; i++)
    {
        angle0 = Math_AngleNormalizeSigned(groanerProps.angleToTarget - groaner->rotation.vy);

        if (((g_DeltaTime >> 3) + 1) < ABS(angle0))
        {
            if (angle0 > Q12_ANGLE(0.0f))
            {
                groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(90.0f));
            }
            else
            {
                groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(90.0f));
            }
        }
        else
        {
            groaner->rotation.vy = groanerProps.angleToTarget;
        }
    }

    angleDeltaToTarget = Math_AngleNormalizeSigned(groanerProps.angleToTarget - groaner->rotation.vy);
    if (angleDeltaToTarget < Q12_ANGLE(20.0f))
    {
        Chara_MoveSpeedUpdate4(groaner, Q12(9.5f), Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f)));
    }
    else
    {
        temp_v0_10 = Q12(1.0f) - (((angleDeltaToTarget - Q12_ANGLE(20.0f)) * 3) >> 1);
        Chara_MoveSpeedUpdate4(groaner, Q12(9.5f), Q12_MULT_PRECISE(Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f)), temp_v0_10));
    }

    if (distToPlayer > Q12(3.0f))
    {
        cond = false;
    }
    else if (distToPlayer < Q12(1.5f))
    {
        cond = true;
    }
    else
    {
        cond = Rng_TestProbabilityBits(12) > (FP_TO(distToPlayer, Q12_SHIFT) / Q12(3.0f)) >> 2;
    }

    if (cond && !temp_s6)
    {
        if (!Chara_HasFlag(&g_SysWork.playerWork.player, CharaFlag_Unk4) &&
            !(g_SysWork.charaGroupFlags[3] & CharaGroupFlag_1) &&
            g_SysWork.playerWork.player.health > Q12(0.0f) &&
            g_SysWork.sysState == 0 &&
            (groaner->position.vy - g_SysWork.playerWork.player.position.vy) > Q12(-0.8f))
        {
            if (groaner->position.vy - g_SysWork.playerWork.player.position.vy < Q12(0.8f))
            {
                if (ABS(Math_AngleNormalizeSigned(ratan2(g_SysWork.playerWork.player.position.vx - groaner->position.vx,
                                             g_SysWork.playerWork.player.position.vz - groaner->position.vz) -
                                      groaner->rotation.vy)) < Q12_ANGLE(30.0f))
                {
                    g_SysWork.charaGroupFlags[3]       |= CharaGroupFlag_1;
                    groaner->model.controlState  = GroanerControl_JumpAttack;
                    groaner->model.anim.status = ANIM_STATUS(GroanerAnim_StandToJumpAttack, false);
                    return;
                }
            }
        }
    }

    if (distToPlayer < Q12(1.2f))
    {
        if (Chara_HasFlag(&g_SysWork.playerWork.player, CharaFlag_Unk4) ||
            (g_SysWork.charaGroupFlags[3] & CharaGroupFlag_1) ||
            g_SysWork.playerWork.player.health <= Q12(0.0f) ||
            g_SysWork.sysState != 0 ||
            !Rng_GenerateInt(0, 15)) // 1 in 16 chance.
        {
            groaner->model.controlState = GroanerControl_4;
            return;
        }
    }

    if (!(groanerProps.flags.val16[0] & GroanerFlag_7) &&
        (distToPlayer > Q12(12.0f) || (!Rng_GenerateUInt(0, 127) && distToPlayer > Q12(6.0f))))
    {
        groaner->model.controlState                         = GroanerControl_WalkForward;
        groaner->model.anim.status                        = ANIM_STATUS(GroanerAnim_WalkForward, false);
        groanerProps.relKeyframeIdx_100  = 100;
        groanerProps.flags.val16[0] &= ~GroanerFlag_10;
    }
}

void sharedFunc_800E4830_2_s00(s_SubCharacter* groaner)
{
    VECTOR3 sp20;
    q3_12   angleDeltaToPlayer0;
    q3_12   angleDeltaToPlayer1;
    s32     temp_a2;
    q19_12  distToPlayer;
    q19_12  deltaX;
    q19_12  deltaZ;
    q19_12  deltaY;
    s32     temp_v1;
    q19_12  rotMax;
    s32     var_v0;
    s32     temp1;
    s32     temp2;

    // TODO: Use macro.
    deltaX       = Q12_TO_Q6(g_SysWork.playerWork.player.position.vx - groaner->position.vx);
    deltaZ       = Q12_TO_Q6(g_SysWork.playerWork.player.position.vz - groaner->position.vz);
    distToPlayer = Q6_TO_Q12(SquareRoot0(SQUARE(deltaX) + SQUARE(deltaZ)));

    angleDeltaToPlayer0 = Math_AngleNormalizeSigned(Math_AngleBetweenPositionsGet(groaner->position, g_SysWork.playerWork.player.position) -
                                        groaner->rotation.vy);

    if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_StandToJumpAttack ||
        ANIM_TIME_REL_KEYFRAME_IDX_GET(groaner->model.anim.time, 214) < 9u)
    {
        temp_v1 = TIMESTEP_ANGLE(1, 3);

        if ((angleDeltaToPlayer0 >= Q12_ANGLE(0.0f) && temp_v1 <  angleDeltaToPlayer0) ||
            (angleDeltaToPlayer0 <  Q12_ANGLE(0.0f) && temp_v1 < -angleDeltaToPlayer0))
        {
            if (angleDeltaToPlayer0 > Q12_ANGLE(0.0f))
            {
                groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, Q12(0.25f));
            }
            else
            {
                groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, Q12(0.25f));
            }

            angleDeltaToPlayer1 = Math_AngleNormalizeSigned(Math_AngleBetweenPositionsGet(groaner->position, g_SysWork.playerWork.player.position) -
                                                groaner->rotation.vy);

            rotMax = TIMESTEP_ANGLE(1, 3);
            if ((angleDeltaToPlayer1 >= Q12_ANGLE(0.0f) && rotMax <  angleDeltaToPlayer1) ||
                (angleDeltaToPlayer1 <  Q12_ANGLE(0.0f) && rotMax < -angleDeltaToPlayer1))
            {
                if (angleDeltaToPlayer1 > Q12_ANGLE(0.0f))
                {
                    groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, Q12(0.25f));
                }
                else
                {
                    groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, Q12(0.25f));
                }
            }
            else
            {
                groaner->rotation.vy = Math_AngleBetweenPositionsGet(groaner->position, g_SysWork.playerWork.player.position);
            }
        }
        else
        {
            groaner->rotation.vy = Math_AngleBetweenPositionsGet(groaner->position, g_SysWork.playerWork.player.position);
        }
        groaner->headingAngle = groaner->rotation.vy;
    }
    else
    {
        // TODO: Convert 771. Maybe some fraction?
        temp1 = groaner->rotation.vy + Q12(0.125f);
        temp2 = groaner->headingAngle + Q12_MULT_PRECISE(g_DeltaTime, 771);

        if (temp2 < temp1)
        {
            groaner->headingAngle += Q12_MULT_PRECISE(g_DeltaTime, 771);
        }
        else
        {
            groaner->headingAngle = groaner->rotation.vy + Q12(0.125f);
        }
    }

    if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_StandToJumpAttack)
    {
        Chara_MoveSpeedUpdate3(groaner, Q12(9.5f), Q12(0.0f));
    }
    else if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_JumpAttack)
    {
        if (!(groanerProps.flags.val16[0] & GroanerFlag_9))
        {
            groanerProps.flags.val16[0]                         |= GroanerFlag_9;
            var_v0                                                = g_SysWork.playerWork.player.collision.cylinder.field_2 + 573;
            var_v0                                                = (distToPlayer - var_v0) << 1;
            groaner->moveSpeed                                   = MIN(var_v0, Q12(4.5f));

            deltaY                                           = g_SysWork.playerWork.player.position.vy - groaner->position.vy;
            groaner->field_44.field_0                             = 1;
            groanerProps.field_10F                                = 2;
            groaner->fallSpeed                                 = Q12_MULT_PRECISE(deltaY - Q12(1.4f), Q12(2.35f));
        }
    }

    if ((ANIM_TIME_REL_KEYFRAME_IDX_GET(groaner->model.anim.time, 222)) < 2u)
    {
        sp20.vx = groaner->position.vx + groaner->collision.shapeOffsets.cylinder.vx;
        sp20.vy = groaner->position.vy - Q12(0.8f);
        sp20.vz = groaner->position.vz + groaner->collision.shapeOffsets.cylinder.vz;
        func_8008A0E4(1, WEAPON_ATTACK(EquippedWeaponId_HuntingRifle, AttackInputType_Hold), groaner, &sp20, &g_SysWork.playerWork.player, groaner->rotation.vy, Q12_ANGLE(90.0f));
    }

    if ((ANIM_TIME_REL_KEYFRAME_IDX_GET(groaner->model.anim.time, 222)) < 14u)
    {
        groaner->fallSpeed += g_GravitySpeed;

        Chara_MoveSpeedUpdate3(groaner, Q12(0.3f), Q12(0.0f));
    }

    if ((ANIM_TIME_REL_KEYFRAME_IDX_GET(groaner->model.anim.time, 236)) < 7u)
    {
        Chara_MoveSpeedUpdate3(groaner, Q12(1.2f), Q12(0.0f));
    }

    if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) == GroanerAnim_RunForward)
    {
        groaner->model.controlState                         = GroanerControl_4;
        groaner->rotation.vy                                += Q12(0.125f);
        groanerProps.targetHeadingAngle += Q12(0.125f);
        g_SysWork.charaGroupFlags[3]                              &= ~CharaGroupFlag_1;

        Character_AnimSet(groaner, ANIM_STATUS(GroanerAnim_RunForward, true), 363);
        groanerProps.flags.val16[0]                         &= ~GroanerFlag_9;
    }
}

void sharedFunc_800E4E84_2_s00(s_SubCharacter* groaner)
{
    s32    temp_a3;
    s32    temp_s1;
    s32    temp_s4;
    q19_12 distToPlayer;
    q19_12 var_s0_2;
    s32    i;

    if (groanerProps.flags.val16[0] & GroanerFlag_1)
    {
        return;
    }

    if (!(groanerProps.flags.val16[0] & GroanerFlag_8))
    {
        groanerProps.field_108          = 0;
        groanerProps.timer_104          = Q12(0.0f);
        groanerProps.flags.val16[0] |= GroanerFlag_8;
    }

    // TODO: Cleaner random angle generation.
    temp_s4 = Los_CharaHitCheck(groaner, Q12(0.9f) - Rng_GenerateInt(0, 920), groaner->rotation.vy);

    if (groanerProps.timer_104 == Q12(0.0f) || !Rng_GenerateUInt(0, 31) ||
        (temp_s4 != 0 && groanerProps.timer_104 >= Q12(0.0f)))
    {
        temp_s1 = Q12(1.5f) - Rng_GenerateInt(0, 1535);
        groanerProps.angleToTarget = Chara_HeadingAngleGet(groaner, temp_s1, groanerProps.targetPositionX,
                                                                     groanerProps.targetPositionZ, Q12(1.0f), false);
        if (groanerProps.angleToTarget == Q12_ANGLE(360.0f))
        {
            groanerProps.angleToTarget = func_8006F99C(groaner, (temp_s1 * 3) >> 2, groaner->rotation.vy);
        }

        if (temp_s4 != 0)
        {
            groanerProps.timer_104 = -(Rng_GenerateInt(Q12(0.375), Q12(0.75f) - 1));
        }
        else
        {
            groanerProps.timer_104 = Rng_GenerateInt(Q12(0.75f), Q12(1.5f) - 1);
        }
    }

    if (groanerProps.timer_104 > Q12(0.0f))
    {
        groanerProps.timer_104 -= g_DeltaTime;
        if (groanerProps.timer_104 < Q12(0.0f))
        {
            groanerProps.timer_104 = Q12(0.0f);
        }
    }
    else
    {
        groanerProps.timer_104 += g_DeltaTime;
        if (groanerProps.timer_104 > Q12(0.0f))
        {
            groanerProps.timer_104 = Q12(0.0f);
        }
    }

    for (i = 0; i < 2; i++)
    {
        temp_a3 = Math_AngleNormalizeSigned((groanerProps.angleToTarget - groaner->rotation.vy));
        if (((g_DeltaTime >> 3) + 1) < ABS(temp_a3))
        {
            if (temp_a3 > Q12_ANGLE(0.0f))
            {
                groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(90.0f));
            }
            else
            {
                groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(90.0f));
            }
        }
        else
        {
            groaner->rotation.vy = groanerProps.angleToTarget;
        }
    }

    temp_a3 = Math_AngleNormalizeSigned(groanerProps.angleToTarget - groaner->rotation.vy);

    if (groanerProps.timer_104 >= Q12(0.0f))
    {
        Chara_MoveSpeedUpdate4(groaner, Q12(9.5f), Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f)));
    }
    else
    {
        temp_a3 = Q12(1.0f) - (((temp_a3 - 227) * 3) >> 1);
        Chara_MoveSpeedUpdate4(groaner, Q12(9.5f), Q12_MULT_PRECISE(temp_a3, Q12_MULT_PRECISE(groanerProps.field_114, 0x3999)));
    }

    var_s0_2                               = 0;
    groanerProps.field_108                += g_DeltaTime;

    if (groanerProps.field_108 > Q12(6.0f))
    {
        if (groanerProps.field_108 > Q12(7.5f))
        {
            var_s0_2 = Q12(0.5f);
        }
        else
        {
            var_s0_2 = (Q12_MULT_PRECISE(groanerProps.field_108 - Q12(6.0f), Q12(1.5f)) >> 2) + Q12(0.25f);
        }
    }

    distToPlayer = Math_Vector2MagCalcSafeQ6(g_SysWork.playerWork.player.position.vx - groaner->position.vx,
                                       g_SysWork.playerWork.player.position.vz - groaner->position.vz);
    if (distToPlayer > Q12(3.5f))
    {
        if (distToPlayer > Q12(5.25f))
        {
            var_s0_2 += Q12(0.5f);
        }
        else
        {
            var_s0_2 += Q12(0.25f) + (Q12_MULT_PRECISE(distToPlayer - Q12(3.5f), Q12(1.75f)) >> 2);
        }
    }

    if (Rng_GenerateUInt(0, Q12_CLAMPED(1.0f)) < var_s0_2)
    {
        groaner->model.controlState = GroanerControl_RunForward;
    }
}

void sharedFunc_800E554C_2_s00(s_SubCharacter* groaner)
{
    if (!(groanerProps.flags.val16[0] & GroanerFlag_1) &&
        groaner->model.anim.status == ANIM_STATUS(GroanerAnim_StandIdle, true) &&
        !Rng_GenerateInt(0, 7)) // 1 in 8 chance.
    {
        groaner->model.controlState         = GroanerControl_WalkForward;
        groaner->model.anim.status = ANIM_STATUS(GroanerAnim_WalkForward, false);
    }
}

void sharedFunc_800E55B0_2_s00(s_SubCharacter* groaner)
{
    q3_12  timeScaled;
    s32    animTime;
    u32    animIdx;
    u8     newAnimStatus;

    if (!(groanerProps.flags.val16[0] & GroanerFlag_1))
    {
        Chara_MoveSpeedUpdate(groaner, Q12(1.8f));
    }

    if (!(groanerProps.flags.val16[0] & GroanerFlag_Airborne))
    {
        groanerProps.flags.val16[0] &= ~GroanerFlag_4;
    }

    if (groaner->model.controlState == GroanerControl_StunFromJump)
    {
        if (ANIM_TIME_RANGE_CHECK(groaner->model.anim.time, 39, 48))
        {
            groanerProps.flags.val16[0] |= GroanerFlag_4;
        }

        if (groanerProps.flags.val16[0] & GroanerFlag_4)
        {
            // TODO: Uncleanr float.
            if (groanerProps.flags.val16[0] & GroanerFlag_5)
            {
                timeScaled = Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 5.3333f);
            }
            else
            {
                timeScaled = Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 3.2188f);
            }

            groanerProps.field_F0 = FP_FROM(timeScaled * Math_Sin(groaner->rotation.vy + Q12_ANGLE(180.0f)), Q12_SHIFT);
            groanerProps.field_F2 = FP_FROM(timeScaled * Math_Cos(groaner->rotation.vy + Q12_ANGLE(180.0f)), Q12_SHIFT);
        }

        if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StunFromJump)
        {
            return;
        }
    }
    else
    {
        animTime = FP_FROM(groaner->model.anim.time, Q12_SHIFT);
        if ((animTime >= 134 && animTime < 157) ||
            (animTime >= 288 && animTime < 311))
        {
            groanerProps.flags.val16[0] |= GroanerFlag_4;
        }

        if (groanerProps.flags.val16[0] & GroanerFlag_4)
        {
            if (groanerProps.flags.val16[0] & GroanerFlag_5)
            {
                timeScaled = Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 1.6954f);
            }
            else
            {
                timeScaled = Q12_MULT_FLOAT_PRECISE(g_DeltaTime, 0.9781f);
            }

            if (groaner->model.controlState == GroanerControl_StunFromStandRight)
            {
                groanerProps.field_F0 = Q12_MULT(timeScaled, Math_Sin(groaner->rotation.vy - Q12_ANGLE(90.0f)));
                groanerProps.field_F2 = Q12_MULT(timeScaled, Math_Cos(groaner->rotation.vy - Q12_ANGLE(90.0f)));

                if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StunFromStandRight)
                {
                    return;
                }
            }
            else
            {
                groanerProps.field_F0 = Q12_MULT(timeScaled, Math_Sin(groaner->rotation.vy + Q12_ANGLE(90.0f)));
                groanerProps.field_F2 = Q12_MULT(timeScaled, Math_Cos(groaner->rotation.vy + Q12_ANGLE(90.0f)));

                if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StunFromStandLeft)
                {
                    return;
                }
            }
        }
    }

    if (!(groanerProps.flags.val16[0] & GroanerFlag_Airborne) && groaner->health == Q12(0.0f))
    {
        animIdx       = ANIM_STATUS_IDX_GET(groaner->model.anim.status);
        newAnimStatus = ANIM_STATUS((animIdx == GroanerAnim_StunFromJump) ? GroanerAnim_StunFromJumpDeathStart : GroanerAnim_Still, false);
        groaner->flags &= ~CharaFlag_Unk2;

        if (animIdx == GroanerAnim_StunFromStandRight)
        {
            newAnimStatus = ANIM_STATUS(GroanerAnim_StunFromStandRightDeathStart, false);
        }
        if (animIdx == GroanerAnim_StunFromStandLeft)
        {
            newAnimStatus = ANIM_STATUS(GroanerAnim_StunFromStandLeftDeathStart, false);
        }

        if (newAnimStatus != GroanerAnim_Still)
        {
            groanerProps.relKeyframeIdx_100 = 0;
            groaner->model.anim.status                               = newAnimStatus;
            groaner->model.controlState                                       = GroanerControl_Death;
        }
    }
}

void sharedFunc_800E5930_2_s00(s_SubCharacter* groaner)
{
    s32 newAnimStatus;
    u32 animStatus;

    if (!(groanerProps.flags.val16[0] & GroanerFlag_1))
    {
        Chara_MoveSpeedUpdate(groaner, Q12(1.8f));
    }

    if (groaner->health == Q12(0.0f))
    {
        if (g_SysWork.targetNpcIdx != Chara_NpcIdxGet(groaner))
        {
            groaner->health  = NO_VALUE;
            groaner->collision.state = 0;
        }
    }

    if (!(groanerProps.flags.val32 & (GroanerFlag_Airborne | GroanerFlag_6)) &&
        groaner->moveSpeed == Q12(0.0f))
    {
        animStatus    = ANIM_STATUS_IDX_GET(groaner->model.anim.status);
        newAnimStatus = ANIM_STATUS(GroanerAnim_Still, false);
        if (animStatus == ANIM_STATUS(GroanerAnim_StandIdle, false))
        {
            newAnimStatus = ANIM_STATUS(GroanerAnim_StandRecoilFront, true);
        }
        if (animStatus == ANIM_STATUS(GroanerAnim_StandIdle, true))
        {
            newAnimStatus = ANIM_STATUS(GroanerAnim_JumpToStun, false);
        }
        if (animStatus == ANIM_STATUS(GroanerAnim_JumpAttack, false))
        {
            newAnimStatus = ANIM_STATUS(GroanerAnim_JumpToStun, true);
        }

        if (newAnimStatus != ANIM_STATUS(GroanerAnim_Still, false))
        {
            func_800622B8(3, groaner, newAnimStatus, 6);
            groanerProps.flags.val16[0] |= GroanerFlag_6;
        }
    }
}

void sharedFunc_800E5AA4_2_s00(s_SubCharacter* groaner)
{
    s_CollisionResult sp10;
    q3_12      angleDeltaToHeading;

    if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_JumpAttack, true))
    {
        groaner->fallSpeed += g_GravitySpeed;
    }
    else
    {
        groaner->fallSpeed += g_GravitySpeed >> 1;
    }

    if (groanerProps.flags.val16[0] & GroanerFlag_1)
    {
        if (!(groanerProps.flags.val16[0] & GroanerFlag_Airborne))
        {
            if (groaner->model.anim.status == ANIM_STATUS(GroanerAnim_RunForward, true))
            {
                Chara_MoveSpeedUpdate(groaner, Q12(7.2f) - 3);
            }
            else
            {
                Chara_MoveSpeedUpdate(groaner, Q12(1.8f));
            }
        }

        if (groanerProps.field_FE > Q12(0.0f))
        {
            angleDeltaToHeading = Math_AngleNormalizeSigned((groaner->headingAngle - groaner->rotation.vy) + Q12_ANGLE(180.0f));
            if (ABS(angleDeltaToHeading) < Q12_ANGLE(45.0f))
            {
                angleDeltaToHeading = Q12_ANGLE(-180.0f);
            }
            else if (angleDeltaToHeading > Q12_ANGLE(0.0f))
            {
                angleDeltaToHeading = Q12_ANGLE(-90.0f);
            }
            else
            {
                angleDeltaToHeading = Q12_ANGLE(90.0f);
            }

            if (Math_AngleNormalizeSigned(groaner->headingAngle - (groaner->rotation.vy + angleDeltaToHeading)) > Q12_ANGLE(0.0f))
            {
                groaner->rotation.vy += Q12_MULT_PRECISE(g_DeltaTime, groanerProps.field_FE);

                if (Math_AngleNormalizeSigned(groaner->headingAngle - (groaner->rotation.vy + angleDeltaToHeading)) <= Q12_ANGLE(0.0f))
                {
                    groanerProps.field_FE = Q12(0.0f);
                    groaner->rotation.vy                 = groaner->headingAngle - angleDeltaToHeading;
                }
            }
            else
            {
                groaner->rotation.vy -= Q12_MULT_PRECISE(g_DeltaTime, groanerProps.field_FE);

                if (Math_AngleNormalizeSigned(groaner->headingAngle - (groaner->rotation.vy + angleDeltaToHeading)) >= Q12_ANGLE(0.0f))
                {
                    groanerProps.field_FE = Q12(0.0f);
                    groaner->rotation.vy                 = groaner->headingAngle - angleDeltaToHeading;
                }
            }

            groanerProps.field_FE = CLAMP_LOW(groanerProps.field_FE - FP_MULTIPLY_PRECISE(g_DeltaTime,
                                                                                          Q12(1.0f / 3.0f),
                                                                                          Q12_SHIFT),
                                              Q12(0.0f));
        }
        else if (groaner->moveSpeed < Q12(0.225f))
        {
            groanerProps.flags.val16[0] &= ~GroanerFlag_1;
        }
    }
    else if (groaner->model.anim.status != ANIM_STATUS(GroanerAnim_JumpAttack, true))
    {
        groaner->headingAngle = groaner->rotation.vy;
    }

    func_8005CB20(groaner, &sp10, groanerProps.field_F0, groanerProps.field_F2);

    groanerProps.field_F2 = Q12(0.0f);
    groanerProps.field_F0 = Q12(0.0f);

    if (groaner->position.vy == sp10.surface.groundHeight)
    {
        groanerProps.flags.val16[0] &= ~GroanerFlag_Airborne;
    }
    else
    {
        groanerProps.flags.val16[0] |= GroanerFlag_Airborne;
    }
}

void sharedFunc_800E5EC4_2_s00(s_SubCharacter* groaner, s_AnmHeader* anmHdr, GsCOORDINATE2* coords)
{
    typedef struct
    {
        SVECTOR field_0;
        MATRIX  field_8;
    } s_sharedFunc_800E5EC4_2_s00;

    q3_12                        angle1;
    q3_12                        angleDeltaToTarget;
    q3_12                        angle0;
    q19_12                       constantDur;
    s_AnimInfo*                  animInfo;
    s_sharedFunc_800E5EC4_2_s00* ptr;

    switch (groaner->model.anim.status)
    {
        case ANIM_STATUS(GroanerAnim_JumpAttack, true):
            if (ANIM_TIME_RANGE_CHECK(groaner->model.anim.time, 207, 219))
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(24.0f);
            }

            if (ANIM_TIME_RANGE_CHECK(groaner->model.anim.time, 220, 225))
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(24.0f);
            }

            if (ANIM_TIME_RANGE_CHECK(groaner->model.anim.time, 226, 243))
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(30.0f);
            }
            break;

        case ANIM_STATUS(GroanerAnim_JumpToStun, true):
            if (groanerProps.flags.val16[0] & GroanerFlag_5)
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(32.0f);
            }
            else
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(25.75f);
            }
            break;

        case ANIM_STATUS(GroanerAnim_StandToStunRight, true):
        case ANIM_STATUS(GroanerAnim_StandToStunLeft, true):
            if (groanerProps.flags.val16[0] & GroanerFlag_5)
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(30.0f);
            }
            else
            {
                GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = Q12(22.5f);
            }
            break;

        case ANIM_STATUS(GroanerAnim_RunForward, true):
            if (groanerProps.targetHeadingAngle == groaner->rotation.vy)
            {
                constantDur = groaner->moveSpeed;
            }
            else
            {
                if (groaner->moveSpeed >= Q12(1.0f))
                {
                    constantDur = groaner->moveSpeed;
                }
                else
                {
                    constantDur = Q12(1.0f);
                }
            }

            constantDur                                                              = Q12_DIV(constantDur * 20, Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f)));
            GROANER_ANIM_INFOS[groaner->model.anim.status].duration.constant = constantDur;
            break;
    }

    Math_MatrixTransform(&groaner->position, (SVECTOR*)&groaner->rotation, coords);

    animInfo = &GROANER_ANIM_INFOS[groaner->model.anim.status];
    animInfo->playbackFunc(&groaner->model, anmHdr, coords, animInfo);

    ptr = PSX_SCRATCH;

    switch (groaner->model.anim.status)
    {
        case ANIM_STATUS(GroanerAnim_WalkForward, false):
        case ANIM_STATUS(GroanerAnim_WalkForward, true):
            angle1    = Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(90.0f));
            angle0    = Q12_ANGLE(33.75f);
            break;

        case ANIM_STATUS(GroanerAnim_JumpAttack, false):
        case ANIM_STATUS(GroanerAnim_JumpAttack, true):
        case ANIM_STATUS(GroanerAnim_StandToJumpAttack, false):
        case ANIM_STATUS(GroanerAnim_StandToJumpAttack, true):
        case ANIM_STATUS(GroanerAnim_RunForward, false):
        case ANIM_STATUS(GroanerAnim_RunForward, true):
            angle1    = Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(135.0f));
            angle0    = Q12_ANGLE(56.25f);
            break;

        default:
            angle1    = Q12_MULT_PRECISE(g_DeltaTime, Q12_ANGLE(135.0f));
            angle0    = Q12_ANGLE(0.0f);
            break;
    }

    angleDeltaToTarget = Math_AngleNormalizeSigned(groanerProps.targetHeadingAngle - groaner->rotation.vy);
    if (angleDeltaToTarget == Q12_ANGLE(0.0f))
    {
        angle0 = Q12_ANGLE(0.0f);
    }
    else if (angleDeltaToTarget < Q12_ANGLE(0.0f))
    {
        angle0 = -angle0;
    }

    if (groanerProps.flexAngle > angle0)
    {
        groanerProps.flexAngle = MAX(angle0, groanerProps.flexAngle - angle1);
    }
    else
    {
        groanerProps.flexAngle = CLAMP_HIGH(angle0, groanerProps.flexAngle + angle1);
    }

    *(s32*)&ptr->field_0 = (groanerProps.flexAngle >> 2) << 16;
    ptr->field_0.vz      = 0;

    Math_RotMatrixZxyNegGte(&ptr->field_0, &ptr->field_8);
    MulMatrix(&coords[3].coord, &ptr->field_8);
    MulMatrix(&coords[4].coord, &ptr->field_8);

    *(s32*)&ptr->field_0.vx = groanerProps.flexAngle << 16;
    ptr->field_0.vz         = 0;

    Math_RotMatrixZxyNegGte(&ptr->field_0, &ptr->field_8);
    MulMatrix(&coords[1].coord, &ptr->field_8);

    groaner->rotation.vy                 = Math_AngleNormalizeSigned(groaner->rotation.vy);
    groanerProps.targetHeadingAngle = groaner->rotation.vy;
}

extern s_Keyframe sharedData_800EEE54_2_s00[];
extern s_Keyframe sharedData_800EF0AC_2_s00[];
extern s_Keyframe sharedData_800EF19C_2_s00;
extern s_Keyframe sharedData_800EF1B0_2_s00[];
extern s_Keyframe sharedData_800EF250_2_s00[];
extern s_Keyframe sharedData_800EF28C_2_s00[];
extern s_Keyframe sharedData_800EF368_2_s00[];
extern s_Keyframe sharedData_800EF4BC_2_s00;
extern s_Keyframe sharedData_800EF4D0_2_s00[];
extern s_Keyframe sharedData_800EF624_2_s00;
extern s_Keyframe sharedData_800EF638_2_s00[];
extern s_Keyframe sharedData_800EF7A0_2_s00;
extern s_Keyframe sharedData_800EF7B4_2_s00[];
extern s_Keyframe sharedData_800EFA84_2_s00[];
extern s_Keyframe sharedData_800EFCDC_2_s00[];
extern s_Keyframe sharedData_800EFF48_2_s00[];
extern s_Keyframe sharedData_800F0038_2_s00[];
extern s_Keyframe sharedData_800F00EC_2_s00[];
extern s_Keyframe sharedData_800F0268_2_s00[];
extern s_Keyframe sharedData_800F03A8_2_s00[];
extern s_Keyframe sharedData_800F04C0_2_s00[];

#define CopyData(arg0, data)                  \
{                                             \
    s32 __temp;                               \
    s32 __temp2;                              \
                                              \
    arg0->collision.box.top = data.field_0;    \
                                              \
    __temp                 = data.field_2;    \
    arg0->collision.box.bottom = __temp;          \
    arg0->collision.box.height = data.field_4;    \
                                              \
    __temp                   = data.field_6;  \
    arg0->collision.box.offsetY   = __temp;        \
    arg0->collision.shapeOffsets.cylinder.vx = data.field_10; \
                                              \
    __temp                   = data.field_12; \
    arg0->collision.shapeOffsets.cylinder.vz = __temp;        \
    arg0->collision.cylinder.radius  = data.field_8;  \
    arg0->collision.shapeOffsets.box.vx = data.field_C;  \
                                              \
    __temp                   = data.field_E;  \
    arg0->collision.shapeOffsets.box.vz = __temp;        \
                                              \
    __temp2                = data.field_A;    \
    arg0->collision.cylinder.field_2 = __temp2;         \
}

#define ANIM_INDEX_FROM_KEYFRAME(offset, keyframe, max, count) \
    ((offset) - ((keyframe) > (count)) < (max) ? ((offset) - ((keyframe) > (count))) : ((max) - 1))

void sharedFunc_800E6338_2_s00(s_SubCharacter* groaner)
{
    s32 keyframeIdx2;
    s32 keyframeIdx1;
    s32 keyframeIdx0;
    s32 temp;

    switch (groaner->model.anim.status)
    {
        case ANIM_STATUS(GroanerAnim_StandIdle, false):
            if (groanerProps.relKeyframeIdx_100 == 5)
            {
                Collision_CharaCollisionSet(groaner, &sharedData_800EF4BC_2_s00, &sharedData_800EF250_2_s00[0]);
            }
            else if (groanerProps.relKeyframeIdx_100 == 13)
            {
                Collision_CharaCollisionSet(groaner, &sharedData_800EF624_2_s00, &sharedData_800EF250_2_s00[0]);
            }
            else if (groanerProps.relKeyframeIdx_100 == 25)
            {
                Collision_CharaCollisionSet(groaner, &sharedData_800EF7A0_2_s00, &sharedData_800EF250_2_s00[0]);
            }
            break;

        case ANIM_STATUS(GroanerAnim_StandIdle, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 191;
            keyframeIdx0 = !(keyframeIdx2 < 5) + !(keyframeIdx2 < 12);
            keyframeIdx1 = !(keyframeIdx2 < 4) + !(keyframeIdx2 < 11);

            if (keyframeIdx2 == 15)
            {
                keyframeIdx1 -= 2;
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800EF250_2_s00[keyframeIdx0], &sharedData_800EF250_2_s00[keyframeIdx1]);
            groanerProps.relKeyframeIdx_100 = keyframeIdx2;
            break;

        case ANIM_STATUS(GroanerAnim_WalkForward, false):
            if (groanerProps.relKeyframeIdx_100 != 100)
            {
                keyframeIdx2      = groanerProps.relKeyframeIdx_100;
                keyframeIdx0 = !(keyframeIdx2 < 5) + !(keyframeIdx2 < 12);
                Collision_CharaCollisionSet(groaner, &sharedData_800EF250_2_s00[keyframeIdx0], &sharedData_800EF28C_2_s00[0]);
            }
            else
            {
                CopyData(groaner, sharedData_800EF28C_2_s00[0]);
            }
            break;

        case ANIM_STATUS(GroanerAnim_WalkForward, true):
            keyframeIdx2 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 371;

            if (keyframeIdx2 < 12)
            {
                if (keyframeIdx2 < 9)
                {
                    if (keyframeIdx2 < 4)
                    {
                        keyframeIdx0 = keyframeIdx2;
                    }
                    else
                    {
                        keyframeIdx0 = 3;
                    }
                }
                else
                {
                    keyframeIdx0 = 4;
                }
            }
            else if (keyframeIdx2 < 22)
            {
                if (((keyframeIdx2 >> 1) - 1) < 8)
                {
                    keyframeIdx0 = (keyframeIdx2 >> 1) - 1;
                }
                else
                {
                    keyframeIdx0 = 7;
                }
            }
            else
            {
                keyframeIdx0 = keyframeIdx2 - 14 - !(keyframeIdx2 < 24);
            }

            if (keyframeIdx2 < 11)
            {
                if (keyframeIdx2 < 8)
                {
                    if ((keyframeIdx2 + 1) < 4)
                    {
                        keyframeIdx1 = keyframeIdx2 + 1;
                    }
                    else
                    {
                        keyframeIdx1 = 3;
                    }
                }
                else
                {
                    keyframeIdx1 = 4;
                }
            }
            else if (keyframeIdx2 < 21)
            {
                if (((keyframeIdx2 - 1) >> 1) < 8)
                {
                    keyframeIdx1 = (keyframeIdx2 - 1) >> 1;
                }
                else
                {
                    keyframeIdx1 = 7;
                }
            }
            else if (keyframeIdx2 != 25)
            {
                keyframeIdx1 = (keyframeIdx2 - 13) - !(keyframeIdx2 < 23);
            }
            else
            {
                keyframeIdx1 = 0;
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800EF28C_2_s00[keyframeIdx0], &sharedData_800EF28C_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilFront, false):
            CopyData(groaner, sharedData_800EF368_2_s00[2]);
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilFront, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 16;
            keyframeIdx0 = (keyframeIdx2 - (keyframeIdx2 > 0)) - !(keyframeIdx2 < 10);
            keyframeIdx1 = (keyframeIdx2 - !(keyframeIdx2 < 9)) - !(keyframeIdx2 < 19);
            Collision_CharaCollisionSet(groaner, &sharedData_800EF368_2_s00[keyframeIdx0], &sharedData_800EF368_2_s00[keyframeIdx1]);

            groanerProps.relKeyframeIdx_100 = 5;
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilRight, false):
            CopyData(groaner, sharedData_800EF4D0_2_s00[2]);
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilRight, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 111;
            keyframeIdx0 = (keyframeIdx2 - (keyframeIdx2 > 0)) - !(keyframeIdx2 < 11);
            keyframeIdx1 = keyframeIdx2 - !(keyframeIdx2 < 10);
            Collision_CharaCollisionSet(groaner, &sharedData_800EF4D0_2_s00[keyframeIdx0], &sharedData_800EF4D0_2_s00[keyframeIdx1]);

            groanerProps.relKeyframeIdx_100 = 13;
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilLeft, false):
            CopyData(groaner, sharedData_800EF638_2_s00[3]);
            break;

        case ANIM_STATUS(GroanerAnim_StandRecoilLeft, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 265;
            keyframeIdx0 = (keyframeIdx2 - !(keyframeIdx2 < 9));
            keyframeIdx1 = (FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 264) - !(keyframeIdx2 < 8);
            Collision_CharaCollisionSet(groaner, &sharedData_800EF638_2_s00[keyframeIdx0], &sharedData_800EF638_2_s00[keyframeIdx1]);

            groanerProps.relKeyframeIdx_100 = 25;
            break;

        case ANIM_STATUS(GroanerAnim_JumpToStun, false):
            CopyData(groaner, sharedData_800EF7B4_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_JumpToStun, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 37;
            keyframeIdx0 = ((FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 39) - !(keyframeIdx2 < 17)) - !(keyframeIdx2 < 38);
            keyframeIdx1 = (((FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 38) - !(keyframeIdx2 < 16)) - !(keyframeIdx2 < 37)) - !(keyframeIdx2 < 39);
            Collision_CharaCollisionSet(groaner, &sharedData_800EF7B4_2_s00[keyframeIdx0], &sharedData_800EF7B4_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToStunRight, false):
            CopyData(groaner, sharedData_800EFA84_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToStunRight, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 131;
            keyframeIdx0 = (FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 134) - !(keyframeIdx2 < 31);
            keyframeIdx1 = (FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 133) - !(keyframeIdx2 < 30);
            Collision_CharaCollisionSet(groaner, &sharedData_800EFA84_2_s00[keyframeIdx0], &sharedData_800EFA84_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToStunLeft, false):
            CopyData(groaner, sharedData_800EFCDC_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToStunLeft, true):
            keyframeIdx2 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 285;
            keyframeIdx0 = (keyframeIdx2 - (keyframeIdx2 > 0)) - !(keyframeIdx2 < 31);
            keyframeIdx1 = (keyframeIdx2 - !(keyframeIdx2 < 30)) - !(keyframeIdx2 < 32);
            Collision_CharaCollisionSet(groaner, &sharedData_800EFCDC_2_s00[keyframeIdx0], &sharedData_800EFCDC_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromJump, false):
            CopyData(groaner, sharedData_800EFF48_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromJump, true):
            keyframeIdx2 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 78;
            keyframeIdx0 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 82;

            if (keyframeIdx2 < 9)
            {
                if (keyframeIdx2 <= 0)
                {
                    if (keyframeIdx2 < 6)
                    {
                        keyframeIdx0 = keyframeIdx2 - (keyframeIdx2 > 0);
                    }
                    else
                    {
                        keyframeIdx0 = 5;
                    }
                }
                else
                {
                    if (keyframeIdx2 - 1 < 6)
                    {
                        keyframeIdx0 = keyframeIdx2 - (keyframeIdx2 > 0);
                    }
                    else
                    {
                        keyframeIdx0 = 5;
                    }
                }
            }

            if (keyframeIdx2 != 15)
            {
                if (keyframeIdx2 < 8)
                {
                    if (keyframeIdx2 < 6)
                    {
                        keyframeIdx1 = keyframeIdx2;
                    }
                    else
                    {
                        keyframeIdx1 = 5;
                    }
                }
                else
                {
                    keyframeIdx1 = keyframeIdx2 - 3;
                }
            }
            else
            {
                keyframeIdx1 = 0;
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800EFF48_2_s00[keyframeIdx0], &sharedData_800EFF48_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandRight, false):
            CopyData(groaner, sharedData_800F0038_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandRight, true):
            keyframeIdx2 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 165;
            if (keyframeIdx2 < 10)
            {
                if (keyframeIdx2 < 6)
                {
                    keyframeIdx0 = 1;
                    keyframeIdx1 = keyframeIdx2 >> 1;
                    if (keyframeIdx1 < 2)
                    {
                        keyframeIdx0 = keyframeIdx1;
                    }
                }
                else
                {
                    keyframeIdx0 = 4;
                    keyframeIdx1 = keyframeIdx2 - 4;
                    if (keyframeIdx1 < 5)
                    {
                        keyframeIdx0 = keyframeIdx1;
                    }
                }
            }
            else
            {
                if (keyframeIdx2 >= 18)
                {
                    keyframeIdx0 = 8;
                    keyframeIdx1 = (keyframeIdx2 >> 1) - 3;
                    if (keyframeIdx1 < 9)
                    {
                        keyframeIdx0 = keyframeIdx1;
                    }
                }
                else
                {
                    keyframeIdx0 = 5;
                }
            }

            if (keyframeIdx2 < 17)
            {
                if (keyframeIdx2 < 5)
                {
                    if (keyframeIdx2 < 2)
                    {
                        keyframeIdx1 = keyframeIdx2;
                    }
                    else
                    {
                        keyframeIdx1 = 1;
                    }
                }
                else
                {
                    keyframeIdx1 = ANIM_INDEX_FROM_KEYFRAME(keyframeIdx2 - 3, keyframeIdx2, 6, 7);
                }
            }
            else
            {
                if (keyframeIdx2 != 25)
                {
                    if ((((keyframeIdx2 - 1) >> 1) - 2) < 9)
                    {
                        keyframeIdx1 = ((keyframeIdx2 - 1) >> 1) - 2;
                    }
                    else
                    {
                        keyframeIdx1 = 8;
                    }
                }
                else
                {
                    keyframeIdx1 = 0;
                }
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800F0038_2_s00[keyframeIdx0], &sharedData_800F0038_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandLeft, false):
            CopyData(groaner, sharedData_800F00EC_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandLeft, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 319;
            keyframeIdx0 = keyframeIdx2 >> 1;

            if (keyframeIdx2 >= 7)
            {
                if (keyframeIdx2 < 16)
                {
                    keyframeIdx0 = ANIM_INDEX_FROM_KEYFRAME(keyframeIdx2 - 3, keyframeIdx2, 9, 9);
                }
                else
                {
                    keyframeIdx0 = (keyframeIdx2 - 7) - !(keyframeIdx2 < 20);
                }
            }

            if (keyframeIdx2 < 9)
            {
                keyframeIdx1 = keyframeIdx2 - !(keyframeIdx2 < 2) - !(keyframeIdx2 < 4);
            }
            else
            {
                if (keyframeIdx2 < 15)
                {
                    if ((keyframeIdx2 - 5) < 9)
                    {
                        keyframeIdx1 = keyframeIdx2 - 5;
                    }
                    else
                    {
                        keyframeIdx1 = 8;
                    }
                }
                else
                {
                    if (keyframeIdx2 != 26)
                    {
                        keyframeIdx1 = (keyframeIdx2 - 6) - !(keyframeIdx2 < 19);
                    }
                    else
                    {
                        keyframeIdx1 = 0;
                    }
                }
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800F00EC_2_s00[keyframeIdx0], &sharedData_800F00EC_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromJumpDeathStart, false):
        case ANIM_STATUS(GroanerAnim_StunFromJumpRecoil, false):
            keyframeIdx2 = groanerProps.relKeyframeIdx_100;
            if (keyframeIdx2 < 9)
            {
                keyframeIdx0 = ANIM_INDEX_FROM_KEYFRAME(keyframeIdx2, keyframeIdx2, 6, 0);
            }
            else
            {
                keyframeIdx0 = keyframeIdx2 - 4;
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800EFF48_2_s00[keyframeIdx0], &sharedData_800F0268_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromJumpDeathStart, true):
        case ANIM_STATUS(GroanerAnim_StunFromJumpRecoil, true):
            keyframeIdx0 = FP_FROM(groaner->model.anim.time, Q12_SHIFT);
            keyframeIdx1 = keyframeIdx0 + 1;
            Collision_CharaCollisionSet(groaner, &sharedData_800F0268_2_s00[keyframeIdx0], &sharedData_800F0268_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromJumpDeathEnd, false):
        case ANIM_STATUS(GroanerAnim_StunFromJumpDeathEnd, true):
            CopyData(groaner, sharedData_800F0268_2_s00[15]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandRightDeathStart, false):
        case ANIM_STATUS(GroanerAnim_StunFromStandRightRecoil, false):
            keyframeIdx2 = groanerProps.relKeyframeIdx_100;
            if (keyframeIdx2 < 10)
            {
                if (keyframeIdx2 < 6)
                {
                    keyframeIdx0 = 1;
                    keyframeIdx1 = keyframeIdx2 >> 1;
                    if (keyframeIdx1 < 2)
                    {
                        keyframeIdx0 = keyframeIdx1;
                    }
                }
                else
                {
                    keyframeIdx0 = 4;
                    keyframeIdx1 = keyframeIdx2 - 4;
                    if (keyframeIdx1 < 5)
                    {
                        keyframeIdx0 = keyframeIdx1;
                    }
                }
            }
            else if (keyframeIdx2 >= 18)
            {
                keyframeIdx0 = 8;
                keyframeIdx1 = (keyframeIdx2 >> 1) - 3;
                if (keyframeIdx1 < 9)
                {
                    keyframeIdx0 = keyframeIdx1;
                }
            }
            else
            {
                keyframeIdx0 = 5;
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800F0038_2_s00[keyframeIdx0], &sharedData_800F03A8_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandRightDeathStart, true):
        case ANIM_STATUS(GroanerAnim_StunFromStandRightRecoil, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 94;
            keyframeIdx0 = (keyframeIdx2 - !(keyframeIdx2 < 13)) - !(keyframeIdx2 < 15);
            keyframeIdx1 = (((FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 93) - !(keyframeIdx2 < 12)) - !(keyframeIdx2 < 14)) - !(keyframeIdx2 < 15);
            Collision_CharaCollisionSet(groaner, &sharedData_800F03A8_2_s00[keyframeIdx0], &sharedData_800F03A8_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandRightDeathEnd, false):
        case ANIM_STATUS(GroanerAnim_StunFromStandRightDeathEnd, true):
            CopyData(groaner, sharedData_800F03A8_2_s00[13]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandLeftDeathStart, false):
        case ANIM_STATUS(GroanerAnim_StunFromStandLeftRecoil, false):
            keyframeIdx2      = groanerProps.relKeyframeIdx_100;
            keyframeIdx0 = keyframeIdx2 >> 1;
            if (keyframeIdx2 >= 7)
            {
                if (keyframeIdx2 < 16)
                {
                    keyframeIdx0 = ANIM_INDEX_FROM_KEYFRAME(keyframeIdx2 - 3, keyframeIdx2, 9, 9);
                }
                else
                {
                    keyframeIdx0 = (keyframeIdx2 - 7) - !(keyframeIdx2 < 20);
                }
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800F00EC_2_s00[keyframeIdx0], &sharedData_800F04C0_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandLeftDeathStart, true):
        case ANIM_STATUS(GroanerAnim_StunFromStandLeftRecoil, true):
            keyframeIdx2 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 244;
            if (keyframeIdx2 < 12)
            {
                keyframeIdx0 = (keyframeIdx2 - (keyframeIdx2 > 0)) - !(keyframeIdx2 < 9);
            }
            else
            {
                temp = keyframeIdx2 >> 1;
                if (keyframeIdx2 >= 16)
                {
                    keyframeIdx0 = temp + 3;
                }
                else
                {
                    keyframeIdx0 = temp + 4;
                }
            }

            if (keyframeIdx2 < 12)
            {
                keyframeIdx1 = keyframeIdx2 - !(keyframeIdx2 < 8);
            }
            else
            {
                temp = (keyframeIdx2 - 1) >> 1;
                if (keyframeIdx2 >= 15)
                {
                    keyframeIdx1 = temp + 4;
                }
                else
                {
                    keyframeIdx1 = temp + 5;
                }
            }

            Collision_CharaCollisionSet(groaner, &sharedData_800F04C0_2_s00[keyframeIdx0], &sharedData_800F04C0_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_StunFromStandLeftDeathEnd, false):
        case ANIM_STATUS(GroanerAnim_StunFromStandLeftDeathEnd, true):
            CopyData(groaner, sharedData_800F04C0_2_s00[13]);
            break;

        case ANIM_STATUS(GroanerAnim_RunForward, false):
            CopyData(groaner, sharedData_800EF1B0_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_RunForward, true):
            keyframeIdx0 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 363;
            Collision_CharaCollisionSet(groaner, &sharedData_800EF1B0_2_s00[keyframeIdx0], &sharedData_800EF1B0_2_s00[(FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 362) & 0x7]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToJumpAttack, false):
            CopyData(groaner, sharedData_800EF0AC_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_StandToJumpAttack, true):
            keyframeIdx2      = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 346;
            keyframeIdx0 = (((keyframeIdx2 - !(keyframeIdx2 < 3)) - !(keyframeIdx2 < 9)) - !(keyframeIdx2 < 12)) - !(keyframeIdx2 < 13);
            keyframeIdx1 = ((((FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 345) - !(keyframeIdx2 < 2)) - !(keyframeIdx2 < 8)) - !(keyframeIdx2 < 11)) - !(keyframeIdx2 < 12);
            Collision_CharaCollisionSet(groaner, &sharedData_800EF0AC_2_s00[keyframeIdx0], &sharedData_800EF0AC_2_s00[keyframeIdx1]);
            break;

        case ANIM_STATUS(GroanerAnim_JumpAttack, false):
            Collision_CharaCollisionSet(groaner, &sharedData_800EF19C_2_s00, &sharedData_800EEE54_2_s00[0]);
            break;

        case ANIM_STATUS(GroanerAnim_JumpAttack, true):
            keyframeIdx0 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 214;
            keyframeIdx1 = FP_FROM(groaner->model.anim.time, Q12_SHIFT) - 213;
            Collision_CharaCollisionSet(groaner, &sharedData_800EEE54_2_s00[keyframeIdx0], &sharedData_800EEE54_2_s00[keyframeIdx1]);

            groaner->collision.box.field_8 = Q12(-0.8f);
            break;
    }

    Chara_CollisionShapeOffsetsUpdate(&groaner->collision.shapeOffsets, groaner);
}

void sharedFunc_800E71E8_2_s00(s_SubCharacter* groaner)
{
    s32    keyframeIdx;
    q19_12 sfxVol;
    s32    sfxPitch;
    s32    temp_a0;

    if (groanerProps.timer_10C > Q12(0.0f))
    {
        groanerProps.timer_10C -= g_DeltaTime;
        if (groanerProps.timer_10C <= Q12(0.0f))
        {
            groanerProps.timer_10C = Q12(0.0f);
            groanerProps.field_10E = 0;
        }
    }
    else if (groanerProps.timer_10C < Q12(0.0f))
    {
        groanerProps.timer_10C += g_DeltaTime;
        if (groanerProps.timer_10C >= Q12(0.0f))
        {
            func_8005DC1C(Sfx_Unk1411, &groaner->position, Q8(0.5f), 0);
            groanerProps.timer_10C = Q12(2.0f);
        }
    }

    switch (groanerProps.field_10E)
    {
        default:
            temp_a0 = groanerProps.field_10F;
            if (temp_a0 == 4)
            {
                groanerProps.timer_10C = Q12(1.0f);
                groanerProps.field_10E = temp_a0;

                if (groaner->health > Q12(0.0f) &&
                    (groanerProps.flags.val32 & (GroanerFlag_3 | GroanerFlag_12)) == GroanerFlag_3)
                {
                    func_8005DC1C(Sfx_Unk1409, &groaner->position, Q8(0.5f), 0);
                }
                else
                {
                    func_8005DC1C(Sfx_Unk1414, &groaner->position, Q8(0.5f), 0);
                }

                groanerProps.flags.val16[0] &= ~GroanerFlag_12;
                break;
            }
            else if (groanerProps.field_10E != 2 && temp_a0 == 2)
            {
                groanerProps.timer_10C = Q12(-0.3f);
                groanerProps.field_10E = temp_a0;
                break;
            }

        case 4:
            if (groanerProps.field_10E == 0 && groanerProps.field_10F == 0 &&
                (groaner->health > Q12(0.0f)))
            {
                if (groaner->model.anim.status == 33)
                {
                    func_8005DC1C(Sfx_Unk1410, &groaner->position, Q8(0.25f), 0);
                    groanerProps.timer_10C = Rng_GenerateInt(Q12(0.5f), Q12(0.6f) - 1);
                    groanerProps.field_10E = 3;
                }
                else if (ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_JumpAttack &&
                         ANIM_STATUS_IDX_GET(groaner->model.anim.status) != GroanerAnim_StandToJumpAttack &&
                         !Rng_GenerateUInt(0, 15))
                {
                    func_8005DC1C(Sfx_Unk1406, &groaner->position, (Q12_DIV(groaner->health, Q12(280.0f)) >> 7) + 32, 0);
                    groanerProps.timer_10C = Rng_GenerateInt(Q12(4.8f), Q12(8.0f) - 2);
                    groanerProps.field_10E = 1;
                }
            }
            break;
    }

    groanerProps.field_10F = 0;

    keyframeIdx = FP_FROM(groaner->model.anim.time, Q12_SHIFT);
    sfxVol      = (Q12_DIV(groaner->moveSpeed, Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f))) >> 6) + 64;
    sfxPitch    = (Q12_DIV(groaner->moveSpeed, Q12_MULT_PRECISE(groanerProps.field_114, Q12(3.6f))) >> 7) + Rng_GenerateUInt(0, 7);

    if ((keyframeIdx > 365 && keyframeIdx < 370) ||
        (keyframeIdx > 374 && keyframeIdx < 382))
    {
        if (groanerProps.playLeftFootstepSfx == 0)
        {
            func_8005DD44(Sfx_GroanerFootstep, &groaner->position, sfxVol, sfxPitch);
            groanerProps.playLeftFootstepSfx++;
        }
    }
    else
    {
        groanerProps.playLeftFootstepSfx = 0;
    }

    if ((keyframeIdx > 367 && keyframeIdx < 373) ||
        (keyframeIdx > 390 && keyframeIdx < 397))
    {
        if (groanerProps.playRightFootstepSfx == 0)
        {
            func_8005DD44(Sfx_GroanerFootstep, &groaner->position, sfxVol, sfxPitch);
            groanerProps.playRightFootstepSfx++;
        }
    }
    else
    {
        groanerProps.playRightFootstepSfx = 0;
    }
}

#undef groanerProps

#ifdef SH_PC_PORT
/* PC port: pull in the dispatch table + keyframe data we extracted from
 * disc_extract/VIN/MAP2_S00.BIN. Each map DLL that #includes groaner.c
 * gets its own copy of these definitions, which is required because
 * the dispatch table points to sharedFunc_800Exxxx functions defined
 * earlier in this same translation unit (the exe import lib can't see
 * DLL-internal functions). */
#include "groaner_rodata.inc"
#endif
