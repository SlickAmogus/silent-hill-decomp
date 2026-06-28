#include <SDL2/SDL.h>
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/player.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/math/math.h"
#include "bodyprog/collision/ray.h"
#include "pc_combat.h"

extern const unsigned char* g_sdlKeyboardState;
extern int                  g_PcAimDevice; /* 0 = mouse, 1 = controller (game_main.c) */

/* Returns true on the frame `sdlScancode` transitions 0→1.
 *
 * Frame-stable: prev-state is sampled at most once per VBlank, so multiple
 * callers in the same frame all see the same rising-edge result. Without
 * this, a press that opens the inventory in gameplay state would also fire
 * a "rising edge" again the first time the inventory state queries it
 * (since each fresh slot starts with prev=0), instantly closing it. */
bool PC_KeyboardKeyClicked(int sdlScancode)
{
    #define PC_KEY_CACHE_SIZE 8
    static int  s_keys[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_prev[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_edge[PC_KEY_CACHE_SIZE]   = {0};
    static s32  s_frame[PC_KEY_CACHE_SIZE]  = {0};
    static int  s_count                     = 0;
    static bool s_initFrames                = false;

    if (!g_sdlKeyboardState) return false;

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_keys[i] == sdlScancode) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count >= PC_KEY_CACHE_SIZE) return false;
        slot = s_count++;
        s_keys[slot]  = sdlScancode;
        /* Seed prev with current held state — if the key is already down
         * when first queried, that's NOT a rising edge. */
        s_prev[slot]  = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot]  = false;
        s_frame[slot] = g_VBlanks;
        return false;
    }

    /* Resample only once per frame (per slot). Other call sites in the
     * same frame get the cached edge result. */
    if (s_frame[slot] != g_VBlanks) {
        bool nowHeld = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot] = nowHeld && !s_prev[slot];
        s_prev[slot] = nowHeld;
        s_frame[slot] = g_VBlanks;
    }
    return s_edge[slot];
}

/* Returns true on the rising edge of the manual-reload key (R) while a gun
 * weapon is equipped with reserve ammo available.
 *
 * The PSX game had no manual reload input — reload triggered automatically
 * on a fire-with-empty-clip. PC adds a dedicated R-key reload as a
 * convenience, bound outside the PSX controller mapping (so all PSX buttons
 * keep their original semantics: Triangle still opens map, Square still
 * runs, etc.). */
bool PC_PlayerManualReloadRequested(void)
{
    return PC_KeyboardKeyClicked(SDL_SCANCODE_R) &&
           g_SysWork.playerCombat.weaponAttack >= WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap) &&
           g_SysWork.playerCombat.totalWeaponAmmo != 0 &&
           INV_ITEM_GROUP(g_SavegamePtr->equippedWeapon) == InvItemGroup_GunWeapons;
}

/* OTS/TPS free-aim aim assist.
 *
 * Free-aim casts a single screen-center bullet ray; an enemy only takes a hit
 * when that ray threads its tight char-collision cylinder AND the separate
 * hand->target damage ray (cast from Harry's hand, offset from the camera eye)
 * threads that same cylinder. The parallax between the two origins meant you had
 * to land the reticle on a narrow strip near the enemy's central axis to score a
 * hit — the "tiny hitbox" feel.
 *
 * Fix: when the reticle is over an enemy's body (mouse) or near it (controller
 * auto-aim), return a point ON the enemy's vertical axis at the aimed height.
 * Pointing the gun at the axis guarantees the hand->target damage ray threads
 * the cylinder, so a hit registers anywhere on the body. The body is modeled as
 * a vertical capsule [box.top..box.height] of radius cylinder.field_2 — the same
 * extents the engine's character trace (func_8006EE0C box path) uses for bullets.
 *
 * camFwd is the unit (Q12) view forward; the ray is camPos + camFwd*t. Returns
 * the chosen NPC index in g_SysWork.npcs[] (or NO_VALUE), writing the world-space
 * aim point to *outAimPoint on success. Nearest qualifying enemy wins.
 */
#define AA_MOUSE_RADIUS_MUL  Q12(1.8f)  /* mouse: hittable radius = 1.8x collision radius (covers visible body) */
#define AA_CTRL_RADIUS_MUL   Q12(3.0f)  /* controller: close-range floor radius */
#define AA_CTRL_CONE_TAN     Q12(0.16f) /* controller: ~9deg magnetic auto-aim cone, scaled by distance */

