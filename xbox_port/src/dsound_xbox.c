/*
 * dsound_xbox.c - RXDK DirectSound hardware-audio sink for the Silent Hill
 * Xbox port.
 *
 * Brings up the Xbox APU via RXDK's DirectSound (linked from the prebuilt XDK
 * objects in xbox_port/src/dsound_objs, with the stdcall<->cdecl bridge in
 * dsound_bridge.c and the x87 CRT intrinsics in msvc_compat.c), creates a
 * looping 16-bit stereo PCM secondary buffer played by the hardware, and a
 * per-frame pump that refills the buffer ahead of the play cursor from the
 * software SPU mixer (audio_xbox.c, Audio_RenderInto6).
 *
 * Because the APU plays the looping buffers in hardware and the pump runs on
 * the main thread (driven from VSync), there is NO audio interrupt/DPC — all
 * SPU voice state and the float ADPCM mixing stay on the main thread.
 *
 * 5.1 SURROUND (Duke3D-Xbox blueprint): when the dashboard's audio setting has
 * the Surround flag, three lockstep looping buffers are created, identical in
 * every way except their creation-time mixbin routing:
 *   s_buf     ch0/ch1 -> FRONT_LEFT / FRONT_RIGHT   (dry game stereo)
 *   s_bufRear ch0/ch1 -> BACK_LEFT  / BACK_RIGHT    (Hafler difference feed)
 *   s_bufCen  ch0/ch1 -> FRONT_CENTER / LOW_FREQUENCY (mono-XA voice, LPF sum)
 * One write cursor; the FRONT buffer's play cursor is the master clock; all
 * three are locked at the same offset/length (sizes match, so the wrap regions
 * split identically) and filled by one Audio_RenderInto6 pass. Analog users
 * get the GP's automatic stereo fold-down of all mixbins. A stereo dashboard
 * (or any creation failure) runs the original single-buffer path untouched.
 */
#define NOD3D 1                  /* xtl.h: skip the D3D half */
#include <xboxkrnl/xboxkrnl.h>   /* KeStallExecutionProcessor, RtlInitializeCriticalSection, ExQueryNonVolatileSetting */
#include <dsound.h>              /* RXDK DirectSound (pulls in the xtl.h shim) */
#include <string.h>

#include "sh_log.h"

/* The software SPU mixer (audio_xbox.c): one pass fills front + optional
 * rear/center-LFE pairs. rear/cenLfe NULL = exact stereo behavior. */
extern void Audio_RenderInto6(short* out, short* rear, short* cenLfe, int frames);

/* XA stream decoder (xa_xbox.c): tops up the decoded-PCM ring (~400 ms ahead)
 * before the mixer pulls from it. Main-thread, cheap no-op when idle. */
extern void Xa_XboxPump(void);

#define DS_OUT_HZ       48000
#define DS_BLOCK_ALIGN  4                  /* 16-bit stereo */
#define DS_BUFFER_SIZE  65536              /* ~340 ms ring — margin for BIN-load stutters
                                            * (pump is frame-tied; long CD reads can hitch).
                                            * ALL THREE surround buffers must share this
                                            * size — the lockstep Lock depends on it. */

static IDirectSound*       s_ds      = NULL;
static IDirectSoundBuffer* s_buf     = NULL;   /* front L/R — always exists   */
static IDirectSoundBuffer* s_bufRear = NULL;   /* back L/R — surround only    */
static IDirectSoundBuffer* s_bufCen  = NULL;   /* center+LFE — surround only  */
static DWORD               s_write   = 0;      /* one running write offset    */
static int                 s_up      = 0;
static int                 s_surround = 0;

