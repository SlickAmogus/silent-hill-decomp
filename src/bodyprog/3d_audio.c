#include "game.h"
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>
#include <psyq/libapi.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/sound/sound_system.h"
#ifdef SH_PC_PORT
#include "sh_log.h"
#endif

VECTOR3 D_800C42C0;
VECTOR3* D_800C42CC;

// ========================================
// 3D SOUND
// ========================================

s32 func_8005D86C(s32 arg0) // 0x8005D86C
{
    s32 var_a0;
    s32 var_v1;
    s32 temp_a1;
    s32 temp_a2;
    s32 temp_a3;
    s32 temp;

    temp    = Q12_FRACT(arg0);
    temp_a1 = FP_FROM(arg0, Q12_SHIFT);

    if (temp_a1 >= 12)
    {
        return 0;
    }
    if (temp_a1 < -20)
    {
        return INT_MAX;
    }

    temp_a2 = (arg0 & 0x7F) << 5;
    temp_a3 = temp >> 7;

    var_a0 = D_800A9F64[temp_a3];
    if (temp_a1 > 0)
    {
        var_a0 >>= temp_a1;
    }
    else if (temp_a1 < 0)
    {
        var_a0 <<= -temp_a1;
    }

    if (temp_a2 != 0)
    {
        var_v1 = D_800A9F64[temp_a3 + 1];
        if (temp_a1 > 0)
        {
            var_v1 >>= temp_a1;
        }
        else if (temp_a1 < 0)
        {
            var_v1 <<= -temp_a1;
        }

        var_a0 = Q12_MULT_PRECISE(var_a0, Q12(1.0f) - temp_a2) + Q12_MULT_PRECISE(var_v1, temp_a2);
    }

    return var_a0;
}

s32 func_8005D974(s32 arg0) // 0x8005D974
{
    s32 val;

    val = func_8005D86C(arg0);

    if (val > Q12(4.0f))
    {
        val = Q12(4.0f);
    }
    else if (val < Q12(0.0f))
    {
        val = Q12(0.0f);
    }

    return val;
}

s32 func_8005D9B8(VECTOR3* pos, q23_8 vol) // 0x8005D9B8
{
    s32 temp_v0;
    s32 deltaX;
    s32 deltaY;
    s32 deltaZ;
    s32 var_s0;
    s32 var_v0;

    vwGetViewPosition(&D_800C42C0);
    D_800C42CC = &g_SysWork.playerWork.player.position;

    deltaX = D_800C42C0.vx - D_800C42CC->vx;
    deltaY = D_800C42C0.vy - D_800C42CC->vy;
    deltaZ = D_800C42C0.vz - D_800C42CC->vz;
    var_s0 = func_8005D974((SquareRoot12(Q12_MULT_PRECISE(deltaX, deltaX) +
                                         Q12_MULT_PRECISE(deltaY, deltaY) +
                                         Q12_MULT_PRECISE(deltaZ, deltaZ)) - Q12(2.5f)) / 10);
    if (var_s0 > Q12(1.0f))
    {
        var_s0 = Q12(1.0f);
    }

    deltaX  = D_800C42CC->vx - pos->vx;
    deltaY  = D_800C42CC->vy - pos->vy;
    deltaZ  = D_800C42CC->vz - pos->vz;
    temp_v0 = func_8005D974((SquareRoot12(Q12_MULT_PRECISE(deltaX, deltaX) +
                                          Q12_MULT_PRECISE(deltaY, deltaY) +
                                          Q12_MULT_PRECISE(deltaZ, deltaZ)) - Q12(6.0f)) / 4);

    var_v0 = Q12_MULT_PRECISE(var_s0, temp_v0);
    if (var_v0 > Q12(2.0f))
    {
        var_v0 = Q12(2.0f);
    }
    else if (var_v0 < Q12(0.0f))
    {
        var_v0 = Q12(0.0f);
    }

    var_v0 = Q12_MULT_PRECISE(vol, var_v0);
    if (var_v0 > Q8_CLAMPED(1.0f))
    {
        var_v0 = Q8_CLAMPED(1.0f);
    }
    else if (var_v0 < Q12(0.0f))
    {
        var_v0 = Q12(0.0f);
    }

    return var_v0;
}

void func_8005DC3C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType, s32 pitch);

void func_8005DC1C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType)
{
    func_8005DC3C(sfxId, pos, vol, soundType, 0);
}

