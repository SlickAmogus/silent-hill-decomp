/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_menu.c - the in-game Conscious menu (key_menu, M by default).
 *
 * Drawn with the native menu layer the Controls and Achievements screens
 * use, into the game's own ordering table, so it looks like part of the game
 * rather than the GL overlay the conversation HUD is. The game is frozen
 * while it is open (game_main zeroes dt for it, as for Quick Options) and the
 * pad is swallowed (Pc_Conscious_PadMask).
 *
 * Three pages: the list of who can talk, General, and one per kind (voice,
 * mood, delivery, extra info, personality). Everything it changes is live at
 * once and written back to conscious.cfg when it closes; a personality is a
 * text file beside the exe, opened in the system's text editor.
 */

#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/text/text_draw.h"
#include "lang_text.h"
#include "pc_bind_input.h"
#include "pc_conscious_menu.h"
#include "pc_native_menu.h"
#include "pc_native_menu_draw.h"
#include "pc_voice_clip.h"
#include "sh_log.h"

#include "pc_conscious.h"

#define CM_HEAD_Y       8
#define CM_RULE1_Y      26
#define CM_ARROW_UP_Y   33
#define CM_LIST_Y       40
#define CM_PITCH        16
#define CM_VISIBLE      9
#define CM_ARROW_DOWN_Y 189
#define CM_RULE2_Y      196
#define CM_FOOTER_Y     202

#define CM_RULE_X0      4
#define CM_RULE_X1      316
#define CM_TEXT_X       28
#define CM_VALUE_RX     300
#define CM_VALUE_W      150
#define CM_HL_X0        3
#define CM_HL_X1        316

#define CM_ROWS_MAX     24
#define CM_VOICES_MAX   96

enum { PAGE_MAIN, PAGE_GENERAL, PAGE_PROF };
enum { RK_TOGGLE, RK_STEP, RK_TEXT, RK_ACTION };

enum
{
    /* main */
    ID_GENERAL = 1,
    ID_PROF0,                     /* + profile index */
    /* general */
    ID_ENABLED = ID_PROF0 + PCCON_PROF_N,
    ID_MONSTERS,
    ID_CHATLOG,
    ID_BUBBLES,
    ID_PROMPTS,
    ID_TTS,
    ID_STT,
    ID_MICON,
    ID_VISION,
    ID_VOLUME,
    ID_NAME,
    ID_LISTEN,
    ID_TURNS,
    ID_MTURNS,
    ID_RANGE,
    ID_INCITE,
    ID_TEMP,
    /* profile */
    ID_TALK,
    ID_VOICE,
    ID_TEST,
    ID_MOOD,
    ID_PITCH,
    ID_STYLE,
    ID_STAB,
    ID_SPEED,
    ID_EXTRA,
    ID_PERSONA,
    ID_PERSONA_RESET,
    ID_VOICE_RESET
};

typedef struct
{
    const char* label;
    int         kind;
    int         id;
} CmRow;

static int          s_open;
static int          s_page;
static int          s_prof;
static int          s_mainSel;
static int          s_onBack;
static int          s_dirty;
static CmRow        s_rows[CM_ROWS_MAX];
static int          s_rowN;
static NmScroll     s_scroll;
static BindNav      s_nav;
static SDL_Scancode s_scMenu = SDL_SCANCODE_UNKNOWN;
static char         s_scMenuName[32];
static int          s_prevMenuKey;

static int           s_edit;          /* id of the text row being typed into, 0 = none */
static char          s_editBuf[240];
static int           s_editLen;
static unsigned char s_editPrev[SDL_NUM_SCANCODES];
static int           s_editHave;

static struct { char id[64]; char name[64]; } s_voices[CM_VOICES_MAX];
static int          s_voiceN;
static int          s_voicesAsked;

static unsigned int s_testClip;
static int          s_testWait;
static int          s_testProf;

static int          s_personaCustom;
static int          s_personaCheck;

static void cm_beep(int sfx)
{
    Sd_PlaySfx((u16)sfx, 0, 64);
}

/* ---- voices -------------------------------------------------------- */