/* Fill [0,DS_BUFFER_SIZE) of every ring with one render (prime + clear). */
static void PrimeBuffer(void)
{
    LPVOID p1 = NULL, r1 = NULL, c1 = NULL;
    DWORD  b1 = 0, rb1 = 0, cb1 = 0;

    if (FAILED(IDirectSoundBuffer_Lock(s_buf, 0, DS_BUFFER_SIZE,
                                       &p1, &b1, NULL, NULL, DSBLOCK_ENTIREBUFFER)))
        return;
    if (s_surround) {
        IDirectSoundBuffer_Lock(s_bufRear, 0, DS_BUFFER_SIZE, &r1, &rb1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
        IDirectSoundBuffer_Lock(s_bufCen,  0, DS_BUFFER_SIZE, &c1, &cb1, NULL, NULL, DSBLOCK_ENTIREBUFFER);
    }
    Audio_RenderInto6((short*)p1, (short*)r1, (short*)c1, (int)(b1 / DS_BLOCK_ALIGN));
    if (r1) IDirectSoundBuffer_Unlock(s_bufRear, r1, rb1, NULL, 0);
    if (c1) IDirectSoundBuffer_Unlock(s_bufCen,  c1, cb1, NULL, 0);
    IDirectSoundBuffer_Unlock(s_buf, p1, b1, NULL, 0);
    s_write = b1 % DS_BUFFER_SIZE;
}

static void ClearRing(IDirectSoundBuffer* b)
{
    LPVOID p1 = NULL; DWORD b1 = 0;
    if (b && SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, DS_BUFFER_SIZE,
                                               &p1, &b1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
        memset(p1, 0, b1);
        IDirectSoundBuffer_Unlock(b, p1, b1, NULL, 0);
    }
}

void Audio_XboxInit(void)
{
    HRESULT      hr;
    DSBUFFERDESC dsbd;
    WAVEFORMATEX wfx;
    DWORD        audioFlags = 0;
    /* Mixbin routings. Must stay in scope through CreateSoundBuffer —
     * dsbd.lpMixBins points at them. */
    DSMIXBINVOLUMEPAIR frontPairs[2], rearPairs[2], cenPairs[2];
    DSMIXBINS          frontBins, rearBins, cenBins;

    /* Reset the APU before DirectSoundCreate — the kernel leaves it
     * half-configured at boot and RXDK expects to own it from scratch. */
    {
        volatile unsigned long* apu = (volatile unsigned long*)0xFE800000u;
        apu[0x1004 / 4] = 0;            /* NV_PAPU_IEN  = 0 (disable APU IRQs)  */
        apu[0x2000 / 4] = 0;            /* NV_PAPU_SECTL = 0 (disable XCNTMODE)  */
        apu[0x1100 / 4] = 0;            /* NV_PAPU_FECTL = 0 (halt front-end)    */
        apu[0x1000 / 4] = 0xFFFFFFFF;   /* NV_PAPU_ISTS  = write-1-to-clear all  */
        KeStallExecutionProcessor(100);
    }

    /* dsound's own global critical section is normally set up by DLL startup,
     * which never runs when we link the objects directly. (globals.obj) */
    {
        extern CRITICAL_SECTION g_DirectSoundCriticalSection;
        RtlInitializeCriticalSection(&g_DirectSoundCriticalSection);
    }

    /* Dashboard audio mode from the EEPROM (XC_AUDIO=9). Surround only when
     * the user asked for it there; any failure = stereo. */
    {
        ULONG type = 0, val = 0, len = 0;
        if (ExQueryNonVolatileSetting(9 /* XC_AUDIO */, &type, &val, sizeof(val), &len) >= 0)
            audioFlags = (DWORD)val;
        s_surround = (audioFlags & XC_AUDIO_FLAGS_SURROUND) != 0;
        SH_DBG("[SH_AUDIO] eeprom audio flags=0x%08x -> %s%s", (unsigned)audioFlags,
               s_surround ? "surround" : "stereo",
               (audioFlags & XC_AUDIO_FLAGS_ENABLE_AC3) ? "+AC3" : "");
    }

    /* Speaker config BEFORE DirectSoundCreate — it decides which GP/EP DSP
     * images (incl. the Dolby Digital encoder) get loaded. The API call alone
     * SILENTLY DROPS the AC3 bit, so also write the RXDK globals directly
     * (the Duke3D-proven trap). */
    if (s_surround) {
        extern DWORD g_dwDirectSoundOverrideSpeakerConfig, g_dwDirectSoundSpeakerConfig;
        DWORD cfg = DSSPEAKER_SURROUND |
                    ((audioFlags & XC_AUDIO_FLAGS_ENABLE_AC3) ? DSSPEAKER_ENABLE_AC3 : 0);
        DirectSoundOverrideSpeakerConfig(cfg);
        g_dwDirectSoundOverrideSpeakerConfig = cfg;
        g_dwDirectSoundSpeakerConfig         = cfg;
    }

    hr = DirectSoundCreate(NULL, &s_ds, NULL);
    if (FAILED(hr)) {
        SH_DBG("[SH_AUDIO] DirectSoundCreate FAILED hr=0x%08x", (unsigned)hr);
        return;
    }

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = DS_OUT_HZ;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = DS_BLOCK_ALIGN;
    wfx.nAvgBytesPerSec = DS_OUT_HZ * DS_BLOCK_ALIGN;

    frontPairs[0].dwMixBin = DSMIXBIN_FRONT_LEFT;  frontPairs[0].lVolume = 0;
    frontPairs[1].dwMixBin = DSMIXBIN_FRONT_RIGHT; frontPairs[1].lVolume = 0;
    frontBins.dwMixBinCount       = 2;
    frontBins.lpMixBinVolumePairs = frontPairs;

    memset(&dsbd, 0, sizeof(dsbd));
    dsbd.dwSize        = sizeof(dsbd);
    dsbd.dwFlags       = 0;                 /* 2D buffer */
    dsbd.dwBufferBytes = DS_BUFFER_SIZE;
    dsbd.lpwfxFormat   = &wfx;
    dsbd.lpMixBins     = &frontBins;
    dsbd.dwInputMixBin = 0;

    hr = IDirectSound_CreateSoundBuffer(s_ds, &dsbd, &s_buf, NULL);
    if (FAILED(hr)) {
        SH_DBG("[SH_AUDIO] CreateSoundBuffer FAILED hr=0x%08x", (unsigned)hr);
        IDirectSound_Release(s_ds);
        s_ds = NULL;
        return;
    }

    /* Surround companions: identical buffers, different mixbins. Failure is
     * NON-FATAL — release and fall back to plain stereo (Duke-proven). */
    if (s_surround) {
        rearPairs[0].dwMixBin = DSMIXBIN_BACK_LEFT;     rearPairs[0].lVolume = 0;
        rearPairs[1].dwMixBin = DSMIXBIN_BACK_RIGHT;    rearPairs[1].lVolume = 0;
        rearBins.dwMixBinCount       = 2;
        rearBins.lpMixBinVolumePairs = rearPairs;
        cenPairs[0].dwMixBin = DSMIXBIN_FRONT_CENTER;   cenPairs[0].lVolume = 0;
        cenPairs[1].dwMixBin = DSMIXBIN_LOW_FREQUENCY;  cenPairs[1].lVolume = 0;
        cenBins.dwMixBinCount       = 2;
        cenBins.lpMixBinVolumePairs = cenPairs;

        dsbd.lpMixBins = &rearBins;
        hr = IDirectSound_CreateSoundBuffer(s_ds, &dsbd, &s_bufRear, NULL);
        if (SUCCEEDED(hr)) {
            dsbd.lpMixBins = &cenBins;
            hr = IDirectSound_CreateSoundBuffer(s_ds, &dsbd, &s_bufCen, NULL);
        }
        if (FAILED(hr)) {
            SH_DBG("[SH_AUDIO] surround buffers FAILED hr=0x%08x — stereo fallback", (unsigned)hr);
            if (s_bufRear) { IDirectSoundBuffer_Release(s_bufRear); s_bufRear = NULL; }
            if (s_bufCen)  { IDirectSoundBuffer_Release(s_bufCen);  s_bufCen  = NULL; }
            s_surround = 0;
        }
    }

    /* Remove the XDK's default 6dB per-buffer headroom (the AC3/EP pipeline
     * already runs quiet — Duke resorted to a 4x software boost without this). */
    IDirectSoundBuffer_SetHeadroom(s_buf, 0);
    if (s_bufRear) IDirectSoundBuffer_SetHeadroom(s_bufRear, 0);
    if (s_bufCen)  IDirectSoundBuffer_SetHeadroom(s_bufCen,  0);

    /* Clear, then prime, then loop. */
    ClearRing(s_buf);
    ClearRing(s_bufRear);
    ClearRing(s_bufCen);
    PrimeBuffer();

    hr = IDirectSoundBuffer_Play(s_buf, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        SH_DBG("[SH_AUDIO] Play FAILED hr=0x%08x", (unsigned)hr);
        return;
    }
    if (s_bufRear) IDirectSoundBuffer_Play(s_bufRear, 0, 0, DSBPLAY_LOOPING);
    if (s_bufCen)  IDirectSoundBuffer_Play(s_bufCen,  0, 0, DSBPLAY_LOOPING);

    s_up = 1;
    SH_DBG("[SH_AUDIO] DirectSound up (%dHz %s, %d-byte ring)",
           DS_OUT_HZ, s_surround ? "5.1" : "stereo", DS_BUFFER_SIZE);
}

/* Refill the rings ahead of the hardware play cursor, then advance the APU
 * frame pipeline. Call once per rendered frame (from VSync). The FRONT
 * buffer's play cursor is the master clock; the surround buffers are locked
 * at the same offset/length (identical sizes -> identical wrap splits). */
void Audio_XboxPump(void)
{
    DWORD  play = 0, write = 0, avail;
    LPVOID p1 = NULL, p2 = NULL, r1 = NULL, r2 = NULL, c1 = NULL, c2 = NULL;
    DWORD  b1 = 0, b2 = 0, rb1 = 0, rb2 = 0, cb1 = 0, cb2 = 0;

    if (!s_up || !s_buf)
        return;

    Xa_XboxPump();   /* decode XA ahead of the render below (also pumped from
                      * XaPlayer_Update in MainLoop; both are main-thread) */

    if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(s_buf, &play, &write))) {
        avail = (s_write <= play) ? (play - s_write)
                                  : (DS_BUFFER_SIZE - s_write + play);
        if (avail >= 1024) {
            avail -= 512;                       /* guard gap ahead of the DAC */
            avail &= ~(DWORD)(DS_BLOCK_ALIGN - 1);
            if (SUCCEEDED(IDirectSoundBuffer_Lock(s_buf, s_write, avail,
                                                  &p1, &b1, &p2, &b2, 0))) {
                if (s_surround) {
                    IDirectSoundBuffer_Lock(s_bufRear, s_write, avail, &r1, &rb1, &r2, &rb2, 0);
                    IDirectSoundBuffer_Lock(s_bufCen,  s_write, avail, &c1, &cb1, &c2, &cb2, 0);
                }
                if (p1 && b1) Audio_RenderInto6((short*)p1, (short*)r1, (short*)c1, (int)(b1 / DS_BLOCK_ALIGN));
                if (p2 && b2) Audio_RenderInto6((short*)p2, (short*)r2, (short*)c2, (int)(b2 / DS_BLOCK_ALIGN));
                if (r1 || r2) IDirectSoundBuffer_Unlock(s_bufRear, r1, rb1, r2, rb2);
                if (c1 || c2) IDirectSoundBuffer_Unlock(s_bufCen,  c1, cb1, c2, cb2);
                IDirectSoundBuffer_Unlock(s_buf, p1, b1, p2, b2);
                s_write = (s_write + b1 + b2) % DS_BUFFER_SIZE;
            }
        }
    }

    /* REQUIRED on Xbox: drives the VP->GP->EP hardware frame pipeline. */
    DirectSoundDoWork();
}