void func_8005DC3C(e_SfxId sfxId, const VECTOR3* pos, q23_8 vol, s32 soundType, s32 pitch) // 0x8005DC3C
{
    q23_8 volCpy;
    q23_8 balance;

    /* PC note: an early port stub forced balance=0 here claiming GTE-based
     * Vc_StereoBalanceGet "may not work cleanly" — disproven: the sfx.c
     * aliases of these wrappers call it unguarded from 243 sites. The clean
     * PSX path below restores real stereo balance (and arms the PC azimuth
     * side-channel inside Vc_StereoBalanceGet) for the 116 func_8005D*
     * call sites: NPC cries, footsteps, melee. */

    // Get stereo balance.
    if (soundType & (1 << 0) || g_GameWork.config.soundType)
    {
        balance = 0;
    }
    else
    {
        balance = Vc_StereoBalanceGet(pos);
    }

    // Clamp volume.
    if (vol > Q8_CLAMPED(1.0f))
    {
        vol = Q8_CLAMPED(1.0f);
    }
    else if (vol < Q8_CLAMPED(0.0f))
    {
        vol = Q8_CLAMPED(0.0f);
    }

    if (!(soundType & (1 << 1)))
    {
        volCpy = func_8005D9B8(pos, vol);
    }
    else
    {
        volCpy = vol;
    }

    if (volCpy > Q8_CLAMPED(1.0f))
    {
        volCpy = Q8_CLAMPED(1.0f);
    }

    if (soundType & (1 << 2))
    {
        Sd_SfxAttributesUpdate(sfxId, balance, ~volCpy, pitch);
    }
    else
    {
        Sd_PlaySfx(sfxId, balance, ~volCpy);
    }
}

void func_8005DD44(e_SfxId sfxId, VECTOR3* pos, q23_8 vol, s8 pitch) // 0x8005DD44
{
    q23_8 volCpy;
    s32   balance;

    /* PC note: early port stub (balance=0, no falloff) removed — see
     * func_8005DC3C above. This also restores the func_8005D9B8 distance
     * falloff this path always had on PSX. */

    // Get stereo balance.
    if (g_GameWork.config.soundType)
    {
        balance = 0;
    }
    else
    {
        balance = Vc_StereoBalanceGet(pos);
    }

    // Clamp volume.
    if (vol > Q8_CLAMPED(1.0f))
    {
        vol = Q8_CLAMPED(1.0f);
    }
    else if (vol < Q8_CLAMPED(0.0f))
    {
        vol = Q8_CLAMPED(0.0f);
    }

    volCpy = func_8005D9B8(pos, vol);
    if (volCpy > Q8_CLAMPED(1.0f))
    {
        volCpy = Q8_CLAMPED(1.0f);
    }

    func_80046620(sfxId, balance, ~volCpy, pitch);
}

static inline s32 AttenuationCalc(s32 volume, VECTOR3* pos, q19_12 falloff)
{
    q19_12 dist;

    dist = Math_Vector3MagCalc(g_SysWork.playerWork.player.position.vx - pos->vx,
                               g_SysWork.playerWork.player.position.vy - pos->vy,
                               g_SysWork.playerWork.player.position.vz - pos->vz);
    return (volume * dist) / falloff;
}

#ifdef SH_PC_PORT
/* Looping positional SFX keep their own attenuation alive.
 *
 * Every caller of func_8005DE0C sits inside a proximity or state test, and
 * that test also gates the SD_Call that STARTED the loop. Walk out of it and
 * the per-frame attenuation simply stops being written, so the voice keeps
 * whatever volume it last had and a looping sample plays at that volume for
 * the rest of the room -- the wheelchair squeak heard the length of the alley,
 * with its declared 8-unit falloff never reaching silence. On hardware the
 * frozen voice was usually stolen by the next sound and went quiet by luck,
 * which is why this only showed up once voices stopped being recycled as
 * aggressively.
 *
 * So remember what each positional sfx was last told, and re-apply it on any
 * frame its caller did not: the distance term is recomputed against the
 * player, so leaving the area fades the loop out on its own curve and
 * returning brings it back. Nothing changes while a caller is updating.
 *
 * The radio loops are excluded on purpose: they own reserved voices 22/23 and
 * Sd_SfxAttributesUpdate RESTARTS them when their voice is not keyed on, so
 * sustaining them here would make the radio play forever. */
bool Pc_Sd_SfxHasVoice(u16 sfxId); /* sd_call.c */

#define PC_SFX_SUSTAIN_MAX 16

typedef struct
{
    u16     sfxId;
    u8      used;
    u8      fresh;   /* a caller wrote this sfx since the last sweep */
    VECTOR3 pos;
    s32     vol;
    q19_12  falloff;
    s8      pitch;
} s_PcSfxSustain;

static s_PcSfxSustain s_pcSustain[PC_SFX_SUSTAIN_MAX];
static s32            s_pcSustaining; /* re-entry guard for the sweep */