s32 Pc_AimAssistFind(const VECTOR3* camPos, const VECTOR3* camFwd, s32 aimRange, VECTOR3* outAimPoint)
{
    s32     bestIdx  = NO_VALUE;
    s32     bestK    = 0x7FFFFFFF;
    s32     bestPerp = 0x7FFFFFFF;
    VECTOR3 bestPt   = { 0, 0, 0 };
    int     isPad    = (g_PcAimDevice == 1);
    s64     fxz2;
    s32     i;

    /* |camFwd_xz|^2 (Q24). Camera pitch is clamped well short of vertical, so
     * this is never near zero, but guard the actual divisor (fxz2 >> 12) so a
     * near-vertical chest-anchored forward can't trigger an integer div-by-zero
     * (x86 idiv SIGFPE) below. */
    fxz2 = (s64)camFwd->vx * camFwd->vx + (s64)camFwd->vz * camFwd->vz;
    if ((fxz2 >> 12) <= 0)
        return NO_VALUE;

    for (i = 0; i < (s32)ARRAY_SIZE(g_SysWork.npcs); i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        s32 cx, cz, dx, dz, radius;
        s32 yA, yB, yLo, yHi, vSlack, rayY, bodyH;
        s32 kmult, qx, qz, perpH, maxPerp;

        if (npc->model.charaId < Chara_AirScreamer || npc->model.charaId >= Chara_LockerDeadBody)
            continue; /* only damageable enemies (mirrors func_8008A3E0) */
        if (npc->collision.state <= CharaCollisionState_Player)
            continue; /* 0 = ignore, 1 = player slot (mirrors func_8005CD38) */
        if (npc->health <= Q12(0.0f))
            continue;

        radius = npc->collision.cylinder.field_2;
        if (radius <= 0)
            continue;

        cx = npc->position.vx + npc->collision.shapeOffsets.box.vx;
        cz = npc->position.vz + npc->collision.shapeOffsets.box.vz;

        dx = cx - camPos->vx;
        dz = cz - camPos->vz;

        /* kmult (Q12) = the multiplier on camFwd putting the ray at its closest
         * horizontal approach to the enemy axis. Since |camFwd| == Q12(1.0),
         * kmult is also the world distance along the ray to that point. */
        kmult = (s32)(((s64)dx * camFwd->vx + (s64)dz * camFwd->vz) / (fxz2 >> 12));
        if (kmult <= 0 || kmult > aimRange)
            continue; /* behind the camera or past aim range */

        qx = camPos->vx + (s32)(((s64)camFwd->vx * kmult) >> 12);
        qz = camPos->vz + (s32)(((s64)camFwd->vz * kmult) >> 12);
        perpH = Math_Vector2MagCalc(cx - qx, cz - qz);

        /* Vertical body span (sign-convention agnostic). */
        yA = npc->position.vy + npc->collision.box.top;
        yB = npc->position.vy + npc->collision.box.height;
        yLo = (yA < yB) ? yA : yB;
        yHi = (yA < yB) ? yB : yA;
        bodyH  = yHi - yLo;
        vSlack = isPad ? (bodyH >> 1) : (bodyH >> 2);

        rayY = camPos->vy + (s32)(((s64)camFwd->vy * kmult) >> 12);
        if (rayY < yLo - vSlack || rayY > yHi + vSlack)
            continue; /* reticle above / below the body */

        if (isPad)
        {
            s32 cone  = (s32)(((s64)kmult * AA_CTRL_CONE_TAN) >> 12);
            s32 floor = (s32)(((s64)radius * AA_CTRL_RADIUS_MUL) >> 12);
            maxPerp = (cone > floor) ? cone : floor;
        }
        else
        {
            maxPerp = (s32)(((s64)radius * AA_MOUSE_RADIUS_MUL) >> 12);
        }
        if (perpH > maxPerp)
            continue; /* reticle not over / near the body */

        /* Nearest qualifying enemy wins (tie-break: most on-target). */
        if (kmult < bestK || (kmult == bestK && perpH < bestPerp))
        {
            VECTOR3 aim;
            aim.vx = cx;
            aim.vy = (rayY < yLo) ? yLo : (rayY > yHi) ? yHi : rayY;
            aim.vz = cz;

            /* Don't aim through walls: reject if level geometry blocks the eye
             * from the aim point. (Char trace excluded — we want the enemy.) */
            if (dx != 0 || dz != 0)
            {
                s_RayTrace occ;
                s32        distToAim = Math_Vector2MagCalc(aim.vx - camPos->vx, aim.vz - camPos->vz);
                if (Ray_TraceQuery(&occ, camPos, &aim) &&
                    occ.hitDistance < distToAim - radius)
                    continue; /* occluded */
            }

            bestIdx  = i;
            bestK    = kmult;
            bestPerp = perpH;
            bestPt   = aim;
        }
    }

    if (bestIdx != NO_VALUE)
        *outAimPoint = bestPt;

    return bestIdx;
}
