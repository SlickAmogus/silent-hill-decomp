/*
 * dsound_xbox.c - RXDK DirectSound hardware-audio sink for the Silent Hill
 * Xbox port.
 *
 * Brings up the Xbox APU via RXDK's DirectSound (linked from the prebuilt XDK
 * objects in xbox_port/src/dsound_objs, with the stdcall<->cdecl bridge in
 * dsound_bridge.c and the x87 CRT intrinsics in msvc_compat.c), creates a
 * looping 16-bit stereo PCM secondary buffer played by the hardware, and a
 * per-frame pump that refills the buffer ahead of the play cursor from the
 * software SPU mixer (audio_xbox.c, Audio_RenderInto).
 *
 * Because the APU plays the looping buffer in hardware and the pump runs on the
 * main thread (driven from VSync), there is NO audio interrupt/DPC — all SPU
 * voice state and the float ADPCM mixing stay on the main thread.
 *
 * STEREO for now. The 22 linked objects already include the 5.1/HRTF/I3DL2/AC3
 * encoder pieces, so surround is a later config change (center/surround buffers
 * + DSSPEAKER_ENABLE_AC3|DSSPEAKER_SURROUND before DirectSoundCreate), not a
 * re-architecture — see the SH_SURROUND seam below.
 */
#define NOD3D 1                  /* xtl.h: skip the D3D half */
#include <xboxkrnl/xboxkrnl.h>   /* KeStallExecutionProcessor, RtlInitializeCriticalSection */
#include <dsound.h>              /* RXDK DirectSound (pulls in the xtl.h shim) */
#include <string.h>

#include "sh_log.h"

/* The software SPU mixer (audio_xbox.c) fills `frames` stereo 16-bit samples. */
extern void Audio_RenderInto(short* out, int frames);

#define DS_OUT_HZ       48000
#define DS_BLOCK_ALIGN  4                  /* 16-bit stereo */
#define DS_BUFFER_SIZE  32768              /* ~170 ms ring (matches the Duke3D driver) */

static IDirectSound*       s_ds    = NULL;
static IDirectSoundBuffer* s_buf   = NULL;
static DWORD               s_write = 0;    /* our running write offset into the ring */
static int                 s_up    = 0;

/* Fill [0,DS_BUFFER_SIZE) of the ring with one render (used to prime + clear). */
static void PrimeBuffer(void)
{
    LPVOID p1 = NULL;
    DWORD  b1 = 0;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(s_buf, 0, DS_BUFFER_SIZE,
                                          &p1, &b1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
        Audio_RenderInto((short*)p1, (int)(b1 / DS_BLOCK_ALIGN));
        IDirectSoundBuffer_Unlock(s_buf, p1, b1, NULL, 0);
        s_write = b1 % DS_BUFFER_SIZE;
    }
}

void Audio_XboxInit(void)
{
    HRESULT      hr;
    DSBUFFERDESC dsbd;
    WAVEFORMATEX wfx;
    /* Explicit front FL/FR routing (matches the proven Duke3D path). Must stay
     * in scope through CreateSoundBuffer — dsbd.lpMixBins points at it. */
    DSMIXBINVOLUMEPAIR frontPairs[2];
    DSMIXBINS          frontBins;

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

    /* Clear, then prime, then loop. */
    {
        LPVOID p1 = NULL; DWORD b1 = 0;
        if (SUCCEEDED(IDirectSoundBuffer_Lock(s_buf, 0, DS_BUFFER_SIZE,
                                              &p1, &b1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
            memset(p1, 0, b1);
            IDirectSoundBuffer_Unlock(s_buf, p1, b1, NULL, 0);
        }
    }
    PrimeBuffer();

    hr = IDirectSoundBuffer_Play(s_buf, 0, 0, DSBPLAY_LOOPING);
    if (FAILED(hr)) {
        SH_DBG("[SH_AUDIO] Play FAILED hr=0x%08x", (unsigned)hr);
        return;
    }

    s_up = 1;
    SH_DBG("[SH_AUDIO] DirectSound up (%dHz stereo, %d-byte ring)", DS_OUT_HZ, DS_BUFFER_SIZE);
}

/* Refill the ring ahead of the hardware play cursor, then advance the APU frame
 * pipeline. Call once per rendered frame (from VSync). */
void Audio_XboxPump(void)
{
    DWORD  play = 0, write = 0, avail;
    LPVOID p1 = NULL, p2 = NULL;
    DWORD  b1 = 0, b2 = 0;

    if (!s_up || !s_buf)
        return;

    if (SUCCEEDED(IDirectSoundBuffer_GetCurrentPosition(s_buf, &play, &write))) {
        avail = (s_write <= play) ? (play - s_write)
                                  : (DS_BUFFER_SIZE - s_write + play);
        if (avail >= 1024) {
            avail -= 512;                       /* guard gap ahead of the DAC */
            avail &= ~(DWORD)(DS_BLOCK_ALIGN - 1);
            if (SUCCEEDED(IDirectSoundBuffer_Lock(s_buf, s_write, avail,
                                                  &p1, &b1, &p2, &b2, 0))) {
                if (p1 && b1) Audio_RenderInto((short*)p1, (int)(b1 / DS_BLOCK_ALIGN));
                if (p2 && b2) Audio_RenderInto((short*)p2, (int)(b2 / DS_BLOCK_ALIGN));
                IDirectSoundBuffer_Unlock(s_buf, p1, b1, p2, b2);
                s_write = (s_write + b1 + b2) % DS_BUFFER_SIZE;
            }
        }
    }

    /* REQUIRED on Xbox: drives the VP->GP->EP hardware frame pipeline. */
    DirectSoundDoWork();
}
