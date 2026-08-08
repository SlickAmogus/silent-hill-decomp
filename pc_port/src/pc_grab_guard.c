/* See pc_grab_guard.h for why a foreign monster's grab wedges the player. */

#include "game.h"
#include "bodyprog/anim.h"
#include "bodyprog/bodyprog.h"
#include "maps/characters/harry.h"

#include "pc_grab_guard.h"

/* Mirrors func_8007FB94's scan, with one deliberate difference: it stops at the
 * table's explicit zero terminator instead of running the engine's fixed 40.
 * Real tables hold between 1 (map1_s04) and 21 (map0_s01) rows, so the vanilla
 * loop reads well past the end — a "match" out there is whatever the overlay
 * happens to store next. Refusing the grab on garbage is the safe direction. */
static int GrabGuard_MapHasAnimRow(s32 animStatus)
{
    const s_UnkStruct3_Mo* rows = g_MapOverlayHdr.field_38;
    int                    i;

    if (rows == NULL)
    {
        return 0;
    }

    for (i = 0; i < 40; i++)
    {
        /* Every shipped table's live rows carry a non-zero packed status, and
         * every table ends with an all-zero row. */
        if (rows[i].status == 0 && rows[i].status_2 == 0)
        {
            break;
        }

        if (rows[i].status_2 == animStatus)
        {
            return 1;
        }
    }

    return 0;
}

int Pc_GrabGuard_GrabIsPlayable(int attackReceived, int isFrontal)
{
    s32 grabAnim;
    s32 releaseAnim;

    /* Both halves come from the state machine's own tables: the grab anim from
     * the grab state's case in Player_Update, the release anim from the state it
     * hands over to. Keep them in step with player_control.c if those change. */
    switch (attackReceived)
    {
        case 45:
        case 56: /* Torso grab. */
            grabAnim    = isFrontal ? ANIM_STATUS(HarryAnim_Unk115, false)
                                    : ANIM_STATUS(HarryAnim_Unk117, false);
            releaseAnim = isFrontal ? ANIM_STATUS(HarryAnim_Unk120, false)
                                    : ANIM_STATUS(HarryAnim_Unk122, false);
            break;

        case 49: /* Leg grab — Grey Child, Mumbler, Larval Stalker. */
            grabAnim    = isFrontal ? ANIM_STATUS(HarryAnim_Unk117, true)
                                    : ANIM_STATUS(HarryAnim_Unk118, false);
            releaseAnim = isFrontal ? ANIM_STATUS(HarryAnim_Unk122, true)
                                    : ANIM_STATUS(HarryAnim_Unk123, false);
            break;

        case 66: /* Neck grab — Cybil boss. Both sides share the grab anim. */
            grabAnim    = ANIM_STATUS(HarryAnim_Unk125, true);
            releaseAnim = isFrontal ? ANIM_STATUS(HarryAnim_Unk120, false)
                                    : ANIM_STATUS(HarryAnim_Unk122, false);
            break;

        /* Romper pin. Four links per side instead of two, and EVERY one has its
         * own exact-equality gate, so a hole anywhere in the chain wedges: the
         * *Start state has no mash timer at all (its only exit is the keyframe
         * gate), and Unk43/Unk44 gate the final return to PlayerState_None. A
         * map that hosts Rompers carries all four or vanilla would hang there
         * too, so requiring the set cannot cost a legitimate pin. */
        case 54:
            if (isFrontal)
            {
                return GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk127, true))  /* PinnedFrontStart */
                    && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk128, false)) /* PinnedFront      */
                    && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk129, true))  /* ReleasePinnedFront */
                    && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk130, true)); /* Unk43            */
            }
            return GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk128, true))   /* PinnedBackStart  */
                && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk129, false))  /* PinnedBack       */
                && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk130, false))  /* ReleasePinnedBack */
                && GrabGuard_MapHasAnimRow(ANIM_STATUS(HarryAnim_Unk131, false)); /* Unk44            */

        default:
            return 1;
    }

    return GrabGuard_MapHasAnimRow(grabAnim) && GrabGuard_MapHasAnimRow(releaseAnim);
}

int Pc_GrabGuard_DamageFallbackIsPlayable(int isFrontal)
{
    /* player_control.c: DamageTorsoFront is ANIM_STATUS(105, false),
     * DamageTorsoBack is ANIM_STATUS(105, true). */
    return GrabGuard_MapHasAnimRow(isFrontal ? ANIM_STATUS(105, false)
                                             : ANIM_STATUS(105, true));
}