/* The sweep below calls it before its definition; clang rejects the implicit declaration. */
void func_8005DE0C(e_SfxId sfxId, VECTOR3* pos, s32 vol, q19_12 falloff, s8 pitch);

static void Pc_SfxSustainRecord(e_SfxId sfxId, VECTOR3* pos, s32 vol, q19_12 falloff, s8 pitch)
{
    s32 i;
    s32 free = -1;

    if (s_pcSustaining || pos == NULL)
        return;
    if (sfxId == Sfx_RadioInterferenceLoop || sfxId == Sfx_RadioStaticLoop)
        return;

    for (i = 0; i < PC_SFX_SUSTAIN_MAX; i++)
    {
        if (s_pcSustain[i].used && s_pcSustain[i].sfxId == (u16)sfxId)
            break;
        if (!s_pcSustain[i].used && free < 0)
            free = i;
    }
    if (i >= PC_SFX_SUSTAIN_MAX)
    {
        if (free < 0)
            return;
        i = free;
    }

    s_pcSustain[i].used    = 1;
    s_pcSustain[i].fresh   = 1;
    s_pcSustain[i].sfxId   = (u16)sfxId;
    s_pcSustain[i].pos     = *pos;
    s_pcSustain[i].vol     = vol;
    s_pcSustain[i].falloff = falloff;
    s_pcSustain[i].pitch   = pitch;
}

/* Once per frame, after the map/event code has had its chance to update. */
void Pc_3dAudio_SustainPositionalLoops(void)
{
    s32 i;

    s_pcSustaining = 1;

    for (i = 0; i < PC_SFX_SUSTAIN_MAX; i++)
    {
        if (!s_pcSustain[i].used)
            continue;

        if (!Pc_Sd_SfxHasVoice(s_pcSustain[i].sfxId))
        {
            s_pcSustain[i].used = 0; /* the game stopped it; stop tracking it */
            continue;
        }

        if (!s_pcSustain[i].fresh)
        {
            func_8005DE0C((e_SfxId)s_pcSustain[i].sfxId, &s_pcSustain[i].pos,
                          s_pcSustain[i].vol, s_pcSustain[i].falloff, s_pcSustain[i].pitch);
        }
        s_pcSustain[i].fresh = 0;
    }

    s_pcSustaining = 0;
}

/* Map change: the positions belong to the map that is going away. */
void Pc_3dAudio_SustainReset(void)
{
    s32 i;
    for (i = 0; i < PC_SFX_SUSTAIN_MAX; i++)
        s_pcSustain[i].used = 0;
}
#endif

void func_8005DE0C(e_SfxId sfxId, VECTOR3* pos, s32 vol, q19_12 falloff, s8 pitch)
{
    s32 balance;
    u16 finalVol;
    s32 s3;
    s32 att0;
    u8  att1;
    s32 att2;

    /* PC note: early port stub (balance=0) removed — see func_8005DC3C. */
    if (g_GameWork.config.soundType)
    {
        balance = 0;
    }
    else
    {
        balance = Vc_StereoBalanceGet(pos);
    }

    if (vol > 0xFF)
    {
        vol = 0xFF;
    }

    if (vol < 0)
    {
#ifdef SH_PC_PORT
        /* Balance above armed the azimuth latch but no Sd_* call will claim
         * it on this path — disarm so the next sound can't inherit it. */
        {
            extern s32 g_Pc_SfxAzimuthValid;
            g_Pc_SfxAzimuthValid = 0;
        }
#endif
        return;
    }

#ifdef SH_PC_PORT
    Pc_SfxSustainRecord(sfxId, pos, vol, falloff, pitch);
#endif

    att0 = AttenuationCalc(vol, pos, falloff);
    s3 = vol - 0xFF;
    if ((att0 - s3) >= 0xFF || (AttenuationCalc(vol, pos, falloff) - s3) >= 0)
    {
        att2 = AttenuationCalc(vol, pos, falloff) - s3;
        finalVol = 0xFF;
        if (att2 < 0xFF)
        {
            att1 = AttenuationCalc(vol, pos, falloff) - (vol + 1);
            finalVol = att1;
        }
    }
    else
    {
        finalVol = 0;
    }

#ifdef SH_PC_PORT
    /* Trace radio loop attenuation. The pitch-2210 + addr-set logs show the
     * SPU is being keyed on, but volL=0/volR=0 — narrow that down to whether
     * func_8005DE0C produces 0 finalVol or whether it survives through
     * Sd_SfxAttributesUpdate's volumeLeft_C math. */
    if (sfxId == 1321 || sfxId == 1322 /* Sfx_RadioInterference/StaticLoop */) {
    }
#endif

    Sd_SfxAttributesUpdate(sfxId, balance, finalVol, pitch);
}
