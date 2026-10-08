/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_msg_voice.h"

#include <stdio.h>

#include <SDL_timer.h>

#include "game.h"
#include "lang_pack.h"
#include "pc_config.h"
#include "sh_log.h"
#include "xa_player.h"

extern const char* PcPort_GetGameDataPath(void);

int g_PcMapMsgGameVoiced;

static int    s_fileStarted;
static int    s_drawnThisFrame;
static int    s_framesUndrawn;

/* Set only for a page that started its OWN file, so a page riding the previous
 * take is not pinned by it. */
static int    s_holdThisPage;
/* Deadline for the post-clip tail, re-armed every frame the clip is audible so
 * it ends up measured from the LAST audible frame. 0 = not armed. */
static Uint32 s_tailUntilMs;

void Pc_MsgVoice_Touch(void)
{
    s_drawnThisFrame = 1;
}

void Pc_MsgVoice_OnPage(int msgIdx)
{
    char key[32];
    char file[32];
    char path[1024];
    char* c;
    int   played = 0;

    if (g_PcMapMsgGameVoiced)
        return;
    if (!Pc_LangPackMsgKey((int)g_SavegamePtr->mapIdx, msgIdx, key, sizeof(key)))
        return;
    snprintf(file, sizeof(file), "%s", key);
    for (c = file; *c != '\0'; c++)
    {
        if (*c == '.')
            *c = '_';
    }
    snprintf(path, sizeof(path), "%s/load/XA/msg_%s.wav", PcPort_GetGameDataPath(), file);
    if (g_PcConfig.allowLooseFiles && XaPlayer_PlayFile(path))
    {
        s_fileStarted = 1;
        played = 1;
    }

    /* A page with no file of its own rides whatever is still playing, so it must
     * not inherit the hold -- otherwise one take covering a multi-page message
     * would pin every page of it until the take ended. */
    s_holdThisPage = played;
    s_tailUntilMs  = 0;
    /* Once per box page; the launcher's "Last played in game" reads the key. */
    SH_DBG("[MSGBOX] %s%s", key, played ? " (voice file)" : "");
}

void Pc_MsgVoice_OnEnd(void)
{
    if (s_fileStarted)
    {
        XaPlayer_StopFile();
        s_fileStarted = 0;
    }
    s_holdThisPage = 0;
    s_tailUntilMs  = 0;
}

/* The authored ~J timer on these pages was written for a line nobody speaks, so
 * it is routinely shorter than a recording of it -- the text moved on and the
 * clip was cut off mid-word. Hold the page while its own clip is audible, then
 * for a short tail so the words are not still hanging when the next line
 * appears. Only the AUTO advance consults this; a manual skip is never gated. */
int Pc_MsgVoice_Holding(void)
{
    extern int Xa_IsVoiceAudioDraining(void);

    if (!s_fileStarted || !s_holdThisPage)
        return 0;

    if (Xa_IsVoiceAudioDraining())
    {
        s_tailUntilMs = SDL_GetTicks() + (Uint32)g_PcConfig.msgVoiceTailMs;
        return 1;
    }

    /* Signed compare so the wrap at ~49 days releases the hold instead of
     * pinning the page for the rest of the session. */
    if (s_tailUntilMs != 0 && (Sint32)(s_tailUntilMs - SDL_GetTicks()) > 0)
        return 1;

    s_tailUntilMs = 0;
    return 0;
}

/* Some boxes close without reaching the draw's end path (auto-close codes,
 * cutscene skips): a file still going after the draw stops being called for
 * a couple of frames is stopped here. Frame-counted, not clocked, so a
 * paused game (no updates) holds instead of cutting. */
void Pc_MsgVoice_Update(void)
{
    if (s_drawnThisFrame)
    {
        s_framesUndrawn = 0;
    }
    else if (s_fileStarted && ++s_framesUndrawn > 2)
    {
        Pc_MsgVoice_OnEnd();
    }
    s_drawnThisFrame = 0;
}
