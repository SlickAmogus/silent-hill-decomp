/*
 * split_audio_xbox.c - who hears a sound in split screen.
 *
 * The game positions a sound against one listener: its pan (and the 5.1
 * azimuth) from the camera, its loudness from the distance to the player. With
 * two players that listener is whichever player is nearer the sound -- the rule
 * the Duke port settled on -- so a monster attacking Player 2 on the far side of
 * the map is heard from Player 2's camera instead of being silent. A player's
 * own sounds (footsteps, shots, cries: emitted at &g_SysWork.playerWork.player
 * .position while that player is in the slot) always belong to that player.
 *
 * Music, voices, movies and menu sounds are single streams and need nothing.
 *
 * split_audio = 1 squeezes each player's sounds into their own half of the
 * stereo field (Player 1 left, Player 2 right), for the side-by-side layout.
 *
 * Everything here is inert unless two views are on screen.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/view/vw_system.h"
#include "pc_config.h"
#include "split_xbox.h"
#include "split_internal_xbox.h"
#include "sh_log.h"

#include <string.h>

typedef struct
{
    VECTOR3 camPos;
    MATRIX  camMat;
    int     valid;
} SplitListener;

static SplitListener s_lis[2];

/* A looping emitter about as far from both players would flap between them
 * every frame; remember the last pick per emitter and only hand it over when the
 * other player is clearly nearer. Keyed by the position pointer and the position,
 * since map code reuses stack vectors. */
#define SA_CACHE 16
typedef struct
{
    const VECTOR3* ptr;
    VECTOR3        pos;
    int            pick;
    unsigned       frame;
} SaCache;

static SaCache  s_cache[SA_CACHE];
static unsigned s_frame;

void SplitAudio_Reset(void)
{
    memset(s_lis, 0, sizeof(s_lis));
    memset(s_cache, 0, sizeof(s_cache));
}

void SplitAudio_FrameTick(void)
{
    s_frame++;
}

int SplitAudio_Active(void)
{
    return g_XbSplitTwoViews && s_lis[0].valid && s_lis[1].valid;
}

/* After player k's camera is final for the frame (it is the published one). */
void SplitAudio_ListenerCapture(int k)
{
    if (!g_XbSplitActive)
        return;
    vwGetViewPosition(&s_lis[k & 1].camPos);
    Vw_CoordHierarchyMatrixCompute(vwGetViewCoord(), &s_lis[k & 1].camMat);
    s_lis[k & 1].valid = 1;
}

static s64 Sa_Dist2(const VECTOR3* a, const VECTOR3* b)
{
    s64 dx = (s64)(Q12_TO_Q6(a->vx) - Q12_TO_Q6(b->vx));
    s64 dz = (s64)(Q12_TO_Q6(a->vz) - Q12_TO_Q6(b->vz));
    return dx * dx + dz * dz;
}

int SplitAudio_Pick(const VECTOR3* pos)
{
    s64      d[2];
    int      near, i, slot = -1, oldest = 0;
    unsigned oldestAge = 0;

    if (pos == &g_SysWork.playerWork.player.position)
        return Split_WorkOwner();
    if (!Split_P2InWorld())
        return 0;

    d[0] = Sa_Dist2(pos, &Split_PlayerChara(0)->position);
    d[1] = Sa_Dist2(pos, &Split_PlayerChara(1)->position);
    near = d[1] < d[0];

    for (i = 0; i < SA_CACHE; i++)
    {
        SaCache* c = &s_cache[i];

        if (c->ptr == pos && s_frame - c->frame <= 30 &&
            ABS(c->pos.vx - pos->vx) <= Q12(1.0f) && ABS(c->pos.vz - pos->vz) <= Q12(1.0f))
        {
            slot = i;
            break;
        }
        if (s_frame - c->frame >= oldestAge)
        {
            oldestAge = s_frame - c->frame;
            oldest    = i;
        }
    }
    if (slot >= 0)
    {
        int cur = s_cache[slot].pick;

        /* ~13% nearer before taking a sound over. */
        if (cur != near && !(d[near] * 4 < d[cur] * 3))
            near = cur;
    }
    else
    {
        slot = oldest;
    }
    s_cache[slot].ptr   = pos;
    s_cache[slot].pos   = *pos;
    s_cache[slot].pick  = near;
    s_cache[slot].frame = s_frame;
    return near;
}

const VECTOR3* SplitAudio_CamPos(int k)
{
    return &s_lis[k & 1].camPos;
}

const MATRIX* SplitAudio_CamMat(int k)
{
    return &s_lis[k & 1].camMat;
}

VECTOR3* SplitAudio_PlayerPos(int k)
{
    return &Split_PlayerChara(k & 1)->position;
}

/* The balance a sound gets once its listener is known: unchanged when shared,
 * squeezed into that player's half of the field in Left/Right mode (where the
 * 5.1 azimuth is dropped too, so the voice follows the balance and not the
 * four-speaker circle). */
s32 SplitAudio_Balance(int k, s32 balance)
{
    extern s32 g_Pc_SfxAzimuthValid;

    if (g_PcConfig.splitAudio != 1)
        return balance;
    g_Pc_SfxAzimuthValid = 0;
    balance = (k ? 80 : -80) + (balance * 47) / 127;
    return CLAMP(balance, -127, 127);
}