void PcConMenu_Voices(const char* list)
{
    const char* p = list;

    s_voiceN = 0;
    while (*p && s_voiceN < CM_VOICES_MAX)
    {
        const char* tab = strchr(p, '\t');
        const char* nl  = strchr(p, '\n');
        if (!tab || !nl || tab > nl)
            break;
        SDL_strlcpy(s_voices[s_voiceN].id, p, (size_t)(tab - p + 1) < sizeof(s_voices[0].id)
                                                  ? (size_t)(tab - p + 1) : sizeof(s_voices[0].id));
        SDL_strlcpy(s_voices[s_voiceN].name, tab + 1, (size_t)(nl - tab) < sizeof(s_voices[0].name)
                                                          ? (size_t)(nl - tab) : sizeof(s_voices[0].name));
        s_voiceN++;
        p = nl + 1;
    }
    SH_DBG("[CONSCIOUS] menu: %d voices on the account", s_voiceN);
}

void PcConMenu_Said(PcConResult* r)
{
    s_testWait = 0;
    if (r->ok && r->pcm)
    {
        Pc_VoiceClip_Stop(s_testClip);
        s_testClip = Pc_VoiceClip_PlayEx(r->pcm, r->samples, r->rate, 0, g_PcCon.voiceVolume,
                                         g_PcConProf[s_testProf].pitch);
    }
    PcConNet_Free(r);
}

static int cm_voice_index(const char* id)
{
    int i;
    if (!id[0])
        return -1;
    for (i = 0; i < s_voiceN; i++)
        if (!strcmp(s_voices[i].id, id))
            return i;
    return -2;
}

/* The voice the test line is read in: the profile's, or what it falls back to. */
static void cm_voice_for_test(int prof, char* out, int cap)
{
    const char* v = g_PcConProf[prof].voice;
    if (v[0])
    {
        SDL_strlcpy(out, v, (size_t)cap);
    }
    else if (prof == PCCON_PROF_GANG || !g_PcCon.monsterVoices[0])
    {
        SDL_strlcpy(out, g_PcCon.voiceId, (size_t)cap);
    }
    else
    {
        const char* comma = strchr(g_PcCon.monsterVoices, ',');
        size_t      n     = comma ? (size_t)(comma - g_PcCon.monsterVoices) : strlen(g_PcCon.monsterVoices);
        SDL_strlcpy(out, g_PcCon.monsterVoices, n + 1 < (size_t)cap ? n + 1 : (size_t)cap);
    }
}

static void cm_test_voice(void)
{
    static const char* const lines[PCCON_PROF_N] = {
        "Hey! Grown-up! This is OUR alley. What do you think you're doing down here?",
        "Hrrrh... I smell meat. Talk fast, little man, before I stop listening.",
        "Ha! I jump on you. I sit on you. Then I bite. You want to talk first? Fine.",
        "Down there on the ground, so slow, so small. I see everything from up here.",
        "Shhh... the dark is mine. You are very far from home, aren't you?",
    };
    const PcConProfile* p = &g_PcConProf[s_prof];
    PcConVoiceTune      t;
    char                voice[64];

    if (!g_PcCon.tts || !g_PcCon.elevenKey[0])
    {
        cm_beep(Sfx_MenuError);
        return;
    }
    t.stability = p->stability;
    t.style     = p->style;
    t.speed     = p->speed;
    cm_voice_for_test(s_prof, voice, (int)sizeof(voice));
    Pc_VoiceClip_Stop(s_testClip);
    s_testClip = 0;
    s_testWait = 1;
    s_testProf = s_prof;
    PcConNet_SubmitSay(0, lines[s_prof], voice, &t);
    cm_beep(Sfx_MenuConfirm);
}

/* ---- personality file ---------------------------------------------- */

static void cm_persona_refresh(void)
{
    char text[4096];
    s_personaCustom = PcCon_PersonaLoad(s_prof, text, (int)sizeof(text)) &&
                      strcmp(text, PcCon_DefaultPersona(s_prof)) != 0;
    s_personaCheck  = 0;
}

static void cm_persona_edit(void)
{
    char  rel[96], full[1024], url[1100];
    FILE* f;
    int   i;

    PcCon_PersonaPath(s_prof, rel, (int)sizeof(rel));
    f = fopen(rel, "rb");
    if (f)
    {
        fclose(f);
    }
    else
    {
        f = fopen(rel, "wb");
        if (!f)
        {
            cm_beep(Sfx_MenuError);
            return;
        }
        fputs(PcCon_DefaultPersona(s_prof), f);
        fputs("\n", f);
        fclose(f);
    }
#ifdef _WIN32
    if (!_fullpath(full, rel, sizeof(full)))
#else
    if (!realpath(rel, full))
#endif
        SDL_strlcpy(full, rel, sizeof(full));
    for (i = 0; full[i]; i++)
        if (full[i] == '\\')
            full[i] = '/';
    snprintf(url, sizeof(url), "file:///%s", full[0] == '/' ? full + 1 : full);
    SH_DBG("[CONSCIOUS] menu: opening %s", url);
    if (SDL_OpenURL(url) != 0)
    {
        SH_DBG("[CONSCIOUS] menu: could not open it: %s", SDL_GetError());
        cm_beep(Sfx_MenuError);
        return;
    }
    cm_beep(Sfx_MenuConfirm);
}

