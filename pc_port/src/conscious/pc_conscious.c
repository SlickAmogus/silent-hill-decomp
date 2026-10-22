/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious.c - the grey children at the end of the opening alley talk.
 *
 * Retail: after "What is this?" (map0_s00) every grey child is re-initialised
 * into StalkerControl_2 and hunts Harry down; dying there is how the game
 * moves on to the cafe. Here they wake the same way, at the same moment, but
 * Ai_Stalker_Control_2 asks Pc_Conscious_StalkerCalm() first. While the answer
 * is yes the child walks up, stops a few steps short and stands facing Harry
 * instead of attacking, and the conversation runs:
 *
 *   they announce themselves -> Harry answers (mic on V, or typed chat on Y)
 *   -> one LLM call writes their reply, a second decides what it MEANS:
 *        ATTACK  - the answer flips to no for every child, so the unmodified
 *                  retail AI takes over and they kill him;
 *        RELEASE - they back off and leave him be for grace_seconds, then
 *                  come back at him (the alley has no way out);
 *        TALK    - they wait for his next answer.
 *
 * Patience runs out after patience_seconds or max_turns whatever is said, so
 * the scene always ends the way the game needs it to.
 *
 * Only this file reads game state. Everything outside the scene is untouched:
 * V is Circle and Y does nothing anywhere else.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include <PsyX/PsyX_public.h>

#include "pc_voice_clip.h"
#include "pc_conscious.h"
#include "sh_log.h"

#define NPC_N       ((int)ARRAY_SIZE(g_SysWork.npcs))
#define HIST_MAX    32
#define HIST_SENT   22
#define NPC_NAME    "Grey Child"
#define THINK_LIMIT 45.0f

typedef enum
{
    CS_OFF = 0,
    CS_ARMED,     /* children spawned, still dormant: the opener is being written */
    CS_AWAKE,     /* awake and walking up; speaks as soon as the opener is in */
    CS_SPEAKING,
    CS_LISTEN,
    CS_THINKING,
    CS_RELEASED,
    CS_HOSTILE
} e_ConState;

static int          s_init;
static int          s_state;
static unsigned int s_gen = 1;
static float        s_stateT;
static float        s_totalT;
static int          s_talking;       /* the first line has been said */
static int          s_turns;
static int          s_releases;
static int          s_firstReplyDone;
static int          s_pendingDecision;
static int          s_sttPending;
static int          s_hintShown;

static PcConResult  s_opener;
static int          s_openerReady;

static unsigned int s_clip;
static int          s_speaker = -1;
static float        s_textHold;

static unsigned int s_calmSeenMs[64];
static float        s_holdX[64], s_holdZ[64];

static struct { int assistant; char* text; } s_hist[HIST_MAX];
static int s_histN;

static SDL_Scancode s_scMic  = SDL_SCANCODE_UNKNOWN;
static SDL_Scancode s_scChat = SDL_SCANCODE_UNKNOWN;
static int          s_prevMic, s_prevChat;
static int          s_swallow;
static int          s_micMaskHold;
static unsigned int s_lastMs;

/* ------------------------------------------------------------------ */
/* Prompts                                                             */
/* ------------------------------------------------------------------ */

static const char s_persona[] =
    "You are THE GREY CHILDREN: a little pack of grey, child-sized, knife-carrying creatures "
    "who live at the dead end of a dark, bloody alley in the Otherworld of Silent Hill (1999). "
    "You speak together as one gang, through whichever of you is loudest. "
    "Personality: aggressive, territorial, petty, bratty and easily annoyed -- schoolyard bullies "
    "who happen to be monsters. Darkly funny: you bicker, you brag, you call him names "
    "(\"grown-up\", \"big man\", \"stranger\"), you get offended over nothing. You are NOT polite "
    "and never helpful. You think this alley is YOURS. "
    "The man in front of you is %s, a writer looking for his missing seven-year-old daughter Cheryl. "
    "He is unarmed and holding nothing but a lighter. "
    "Rules: reply with ONE to THREE short sentences, under 40 words, meant to be spoken out loud. "
    "No stage directions, no asterisks, no emojis, no narration, no quotation marks around your line. "
    "Stay in character no matter what he says; you know nothing about video games, AI or the real world. "
    "Questions and demands are your thing: make him justify himself. "
    "If he is rude, threatening, boring, cowardly or makes no sense you get angrier and promise to cut him. "
    "If he is genuinely clever, funny, brave or flatters you well, you MAY decide to let him go -- "
    "but say it plainly (tell him to get out of your alley) and warn him what happens if he stays. "
    "If you decide to attack, say so plainly in your line. Lines in [brackets] describe what is happening; "
    "never repeat the brackets.";

