/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_msg_voice.h"

#include <stdio.h>

#include "game.h"
#include "lang_pack.h"
#include "pc_config.h"
#include "sh_log.h"
#include "xa_player.h"

extern const char* PcPort_GetGameDataPath(void);

int g_PcMapMsgGameVoiced;

static int s_fileStarted;
static int s_drawnThisFrame;
static int s_framesUndrawn;

void Pc_MsgVoice_Touch(void)
{
    s_drawnThisFrame = 1;
}

void Pc_MsgVoice_OnPage(int msgIdx)
{
    char key[32];
    char path[1024];
    char* c;

    if (g_PcMapMsgGameVoiced || !g_PcConfig.allowLooseFiles)
        return;
    if (!Pc_LangPackMsgKey((int)g_SavegamePtr->mapIdx, msgIdx, key, sizeof(key)))
        return;
    for (c = key; *c != '\0'; c++)
    {
        if (*c == '.')
            *c = '_';
    }
    snprintf(path, sizeof(path), "%s/load/XA/msg_%s.wav", PcPort_GetGameDataPath(), key);
    if (XaPlayer_PlayFile(path))
    {
        s_fileStarted = 1;
        SH_DBG("[MSGVOICE] %s", key);
    }
}

void Pc_MsgVoice_OnEnd(void)
{
    if (s_fileStarted)
    {
        XaPlayer_StopFile();
        s_fileStarted = 0;
    }
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