static void cm_persona_reset(void)
{
    char rel[96];
    PcCon_PersonaPath(s_prof, rel, (int)sizeof(rel));
    if (remove(rel) != 0)
    {
        cm_beep(Sfx_MenuError);
        return;
    }
    s_personaCustom = 0;
    cm_beep(Sfx_MenuConfirm);
}

/* ---- pages ---------------------------------------------------------- */

static void cm_add(const char* label, int kind, int id)
{
    if (s_rowN >= CM_ROWS_MAX)
        return;
    s_rows[s_rowN].label = label;
    s_rows[s_rowN].kind  = kind;
    s_rows[s_rowN].id    = id;
    s_rowN++;
}

static void cm_build(int page, int sel)
{
    static const char* const profLabels[PCCON_PROF_N] = { "Grey_Children", "Groaner", "Romper", "Air_Screamer",
                                                          "Night_Flutter" };
    int i;

    s_page = page;
    s_rowN = 0;
    switch (page)
    {
    case PAGE_MAIN:
        cm_add("General", RK_ACTION, ID_GENERAL);
        for (i = 0; i < PCCON_PROF_N; i++)
            cm_add(profLabels[i], RK_ACTION, ID_PROF0 + i);
        break;

    case PAGE_GENERAL:
        cm_add("Talking_NPCs", RK_TOGGLE, ID_ENABLED);
        cm_add("Monsters_Talk", RK_TOGGLE, ID_MONSTERS);
        cm_add("Chat_Log", RK_TOGGLE, ID_CHATLOG);
        cm_add("Speech_Bubbles", RK_TOGGLE, ID_BUBBLES);
        cm_add("Prompts", RK_TOGGLE, ID_PROMPTS);
        cm_add("Voices", RK_TOGGLE, ID_TTS);
        cm_add("Microphone", RK_TOGGLE, ID_STT);
        cm_add("Mic_Starts_On", RK_TOGGLE, ID_MICON);
        cm_add("They_See_The_Screen", RK_TOGGLE, ID_VISION);
        cm_add("Voice_Volume", RK_STEP, ID_VOLUME);
        cm_add("Your_Name", RK_TEXT, ID_NAME);
        cm_add("Answer_Time", RK_STEP, ID_LISTEN);
        cm_add("Patience_(Alley)", RK_STEP, ID_TURNS);
        cm_add("Patience_(Monsters)", RK_STEP, ID_MTURNS);
        cm_add("Hearing_Range", RK_STEP, ID_RANGE);
        cm_add("Trick_Chance", RK_STEP, ID_INCITE);
        cm_add("Creativity", RK_STEP, ID_TEMP);
        break;

    default:
        cm_add("Talks", RK_TOGGLE, ID_TALK);
        cm_add("Voice", RK_STEP, ID_VOICE);
        cm_add("Test_Voice", RK_ACTION, ID_TEST);
        cm_add("Mood", RK_STEP, ID_MOOD);
        cm_add("Pitch", RK_STEP, ID_PITCH);
        cm_add("Emotion", RK_STEP, ID_STYLE);
        cm_add("Steadiness", RK_STEP, ID_STAB);
        cm_add("Speed", RK_STEP, ID_SPEED);
        cm_add("Extra_Info", RK_TEXT, ID_EXTRA);
        cm_add("Personality", RK_ACTION, ID_PERSONA);
        cm_add("Reset_Personality", RK_ACTION, ID_PERSONA_RESET);
        cm_add("Reset_Voice_Settings", RK_ACTION, ID_VOICE_RESET);
        cm_persona_refresh();
        break;
    }
    NmScroll_Init(&s_scroll, s_rowN, CM_VISIBLE);
    NmScroll_Select(&s_scroll, sel < s_rowN ? sel : 0);
    s_onBack = 0;
}

/* ---- values --------------------------------------------------------- */

static const char* cm_onoff(int v)
{
    return v ? "On" : "Off";
}