static const char* const s_fallback[] = {
    "Hey! HEY! This is OUR alley, grown-up. Who are you and what do you want?",
    "Speak up, stranger. We are getting bored... and bored makes us hungry.",
    "Nobody comes down here. Nobody! So why are YOU here?",
};

/* ------------------------------------------------------------------ */
/* History                                                             */
/* ------------------------------------------------------------------ */

static void Hist_Clear(void)
{
    int i;
    for (i = 0; i < s_histN; i++)
        free(s_hist[i].text);
    s_histN = 0;
}

static void Hist_Add(int assistant, const char* text)
{
    if (s_histN == HIST_MAX)
    {
        free(s_hist[0].text);
        memmove(&s_hist[0], &s_hist[1], sizeof(s_hist[0]) * (HIST_MAX - 1));
        s_histN--;
    }
    s_hist[s_histN].assistant = assistant;
    s_hist[s_histN].text      = SDL_strdup(text);
    s_histN++;
}

static char* Con_BuildMsgs(void)
{
    char   sys[sizeof(s_persona) + 64];
    size_t cap = sizeof(sys) * 2 + 256;
    char*  out;
    int    i, from;

    for (i = 0; i < s_histN; i++)
        cap += strlen(s_hist[i].text) * 2 + 48;
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    snprintf(sys, sizeof(sys), s_persona, g_PcCon.playerName);
    strcpy(out, "[{\"role\":\"system\",\"content\":\"");
    PcCon_JsonEscapeCat(out, (int)cap, sys);
    strcat(out, "\"}");
    from = s_histN > HIST_SENT ? s_histN - HIST_SENT : 0;
    /* The chat APIs want the first non-system message to be the user's. */
    if (from < s_histN && s_hist[from].assistant)
        from++;
    for (i = from; i < s_histN; i++)
    {
        strcat(out, s_hist[i].assistant ? ",{\"role\":\"assistant\",\"content\":\"" : ",{\"role\":\"user\",\"content\":\"");
        PcCon_JsonEscapeCat(out, (int)cap - 8, s_hist[i].text);
        strcat(out, "\"}");
    }
    strcat(out, "]");
    return out;
}