static void cm_value(int id, char* out, int cap)
{
    const PcConProfile* p = &g_PcConProf[s_prof];

    out[0] = '\0';
    if (id >= ID_PROF0 && id < ID_PROF0 + PCCON_PROF_N)
    {
        SDL_strlcpy(out, g_PcConProf[id - ID_PROF0].talk ? "Talks" : "Silent", (size_t)cap);
        return;
    }
    switch (id)
    {
    case ID_ENABLED:  SDL_strlcpy(out, cm_onoff(g_PcCon.enabled), (size_t)cap); break;
    case ID_MONSTERS: SDL_strlcpy(out, cm_onoff(g_PcCon.monsters), (size_t)cap); break;
    case ID_CHATLOG:  SDL_strlcpy(out, cm_onoff(g_PcCon.chatLog), (size_t)cap); break;
    case ID_BUBBLES:  SDL_strlcpy(out, cm_onoff(g_PcCon.bubbles), (size_t)cap); break;
    case ID_PROMPTS:  SDL_strlcpy(out, cm_onoff(g_PcCon.prompts), (size_t)cap); break;
    case ID_TTS:      SDL_strlcpy(out, cm_onoff(g_PcCon.tts), (size_t)cap); break;
    case ID_STT:      SDL_strlcpy(out, cm_onoff(g_PcCon.stt), (size_t)cap); break;
    case ID_MICON:    SDL_strlcpy(out, cm_onoff(g_PcCon.micStartOn), (size_t)cap); break;
    case ID_VISION:   SDL_strlcpy(out, cm_onoff(g_PcCon.vision), (size_t)cap); break;
    case ID_VOLUME:   snprintf(out, (size_t)cap, "%d", (int)(g_PcCon.voiceVolume * 100.0f + 0.5f)); break;
    case ID_NAME:     SDL_strlcpy(out, g_PcCon.playerName, (size_t)cap); break;
    case ID_LISTEN:   snprintf(out, (size_t)cap, "%d s", (int)(g_PcCon.listenSec + 0.5f)); break;
    case ID_TURNS:    snprintf(out, (size_t)cap, "%d turns", g_PcCon.maxTurns); break;
    case ID_MTURNS:   snprintf(out, (size_t)cap, "%d turns", g_PcCon.monsterMaxTurns); break;
    case ID_RANGE:    snprintf(out, (size_t)cap, "%d m", (int)(g_PcCon.monsterHearRange + 0.5f)); break;
    case ID_INCITE:   snprintf(out, (size_t)cap, "%d in 100", (int)(g_PcCon.monsterInciteChance * 100.0f + 0.5f)); break;
    case ID_TEMP:     snprintf(out, (size_t)cap, "%.1f", g_PcCon.llmTemperature); break;

    case ID_TALK:     SDL_strlcpy(out, cm_onoff(p->talk), (size_t)cap); break;
    case ID_VOICE:
    {
        int v = cm_voice_index(p->voice);
        if (v == -1)
            SDL_strlcpy(out, s_prof == PCCON_PROF_GANG ? "Default" : "Random", (size_t)cap);
        else if (v == -2)
            snprintf(out, (size_t)cap, "Custom %.8s", p->voice);
        else
            SDL_strlcpy(out, s_voices[v].name, (size_t)cap);
        break;
    }
    case ID_TEST:     SDL_strlcpy(out, s_testWait ? "..." : "", (size_t)cap); break;
    case ID_MOOD:     SDL_strlcpy(out, PcCon_MoodName(p->mood), (size_t)cap); break;
    case ID_PITCH:    snprintf(out, (size_t)cap, "%.2f", p->pitch); break;
    case ID_STYLE:    snprintf(out, (size_t)cap, "%d", (int)(p->style * 100.0f + 0.5f)); break;
    case ID_STAB:     snprintf(out, (size_t)cap, "%d", (int)(p->stability * 100.0f + 0.5f)); break;
    case ID_SPEED:    snprintf(out, (size_t)cap, "%.2f", p->speed); break;
    case ID_EXTRA:    SDL_strlcpy(out, p->extra[0] ? p->extra : "None", (size_t)cap); break;
    case ID_PERSONA:  SDL_strlcpy(out, s_personaCustom ? "Custom" : "As written", (size_t)cap); break;
    default:          break;
    }
}

static float cm_stepf(float v, int dir, float step, float lo, float hi)
{
    v += (float)dir * step;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    /* Kill the drift that repeated float steps pile up. */
    return (float)(int)(v / step + (v >= 0.0f ? 0.5f : -0.5f)) * step;
}

static int cm_stepi(int v, int dir, int lo, int hi)
{
    v += dir;
    return v < lo ? lo : v > hi ? hi : v;
}

/* 1 when something changed. */
static int cm_step(int id, int dir)
{
    PcConProfile* p = &g_PcConProf[s_prof];

    switch (id)
    {
    case ID_ENABLED:  g_PcCon.enabled = !g_PcCon.enabled; break;
    case ID_MONSTERS: g_PcCon.monsters = !g_PcCon.monsters; break;
    case ID_CHATLOG:  g_PcCon.chatLog = !g_PcCon.chatLog; break;
    case ID_BUBBLES:  g_PcCon.bubbles = !g_PcCon.bubbles; break;
    case ID_PROMPTS:  g_PcCon.prompts = !g_PcCon.prompts; break;
    case ID_TTS:      g_PcCon.tts = !g_PcCon.tts; break;
    case ID_STT:
        g_PcCon.stt = !g_PcCon.stt;
        if (!g_PcCon.stt)
            PcConMic_SetOn(0);
        break;
    case ID_MICON:    g_PcCon.micStartOn = !g_PcCon.micStartOn; break;
    case ID_VISION:   g_PcCon.vision = !g_PcCon.vision; break;
    case ID_VOLUME:   g_PcCon.voiceVolume = cm_stepf(g_PcCon.voiceVolume, dir, 0.1f, 0.0f, 1.5f); break;
    case ID_LISTEN:   g_PcCon.listenSec = cm_stepf(g_PcCon.listenSec, dir, 5.0f, 10.0f, 120.0f); break;
    case ID_TURNS:    g_PcCon.maxTurns = cm_stepi(g_PcCon.maxTurns, dir, 1, 20); break;
    case ID_MTURNS:   g_PcCon.monsterMaxTurns = cm_stepi(g_PcCon.monsterMaxTurns, dir, 1, 20); break;
    case ID_RANGE:
        g_PcCon.monsterHearRange = cm_stepf(g_PcCon.monsterHearRange, dir, 1.0f, 4.0f, 30.0f);
        if (g_PcCon.monsterFleeRange < g_PcCon.monsterHearRange + 2.0f)
            g_PcCon.monsterFleeRange = g_PcCon.monsterHearRange + 2.0f;
        break;
    case ID_INCITE:   g_PcCon.monsterInciteChance = cm_stepf(g_PcCon.monsterInciteChance, dir, 0.05f, 0.0f, 1.0f); break;
    case ID_TEMP:     g_PcCon.llmTemperature = cm_stepf(g_PcCon.llmTemperature, dir, 0.1f, 0.2f, 1.6f); break;

    case ID_TALK:     p->talk = !p->talk; break;
    case ID_VOICE:
    {
        int v = cm_voice_index(p->voice);
        if (s_voiceN == 0)
            return 0;
        if (v == -2)
            v = -1;
        v += dir;
        if (v < -1) v = s_voiceN - 1;
        if (v >= s_voiceN) v = -1;
        SDL_strlcpy(p->voice, v < 0 ? "" : s_voices[v].id, sizeof(p->voice));
        break;
    }
    case ID_MOOD:     p->mood = (p->mood + dir + PCCON_MOOD_N) % PCCON_MOOD_N; break;
    case ID_PITCH:    p->pitch = cm_stepf(p->pitch, dir, 0.05f, 0.7f, 1.4f); break;
    case ID_STYLE:    p->style = cm_stepf(p->style, dir, 0.05f, 0.0f, 1.0f); break;
    case ID_STAB:     p->stability = cm_stepf(p->stability, dir, 0.05f, 0.0f, 1.0f); break;
    case ID_SPEED:    p->speed = cm_stepf(p->speed, dir, 0.05f, 0.7f, 1.2f); break;
    default:          return 0;
    }
    s_dirty = 1;
    return 1;
}

/* ---- text entry ----------------------------------------------------- */

static void cm_edit_begin(int id)
{
    const char* cur = id == ID_NAME ? g_PcCon.playerName : g_PcConProf[s_prof].extra;
    SDL_strlcpy(s_editBuf, cur, sizeof(s_editBuf));
    s_editLen  = (int)strlen(s_editBuf);
    s_edit     = id;
    s_editHave = 0;
    cm_beep(Sfx_MenuConfirm);
}