static char* Con_BuildJudge(void)
{
    size_t cap = 512;
    char*  out;
    int    i, from = s_histN > 12 ? s_histN - 12 : 0;

    for (i = from; i < s_histN; i++)
        cap += strlen(s_hist[i].text) + 24;
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    snprintf(out, cap,
             "Exchanges so far: %d (they lose patience around %d). Times they already let him go: %d.\n"
             "CONVERSATION:\n",
             s_turns, g_PcCon.maxTurns, s_releases);
    for (i = from; i < s_histN; i++)
    {
        strcat(out, s_hist[i].assistant ? "GREY CHILDREN: " : "");
        strcat(out, s_hist[i].text);
        strcat(out, "\n");
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Game state                                                          */
/* ------------------------------------------------------------------ */

/* Only meaningful while gameState is InGame: the inventory, map and options
 * screens are other game states, and opening one mid-conversation must not
 * end it. */
static int Con_InScene(void)
{
    int i;
    if (g_SavegamePtr == NULL || g_SavegamePtr->mapIdx != MapIdx_MAP0_S00)
        return 0;
    for (i = 0; i < NPC_N; i++)
        if (g_SysWork.npcs[i].model.charaId == Chara_GreyChild)
            return 1;
    return 0;
}

static float Con_DistToPlayer(int npc)
{
    const float dx = (float)(g_SysWork.npcs[npc].position.vx - g_SysWork.playerWork.player.position.vx) / 4096.0f;
    const float dz = (float)(g_SysWork.npcs[npc].position.vz - g_SysWork.playerWork.player.position.vz) / 4096.0f;
    return sqrtf(dx * dx + dz * dz);
}

static int Con_Nearest(float* outDist)
{
    int   i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < NPC_N; i++)
    {
        float d;
        if (g_SysWork.npcs[i].model.charaId != Chara_GreyChild || g_SysWork.npcs[i].health <= 0)
            continue;
        d = Con_DistToPlayer(i);
        if (d < bd) { bd = d; best = i; }
    }
    if (outDist) *outDist = bd;
    return best;
}

static float Con_Gain(int npc)
{
    float d;
    if (npc < 0 || npc >= NPC_N || g_SysWork.npcs[npc].model.charaId != Chara_GreyChild)
        return 0.6f;
    d = Con_DistToPlayer(npc);
    if (d <= 3.0f)
        return 1.0f;
    d = 1.0f - (d - 3.0f) / (g_PcCon.hearRange - 3.0f);
    return d < 0.0f ? 0.0f : d;
}

static int Con_Awake(void)
{
    const unsigned int now = SDL_GetTicks();
    int i;
    for (i = 0; i < NPC_N && i < 64; i++)
        if (s_calmSeenMs[i] && now - s_calmSeenMs[i] < 500)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Flow                                                                */
/* ------------------------------------------------------------------ */

static void Con_Enter(int st)
{
    s_state  = st;
    s_stateT = 0.0f;
}

static void Con_Reset(void)
{
    if (s_state != CS_OFF)
        SH_DBG("[CONSCIOUS] scene over");
    Pc_VoiceClip_Stop(s_clip);
    s_clip = 0;
    s_gen++;
    Hist_Clear();
    PcConNet_Free(&s_opener);
    s_openerReady    = 0;
    s_talking        = 0;
    s_turns          = 0;
    s_releases       = 0;
    s_firstReplyDone = 0;
    s_totalT         = 0.0f;
    s_sttPending     = 0;
    s_hintShown      = 0;
    s_textHold       = 0.0f;
    s_speaker        = -1;
    memset(s_calmSeenMs, 0, sizeof(s_calmSeenMs));
    PcConChat_Close();
    PcConMic_SetOn(0);
    Con_Enter(CS_OFF);
}

static void Con_SubmitTurn(const char* userContent, int forced)
{
    Hist_Add(0, userContent);
    PcConNet_SubmitTurn(s_gen, Con_BuildMsgs(), Con_BuildJudge(), forced);
}

/* Patience is a hard limit on top of whatever the judge thinks, so a player
 * who keeps them talking forever still gets the death the game needs. */
static void Con_PlayerTurn(const char* content)
{
    char buf[1024];
    int  out = s_totalT >= g_PcCon.patienceSec || s_turns >= g_PcCon.maxTurns;
    if (out)
    {
        snprintf(buf, sizeof(buf), "%s\n[You have completely run out of patience. This is your last line: "
                                   "tell him he is done and that you are going to get him NOW.]", content);
        Con_SubmitTurn(buf, PCCON_DECIDE_ATTACK);
    }
    else
    {
        Con_SubmitTurn(content, -1);
    }
    Con_Enter(CS_THINKING);
}

static void Con_Hostile(const char* why)
{
    SH_DBG("[CONSCIOUS] hostile (%s)", why);
    PcConChat_Add(PCCHAT_SYSTEM, NULL, "[The grey children attack!]");
    PcConChat_Close();
    Con_Enter(CS_HOSTILE);
}

static void Con_Speak(const char* text, const short* pcm, unsigned int samples, int rate, int decision)
{
    float d;
    s_speaker = Con_Nearest(&d);
    PcConChat_Add(PCCHAT_NPC, NPC_NAME, text);
    Pc_VoiceClip_Stop(s_clip);
    s_clip     = pcm ? Pc_VoiceClip_PlayEx(pcm, samples, rate, 0, Con_Gain(s_speaker), g_PcCon.voicePitch) : 0;
    s_textHold = s_clip ? 0.3f : 1.2f + 0.055f * (float)strlen(text);
    s_pendingDecision = decision;
    s_talking = 1;
    s_turns++;
    SH_DBG("[CONSCIOUS] say (turn %d, decision %d, audio %s): %s", s_turns, decision, s_clip ? "yes" : "no", text);
    Con_Enter(CS_SPEAKING);
}

static void Con_SpeakResult(PcConResult* r, int fallbackIdx, int forced)
{
    if (r && r->ok && r->text)
    {
        Con_Speak(r->text, r->pcm, r->samples, r->rate, r->decision);
        return;
    }
    Hist_Add(1, s_fallback[fallbackIdx]);
    Con_Speak(s_fallback[fallbackIdx], NULL, 0, 0, forced >= 0 ? forced : PCCON_DECIDE_TALK);
}

static int Con_Words(const char* s)
{
    int n = 0, in = 0;
    for (; *s; s++)
    {
        int w = isalnum((unsigned char)*s) || *s == '\'';
        if (w && !in) n++;
        in = w;
    }
    return n;
}

static void Con_PlayerSaid(const char* text, int typed)
{
    char line[512];
    int  need;

    if (!text || !text[0])
        return;
    if (s_state != CS_LISTEN)
    {
        PcConChat_Add(PCCHAT_DIM, g_PcCon.playerName, text);
        return;
    }
    /* Five words before the first answer counts, so a cough, a "hm" or the
     * room talking is not taken as Harry's reply to being cornered. */
    need = typed ? 1 : (s_firstReplyDone ? g_PcCon.minWords : g_PcCon.minWordsFirst);
    if (Con_Words(text) < need)
    {
        snprintf(line, sizeof(line), "%s  (too short, ignored)", text);
        PcConChat_Add(PCCHAT_DIM, g_PcCon.playerName, line);
        return;
    }
    PcConChat_Add(PCCHAT_PLAYER, g_PcCon.playerName, text);
    s_firstReplyDone = 1;
    snprintf(line, sizeof(line), "%s says: \"%s\"", g_PcCon.playerName, text);
    Con_PlayerTurn(line);
}

static void Con_HandleResult(PcConResult* r)
{
    /* Counted down even when stale: a timed-out turn bumps s_gen, and a
     * transcript dropped without this would freeze the listen clock. */
    if (r->kind == PCCON_JOB_STT && s_sttPending > 0)
        s_sttPending--;
    if (r->gen != s_gen)
    {
        PcConNet_Free(r);
        return;
    }
    if (r->kind == PCCON_JOB_STT)
    {
        if (r->ok && r->text && r->text[0])
            Con_PlayerSaid(r->text, 0);
        PcConNet_Free(r);
        return;
    }

    if (r->ok && r->text)
        Hist_Add(1, r->text);

    if (s_state == CS_ARMED || s_state == CS_AWAKE)
    {
        PcConNet_Free(&s_opener);
        s_opener      = *r;
        s_openerReady = 1;
        return;
    }
    if (s_state == CS_THINKING)
    {
        int forced = (s_totalT >= g_PcCon.patienceSec || s_turns >= g_PcCon.maxTurns) ? PCCON_DECIDE_ATTACK : -1;
        Con_SpeakResult(r, 1 + (s_turns & 1), forced);
    }
    PcConNet_Free(r);
}

static void Con_Tick(float dt, int live)
{
    float nearD;

    if (g_GameWork.gameState != GameState_InGame)
        return;
    if (!Con_InScene())
    {
        if (s_state != CS_OFF)
            Con_Reset();
        return;
    }

    if (s_state == CS_OFF)
    {
        int i;
        SH_DBG("[CONSCIOUS] grey children present: writing their opener");
        Con_Enter(CS_ARMED);
        PcConNet_Start();
        Con_SubmitTurn("[A grown-up stranger has wandered into your alley and found the body on the fence. "
                       "You have him cornered. Announce yourselves and demand to know who he is and what "
                       "he thinks he is doing here.]",
                       PCCON_DECIDE_TALK);
        for (i = 0; i < NPC_N && i < 64; i++)
            s_calmSeenMs[i] = 0;
        if (g_PcCon.micStartOn)
            PcConMic_SetOn(1);
    }

    if (s_state != CS_HOSTILE && g_SysWork.playerWork.player.health <= 0)
    {
        Con_Hostile("Harry is down");
        return;
    }

    if (s_clip)
        Pc_VoiceClip_SetGain(s_clip, Con_Gain(s_speaker));

    if (!live)
        return;
    s_stateT += dt;
    if (s_talking && s_state != CS_HOSTILE)
        s_totalT += dt;

    switch (s_state)
    {
    case CS_ARMED:
        if (Con_Awake())
        {
            SH_DBG("[CONSCIOUS] awake, calm");
            Con_Enter(CS_AWAKE);
        }
        break;

    case CS_AWAKE:
        Con_Nearest(&nearD);
        if (s_openerReady && (nearD < 4.5f || s_stateT > 3.0f))
        {
            PcConResult o = s_opener;
            memset(&s_opener, 0, sizeof(s_opener));
            s_openerReady = 0;
            Con_SpeakResult(&o, 0, PCCON_DECIDE_TALK);
            PcConNet_Free(&o);
        }
        else if (!s_openerReady && s_stateT > 10.0f)
        {
            Con_SpeakResult(NULL, 0, PCCON_DECIDE_TALK);
        }
        break;

    case CS_SPEAKING:
        if (s_textHold > 0.0f)
            s_textHold -= dt;
        if (s_textHold <= 0.0f && !Pc_VoiceClip_Playing(s_clip))
        {
            s_clip = 0;
            if (s_pendingDecision == PCCON_DECIDE_ATTACK)
            {
                Con_Hostile("their line meant attack");
            }
            else if (s_pendingDecision == PCCON_DECIDE_RELEASE)
            {
                int i;
                s_releases++;
                /* Back off a step and a half from Harry and stay put, so the
                 * way past them is open while the grace period runs. */
                for (i = 0; i < NPC_N && i < 64; i++)
                {
                    float sx = (float)g_SysWork.npcs[i].position.vx / 4096.0f;
                    float sz = (float)g_SysWork.npcs[i].position.vz / 4096.0f;
                    float px = (float)g_SysWork.playerWork.player.position.vx / 4096.0f;
                    float pz = (float)g_SysWork.playerWork.player.position.vz / 4096.0f;
                    float dx = sx - px, dz = sz - pz;
                    float l  = sqrtf(dx * dx + dz * dz);
                    if (l < 0.01f) l = 0.01f;
                    s_holdX[i] = sx + dx / l * 1.5f;
                    s_holdZ[i] = sz + dz / l * 1.5f;
                }
                PcConChat_Add(PCCHAT_SYSTEM, NULL, "[They are letting you go... for now.]");
                Con_Enter(CS_RELEASED);
            }
            else
            {
                if (!s_hintShown)
                {
                    char hint[128];
                    s_hintShown = 1;
                    snprintf(hint, sizeof(hint), "[Answer them: %s toggles your mic, %s to type.]",
                             g_PcCon.keyMic, g_PcCon.keyChat);
                    PcConChat_Add(PCCHAT_SYSTEM, NULL, hint);
                }
                Con_Enter(CS_LISTEN);
            }
        }
        break;

    case CS_LISTEN:
        /* The clock stops while he is mid-sentence, being transcribed or
         * typing: an answer in progress is not silence. */
        if (PcConMic_Speaking() || s_sttPending > 0 || PcConChat_IsOpen())
            s_stateT -= dt;
        if (s_stateT >= g_PcCon.listenSec)
        {
            char ev[256];
            snprintf(ev, sizeof(ev), "[%s just stands there and says nothing for %d seconds.]",
                     g_PcCon.playerName, (int)g_PcCon.listenSec);
            PcConChat_Add(PCCHAT_DIM, NULL, "[You say nothing.]");
            Con_PlayerTurn(ev);
        }
        break;

    case CS_THINKING:
        if (s_stateT > THINK_LIMIT)
        {
            SH_DBG("[CONSCIOUS] no answer from the model in %.0fs", THINK_LIMIT);
            s_gen++;
            Con_SpeakResult(NULL, 1, -1);
        }
        break;

    case CS_RELEASED:
        if (s_stateT >= g_PcCon.graceSec)
        {
            Con_Nearest(&nearD);
            if (nearD < g_PcCon.hearRange * 0.5f)
            {
                char ev[320];
                snprintf(ev, sizeof(ev),
                         "[You told him to get out of your alley %d seconds ago, but he is STILL here. "
                         "That makes %d time%s you let him go.%s]",
                         (int)g_PcCon.graceSec, s_releases, s_releases == 1 ? "" : "s",
                         s_releases >= 2 ? " Your patience is almost gone." : "");
                PcConChat_Add(PCCHAT_SYSTEM, NULL, "[They're coming back...]");
                Con_PlayerTurn(ev);
            }
        }
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void Con_Input(void)
{
    extern int g_PcConsoleInputActive;
    extern int g_PcQuickOptionsActive;
    const unsigned char* ks = SDL_GetKeyboardState(NULL);
    const int scene = (s_state != CS_OFF) && g_SysWork.sysState == SysState_Gameplay;
    int curMic, curChat;

    if (!ks)
        return;
    if (PcConChat_IsOpen())
    {
        char line[256];
        if (PcConChat_FeedKeys(ks, line, (int)sizeof(line)))
            Con_PlayerSaid(line, 1);
        s_swallow = 1;
        s_prevMic = s_prevChat = 1;
        return;
    }

    curMic  = s_scMic  != SDL_SCANCODE_UNKNOWN ? ks[s_scMic]  : 0;
    curChat = s_scChat != SDL_SCANCODE_UNKNOWN ? ks[s_scChat] : 0;
    if (scene && !g_PcConsoleInputActive && !g_PcQuickOptionsActive && s_state != CS_HOSTILE)
    {
        if (curMic && !s_prevMic)
        {
            if (!g_PcCon.stt || !g_PcCon.elevenKey[0])
                PcConChat_Add(PCCHAT_SYSTEM, NULL, "[Speech-to-text is off: set stt and elevenlabs_key in conscious.cfg]");
            else
                PcConMic_SetOn(!PcConMic_On());
        }
        if (curChat && !s_prevChat)
            PcConChat_Open();
    }
    s_prevMic  = curMic;
    s_prevChat = curChat;
}

static unsigned int Con_KeyButtons(const PsyXKeyboardMapping* m, int sc)
{
    unsigned int b = 0;
    if (m->kc_square   == sc) b |= 0x8000;
    if (m->kc_circle   == sc) b |= 0x2000;
    if (m->kc_triangle == sc) b |= 0x1000;
    if (m->kc_cross    == sc) b |= 0x4000;
    if (m->kc_l1 == sc) b |= 0x400;
    if (m->kc_l2 == sc) b |= 0x100;
    if (m->kc_l3 == sc) b |= 0x2;
    if (m->kc_r1 == sc) b |= 0x800;
    if (m->kc_r2 == sc) b |= 0x200;
    if (m->kc_r3 == sc) b |= 0x4;
    if (m->kc_start  == sc) b |= 0x8;
    if (m->kc_select == sc) b |= 0x1;
    if (m->kc_dpad_up    == sc) b |= 0x10;
    if (m->kc_dpad_right == sc) b |= 0x20;
    if (m->kc_dpad_down  == sc) b |= 0x40;
    if (m->kc_dpad_left  == sc) b |= 0x80;
    return b;
}

/* Called by game_main once per input parse. ~0 = drop the whole pad (typing),
 * otherwise the controller bits to clear (the buttons sharing the mic key,
 * V = Circle by default). Both hold for a few frames after the key comes up:
 * the keyboard->pad emulation lags SDL by a frame, and the stale press would
 * otherwise parse as a fresh click. */
unsigned int Pc_Conscious_PadMask(void)
{
    const unsigned char* ks;
    if (!g_PcCon.enabled || s_state == CS_OFF)
        return 0;
    ks = SDL_GetKeyboardState(NULL);
    if (PcConChat_IsOpen())
        return ~0u;
    if (s_swallow)
    {
        if (ks && !ks[SDL_SCANCODE_RETURN] && !ks[SDL_SCANCODE_KP_ENTER] && !ks[SDL_SCANCODE_ESCAPE])
        {
            if (++s_swallow > 5)
                s_swallow = 0;
        }
        return ~0u;
    }
    if (s_scMic == SDL_SCANCODE_UNKNOWN || s_state == CS_HOSTILE)
        return 0;
    if (ks && ks[s_scMic])
        s_micMaskHold = 5;
    else if (s_micMaskHold > 0)
        s_micMaskHold--;
    if (s_micMaskHold == 0)
        return 0;
    return Con_KeyButtons(&g_cfg_keyboardMapping, s_scMic) | Con_KeyButtons(&g_cfg_keyboardMapping2, s_scMic);
}

/* ------------------------------------------------------------------ */
/* Stalker hook                                                        */
/* ------------------------------------------------------------------ */

/* Asked by Ai_Stalker_Control_2 (map0_s00) every frame. 1 = stay calm and walk
 * to (goalX, goalZ), stopping stopDist short; 0 = run the retail AI. */
int Pc_Conscious_StalkerCalm(s_SubCharacter* chara, q19_12* goalX, q19_12* goalZ, q19_12* stopDist)
{
    int idx = (int)(chara - &g_SysWork.npcs[0]);
    int rank = 0, i;

    if (!g_PcCon.enabled || idx < 0 || idx >= NPC_N || idx >= 64)
        return 0;
    if (s_state == CS_OFF || s_state == CS_HOSTILE || g_SavegamePtr->mapIdx != MapIdx_MAP0_S00)
        return 0;

    s_calmSeenMs[idx] = SDL_GetTicks();
    if (!s_calmSeenMs[idx]) s_calmSeenMs[idx] = 1;

    for (i = 0; i < idx; i++)
        if (g_SysWork.npcs[i].model.charaId == Chara_GreyChild)
            rank++;

    if (s_state == CS_RELEASED)
    {
        *goalX    = (q19_12)(s_holdX[idx] * 4096.0f);
        *goalZ    = (q19_12)(s_holdZ[idx] * 4096.0f);
        *stopDist = Q12(0.3f);
    }
    else
    {
        /* A ring at different radii, so three children do not queue up on
         * the same spot in front of him. */
        *goalX    = g_SysWork.playerWork.player.position.vx;
        *goalZ    = g_SysWork.playerWork.player.position.vz;
        *stopDist = Q12(1.6f) + rank * Q12(0.55f);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

void Pc_Conscious_Frame(void)
{
    const unsigned int now = SDL_GetTicks();
    float          dt;
    int            live;
    unsigned char* wav;
    unsigned long  wavLen;
    PcConResult    r;
    PcConHudState  hud;

    if (!s_init)
    {
        s_init = 1;
        PcCon_CfgLoad();
        s_scMic  = SDL_GetScancodeFromName(g_PcCon.keyMic);
        s_scChat = SDL_GetScancodeFromName(g_PcCon.keyChat);
        s_lastMs = now;
    }
    if (!g_PcCon.enabled)
        return;

    dt = (float)(now - s_lastMs) / 1000.0f;
    s_lastMs = now;
    if (dt > 0.1f) dt = 0.1f;

    Pc_VoiceClip_Tick();
    Con_Input();

    /* Through speakers the mic hears them talking; none of that is Harry. */
    if (s_state == CS_SPEAKING)
        PcConMic_Discard();
    if (PcConMic_Pump(&wav, &wavLen))
    {
        if (s_state != CS_OFF && g_PcCon.stt && g_PcCon.elevenKey[0])
        {
            s_sttPending++;
            PcConNet_SubmitStt(s_gen, wav, wavLen);
        }
        else
        {
            free(wav);
        }
    }
    while (PcConNet_Poll(&r))
        Con_HandleResult(&r);

    live = g_SysWork.sysState == SysState_Gameplay && g_DeltaTime != 0;
    Con_Tick(dt, live);

    memset(&hud, 0, sizeof(hud));
    if (s_state != CS_OFF && s_state != CS_HOSTILE && g_GameWork.gameState == GameState_InGame)
    {
        hud.micIcon     = 1;
        hud.micOn       = PcConMic_On();
        hud.micSpeaking = PcConMic_Speaking();
        hud.micFailed   = PcConMic_Failed();
        hud.micLevel    = PcConMic_Level();
        hud.micKeyName  = g_PcCon.keyMic;
        hud.chatKeyName = g_PcCon.keyChat;
        if (s_state == CS_LISTEN)
        {
            int left = (int)ceilf(g_PcCon.listenSec - s_stateT);
            snprintf(hud.status, sizeof(hud.status), s_sttPending ? "They're listening..." :
                     "They want an answer... %d", left < 0 ? 0 : left);
        }
        else if (s_state == CS_THINKING)
        {
            snprintf(hud.status, sizeof(hud.status), "The grey children mutter among themselves...");
        }
        else if (s_state == CS_RELEASED && s_stateT < g_PcCon.graceSec)
        {
            snprintf(hud.status, sizeof(hud.status), "They're letting you go... %d",
                     (int)ceilf(g_PcCon.graceSec - s_stateT));
        }
    }
    if (g_GameWork.gameState == GameState_InGame)
        PcConHud_Draw(&hud);
}