static void cm_edit_end(int commit)
{
    if (commit)
    {
        if (s_edit == ID_NAME)
        {
            if (s_editLen > 0)
                SDL_strlcpy(g_PcCon.playerName, s_editBuf, sizeof(g_PcCon.playerName));
        }
        else
        {
            SDL_strlcpy(g_PcConProf[s_prof].extra, s_editBuf, sizeof(g_PcConProf[s_prof].extra));
        }
        s_dirty = 1;
    }
    cm_beep(commit ? Sfx_MenuConfirm : Sfx_MenuCancel);
    s_edit = 0;
    /* The Enter or Esc that ended it must not also navigate. */
    BindNav_Reset(&s_nav);
}

static void cm_edit_update(const Uint8* ks)
{
    const int cap = s_edit == ID_NAME ? (int)sizeof(g_PcCon.playerName) : (int)sizeof(s_editBuf);
    int       sc, shift, ctrl;

#define CM_EDGE(k) (ks[k] && !s_editPrev[k])
    if (!s_editHave)
    {
        memcpy(s_editPrev, ks, SDL_NUM_SCANCODES);
        s_editHave = 1;
        return;
    }
    shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    ctrl  = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
    if (CM_EDGE(SDL_SCANCODE_ESCAPE))
    {
        cm_edit_end(0);
    }
    else if (CM_EDGE(SDL_SCANCODE_RETURN) || CM_EDGE(SDL_SCANCODE_KP_ENTER))
    {
        cm_edit_end(1);
    }
    else
    {
        if (CM_EDGE(SDL_SCANCODE_BACKSPACE) && s_editLen > 0)
            s_editBuf[--s_editLen] = '\0';
        if (ctrl && CM_EDGE(SDL_SCANCODE_BACKSPACE))
        {
            s_editLen    = 0;
            s_editBuf[0] = '\0';
        }
        for (sc = 0; sc < SDL_NUM_SCANCODES && !ctrl; sc++)
        {
            char c;
            if (!CM_EDGE(sc))
                continue;
            c = PcConChat_Glyph(sc, shift);
            if (c && s_editLen < cap - 1)
            {
                s_editBuf[s_editLen++] = c;
                s_editBuf[s_editLen]   = '\0';
            }
        }
    }
#undef CM_EDGE
    memcpy(s_editPrev, ks, SDL_NUM_SCANCODES);
}

/* ---- open / close / input ------------------------------------------- */

int Pc_ConsciousMenu_IsOpen(void)
{
    return s_open;
}

static void cm_open(void)
{
    s_open  = 1;
    s_dirty = 0;
    s_edit  = 0;
    cm_build(PAGE_MAIN, 0);
    BindNav_Reset(&s_nav);
    if (!s_voicesAsked && g_PcCon.elevenKey[0])
    {
        s_voicesAsked = 1;
        PcConNet_SubmitVoices(0);
    }
    cm_beep(Sfx_MenuConfirm);
    SH_DBG("[CONSCIOUS] menu open");
}

static void cm_close(void)
{
    if (s_edit)
        cm_edit_end(0);
    Pc_VoiceClip_Stop(s_testClip);
    s_testClip = 0;
    if (s_dirty)
        PcCon_CfgSave();
    s_open = 0;
    cm_beep(Sfx_MenuCancel);
    SH_DBG("[CONSCIOUS] menu closed%s", s_dirty ? ", settings saved" : "");
}

static void cm_back(void)
{
    if (s_page == PAGE_MAIN)
    {
        cm_close();
        return;
    }
    cm_beep(Sfx_MenuCancel);
    cm_build(PAGE_MAIN, s_mainSel);
}

static void cm_activate(const CmRow* r)
{
    if (r->id == ID_GENERAL || (r->id >= ID_PROF0 && r->id < ID_PROF0 + PCCON_PROF_N))
    {
        s_mainSel = s_scroll.sel;
        if (r->id != ID_GENERAL)
            s_prof = r->id - ID_PROF0;
        cm_beep(Sfx_MenuConfirm);
        cm_build(r->id == ID_GENERAL ? PAGE_GENERAL : PAGE_PROF, 0);
        return;
    }
    switch (r->kind)
    {
    case RK_TEXT:
        cm_edit_begin(r->id);
        return;
    case RK_TOGGLE:
    case RK_STEP:
        if (cm_step(r->id, 1))
            cm_beep(Sfx_MenuMove);
        else
            cm_beep(Sfx_MenuError);
        return;
    default:
        break;
    }
    switch (r->id)
    {
    case ID_TEST:          cm_test_voice(); break;
    case ID_PERSONA:       cm_persona_edit(); break;
    case ID_PERSONA_RESET: cm_persona_reset(); break;
    case ID_VOICE_RESET:
    {
        PcConProfile* p = &g_PcConProf[s_prof];
        p->pitch     = g_PcCon.voicePitch;
        p->stability = g_PcCon.voiceStability;
        p->style     = g_PcCon.voiceStyle;
        p->speed     = 1.0f;
        p->mood      = 0;
        s_dirty      = 1;
        cm_beep(Sfx_MenuConfirm);
        break;
    }
    default:
        cm_beep(Sfx_MenuError);
        break;
    }
}

static int cm_may_open(void)
{
    extern int g_PcConsoleInputActive;
    extern int g_PcQuickOptionsActive;
    return g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
           !(g_SysWork.sysFlags & SysFlag_DemoActive) && !g_PcConsoleInputActive && !g_PcQuickOptionsActive &&
           !PcConChat_IsOpen();
}

void Pc_ConsciousMenu_Update(void)
{
    const Uint8* ks = SDL_GetKeyboardState(NULL);
    int          pressed[BN_COUNT];
    int          menuKey, edge;

    if (!ks)
        return;
    if (strcmp(s_scMenuName, g_PcCon.keyMenu))
    {
        SDL_strlcpy(s_scMenuName, g_PcCon.keyMenu, sizeof(s_scMenuName));
        s_scMenu = g_PcCon.keyMenu[0] ? SDL_GetScancodeFromName(g_PcCon.keyMenu) : SDL_SCANCODE_UNKNOWN;
    }
    menuKey = s_scMenu != SDL_SCANCODE_UNKNOWN && ks[s_scMenu];
    edge    = menuKey && !s_prevMenuKey;
    s_prevMenuKey = menuKey;

    if (!s_open)
    {
        if (edge && cm_may_open())
            cm_open();
        return;
    }
    if (g_GameWork.gameState != GameState_InGame)
    {
        cm_close();
        return;
    }
    if (s_edit)
    {
        cm_edit_update(ks);
        return;
    }
    if (edge)
    {
        cm_close();
        return;
    }
    if (s_page == PAGE_PROF && ++s_personaCheck >= 60)
        cm_persona_refresh();

    BindNav_Poll(&s_nav, pressed);
    if (pressed[BN_BACK])
    {
        cm_back();
        return;
    }
    if (pressed[BN_UP] || pressed[BN_DOWN])
    {
        int oldSel = s_scroll.sel, oldBack = s_onBack;
        if (pressed[BN_UP])
        {
            if (s_onBack)
                s_onBack = 0;
            else if (s_scroll.sel == 0)
                s_onBack = 1;
            else
                NmScroll_Move(&s_scroll, -1, 0);
        }
        else
        {
            if (s_onBack)
            {
                s_onBack = 0;
                NmScroll_Select(&s_scroll, 0);
            }
            else if (s_scroll.sel >= s_rowN - 1)
                s_onBack = 1;
            else
                NmScroll_Move(&s_scroll, 1, 0);
        }
        if (s_scroll.sel != oldSel || s_onBack != oldBack)
            cm_beep(Sfx_MenuMove);
    }
    if ((pressed[BN_LEFT] || pressed[BN_RIGHT]) && !s_onBack)
    {
        const CmRow* r = &s_rows[s_scroll.sel];
        if ((r->kind == RK_TOGGLE || r->kind == RK_STEP) && cm_step(r->id, pressed[BN_LEFT] ? -1 : 1))
            cm_beep(Sfx_MenuMove);
    }
    if (pressed[BN_OK])
    {
        if (s_onBack)
            cm_back();
        else
            cm_activate(&s_rows[s_scroll.sel]);
    }
}

/* ---- draw ----------------------------------------------------------- */

static const char* cm_hint(const CmRow* r)
{
    if (s_edit)
        return "Enter_saves,_Esc_cancels";
    if (s_onBack)
        return s_page == PAGE_MAIN ? "Close_(saves)" : "";
    if (r->id == ID_PERSONA)
        return "Opens_in_your_text_editor";
    switch (r->kind)
    {
    case RK_TOGGLE:
    case RK_STEP:   return "Left/Right_to_change";
    case RK_TEXT:   return "Enter_to_type";
    default:        return "";
    }
}

/* Right-aligned at CM_VALUE_RX, at most CM_VALUE_W wide: a long one pans
 * while its row is selected. */
static void cm_value_draw(const char* v, int y, int sel, int arrows)
{
    char clean[300];
    int  w;

    Nm_SanitizeGlyphs(v, clean, sizeof(clean), 0);
    if (!clean[0])
        return;
    w = Pc_LangMenuTextWidth(clean);
    if (w > CM_VALUE_W)
        w = CM_VALUE_W;
    Nm_TextDrawMarquee(CM_VALUE_RX - w, y, NM_TEXT_TALL, sel ? StringColorId_Gold : StringColorId_White, clean,
                       CM_VALUE_W, sel);
    if (sel && arrows)
        Nm_ValueArrowsDraw(CM_VALUE_RX - w, CM_VALUE_RX, y);
}

/* The text being typed: its tail, so the cursor end is always on screen. */
static void cm_edit_draw(int y)
{
    char        clean[300];
    char        shown[300];
    const char* tail;

    Nm_SanitizeGlyphs(s_editBuf, clean, sizeof(clean), 0);
    tail = clean;
    while (*tail && Pc_LangMenuTextWidth(tail) > CM_VALUE_W - 10)
        tail++;
    snprintf(shown, sizeof(shown), "%s%s", tail, (SDL_GetTicks() / 400) & 1 ? "*" : "");
    Nm_TextDrawVerbatim(CM_VALUE_RX - CM_VALUE_W, y, NM_TEXT_TALL, StringColorId_Gold, shown);
}

static void cm_row_draw(int i, int y)
{
    const CmRow* r   = &s_rows[i];
    const int    sel = !s_onBack && i == s_scroll.sel;
    char         v[300];

    Nm_BulletDraw(CM_TEXT_X - NM_BULLET_TEXT_DX, y, sel);
    Nm_TextDraw(CM_TEXT_X, y, NM_TEXT_TALL, StringColorId_White, r->label);
    if (sel && s_edit == r->id)
    {
        cm_edit_draw(y);
    }
    else
    {
        cm_value(r->id, v, (int)sizeof(v));
        cm_value_draw(v, y, sel, r->kind == RK_TOGGLE || r->kind == RK_STEP);
    }
    if (sel)
        Nm_HighlightDraw(CM_HL_X0, CM_HL_X1, y + 14);
}

static void cm_heading_draw(void)
{
    char head[64];

    if (s_page == PAGE_MAIN)
        SDL_strlcpy(head, "CONSCIOUS_NPCS", sizeof(head));
    else if (s_page == PAGE_GENERAL)
        SDL_strlcpy(head, "GENERAL", sizeof(head));
    else
        Nm_SanitizeGlyphs(PcCon_ProfName(s_prof), head, sizeof(head), 1);
    Nm_TextDrawCentered(160, CM_HEAD_Y, NM_TEXT_TALL, StringColorId_White, head);
}

void Pc_ConsciousMenu_Draw(void)
{
    const char* back;
    const char* hint;
    int         i;

    if (!s_open)
        return;

    Nm_BlackBackgroundDraw();
    cm_heading_draw();
    Nm_RuleDraw(CM_RULE_X0, CM_RULE_X1, CM_RULE1_Y, 0);

    for (i = s_scroll.top; i < s_rowN && i < s_scroll.top + s_scroll.visible; i++)
        cm_row_draw(i, CM_LIST_Y + (i - s_scroll.top) * CM_PITCH);
    if (s_rowN > s_scroll.visible)
    {
        Nm_ScrollArrowDraw(160, CM_ARROW_UP_Y, 1, NmScroll_CanUp(&s_scroll));
        Nm_ScrollArrowDraw(160, CM_ARROW_DOWN_Y, 0, NmScroll_CanDown(&s_scroll));
    }

    Nm_RuleDraw(CM_RULE_X0, CM_RULE_X1, CM_RULE2_Y, 0);
    back = s_page == PAGE_MAIN ? "Close" : "Back";
    Nm_BulletDraw(CM_TEXT_X - NM_BULLET_TEXT_DX, CM_FOOTER_Y, s_onBack);
    Nm_TextDraw(CM_TEXT_X, CM_FOOTER_Y, NM_TEXT_TALL, StringColorId_White, back);
    if (s_onBack)
        Nm_HighlightDraw(CM_TEXT_X - NM_BULLET_TEXT_DX - 1, CM_TEXT_X + Nm_TextWidth(back) + 4, CM_FOOTER_Y + 14);
    hint = cm_hint(&s_rows[s_scroll.sel]);
    if (hint[0])
        Nm_TextDrawRight(CM_RULE_X1, CM_FOOTER_Y, NM_TEXT_TALL, StringColorId_LightGrey, hint);
}
