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
    ID_CALL,
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
    else if (prof == PCCON_PROF_DARK)
    {
        SDL_strlcpy(out, Pc_Conscious_DarkVoice(), (size_t)cap);
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
        "Cheryl? ...No. You're not her. And you're not me. Keep walking, Harry. I'll catch up.",
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

static int pe_fold_into(const char* src, char* out);

/* Compared as the editor holds them, or a saved untouched default (quotes
 * folded to apostrophes) would read as custom. */
static void cm_persona_refresh(void)
{
    char text[4096], a[1024], b[1024];
    s_personaCustom = 0;
    if (PcCon_PersonaLoad(s_prof, text, (int)sizeof(text)))
    {
        pe_fold_into(text, a);
        pe_fold_into(PcCon_DefaultPersona(s_prof), b);
        s_personaCustom = strcmp(a, b) != 0;
    }
    s_personaCheck = 0;
}

/* ---- personality editor ---------------------------------------------- */

/* A wrapped text box drawn in the menu font: caret, Shift selection, word
 * jumps, clipboard and one step of undo. Only what FONT16 can draw gets in,
 * so what is on screen is exactly what goes into the prompt. */

#define PE_MAX      700
#define PE_X        16
#define PE_W        288
#define PE_TOP      36
#define PE_LINES    8
#define PE_ARROW_Y  170
#define PE_KEYS_Y   178
#define PE_LINE_CAP 64

static int  s_pe;                    /* the editor is up */
static char s_peBuf[PE_MAX + 1];
static int  s_peLen, s_peCur, s_peAnchor, s_peTop;
static char s_peUndo[PE_MAX + 1];
static int  s_peUndoLen, s_peUndoCur, s_peHaveUndo;
static struct { int start, len; } s_peLine[PE_MAX + 1];
static int  s_peLineN;
static unsigned char s_pePrev[SDL_NUM_SCANCODES];
static int  s_peHave;
static Uint32 s_peRep[6];

static void cm_build(int page, int sel);

static int pe_ok(char c)
{
    return c == ' ' || c == '!' || c == '&' || (c >= 0x27 && c <= 0x7A);
}

/* Into the buffer as the font can draw it: quotes become apostrophes,
 * line breaks spaces, anything else the font lacks is dropped. */
static char pe_fold(char c)
{
    if (c == '"') return '\'';
    if (c == '\n' || c == '\r' || c == '\t') return ' ';
    return pe_ok(c) ? c : 0;
}

/* src as the box holds it (single spaces, no trailing one); returns the length. */
static int pe_fold_into(const char* src, char* out)
{
    int i, n = 0;
    for (i = 0; src[i] && n < PE_MAX; i++)
    {
        char c = pe_fold(src[i]);
        if (c && !(c == ' ' && (n == 0 || out[n - 1] == ' ')))
            out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
    return n;
}

/* Width of buf[start .. start+n) as drawn: spaces draw as '_'. */
static int pe_width(int start, int n)
{
    char tmp[PE_MAX + 1];
    int  i;
    for (i = 0; i < n; i++)
        tmp[i] = s_peBuf[start + i] == ' ' ? '_' : s_peBuf[start + i];
    tmp[n] = '\0';
    return n > 0 ? Pc_LangMenuTextWidth(tmp) : 0;
}

static void pe_wrap(void)
{
    int i = 0;

    s_peLineN = 0;
    for (;;)
    {
        const int start = i;
        int       j = i, space = -1;

        while (j < s_peLen && j - start < PE_LINE_CAP)
        {
            if (s_peBuf[j] == ' ')
                space = j;
            if (pe_width(start, j - start + 1) > PE_W)
                break;
            j++;
        }
        if (j >= s_peLen)
        {
            s_peLine[s_peLineN].start = start;
            s_peLine[s_peLineN].len   = s_peLen - start;
            s_peLineN++;
            break;
        }
        if (space >= start)
            j = space + 1;
        else if (j == start)
            j++;
        s_peLine[s_peLineN].start = start;
        s_peLine[s_peLineN].len   = j - start;
        s_peLineN++;
        i = j;
    }
}

static int pe_line_of(int k)
{
    int l;
    for (l = 0; l < s_peLineN - 1; l++)
        if (k < s_peLine[l].start + s_peLine[l].len)
            return l;
    return s_peLineN - 1;
}

/* Where the caret may sit at the end of a line: before its wrap space. */
static int pe_line_end(int l)
{
    int e = s_peLine[l].start + s_peLine[l].len;
    if (l < s_peLineN - 1 && e > s_peLine[l].start && s_peBuf[e - 1] == ' ')
        e--;
    return e;
}

static void pe_keep_visible(void)
{
    int l = pe_line_of(s_peCur);
    if (l < s_peTop)
        s_peTop = l;
    if (l >= s_peTop + PE_LINES)
        s_peTop = l - PE_LINES + 1;
}

static int pe_sel(int* a, int* b)
{
    *a = s_peCur < s_peAnchor ? s_peCur : s_peAnchor;
    *b = s_peCur < s_peAnchor ? s_peAnchor : s_peCur;
    return *a != *b;
}

static void pe_snapshot(void)
{
    memcpy(s_peUndo, s_peBuf, sizeof(s_peBuf));
    s_peUndoLen  = s_peLen;
    s_peUndoCur  = s_peCur;
    s_peHaveUndo = 1;
}

static void pe_delete_range(int a, int b)
{
    memmove(&s_peBuf[a], &s_peBuf[b], (size_t)(s_peLen - b + 1));
    s_peLen -= b - a;
    s_peCur = s_peAnchor = a;
}

static void pe_insert(const char* text)
{
    int a, b;
    if (pe_sel(&a, &b))
        pe_delete_range(a, b);
    for (; *text; text++)
    {
        char c = pe_fold(*text);
        if (!c || s_peLen >= PE_MAX)
            continue;
        memmove(&s_peBuf[s_peCur + 1], &s_peBuf[s_peCur], (size_t)(s_peLen - s_peCur + 1));
        s_peBuf[s_peCur++] = c;
        s_peLen++;
    }
    s_peAnchor = s_peCur;
}

static int pe_word_left(int k)
{
    while (k > 0 && s_peBuf[k - 1] == ' ') k--;
    while (k > 0 && s_peBuf[k - 1] != ' ') k--;
    return k;
}

static int pe_word_right(int k)
{
    while (k < s_peLen && s_peBuf[k] != ' ') k++;
    while (k < s_peLen && s_peBuf[k] == ' ') k++;
    return k;
}

/* The index on line l whose x is nearest x. */
static int pe_at_x(int l, int x)
{
    const int start = s_peLine[l].start;
    const int end   = pe_line_end(l);
    int       best = start, bd = 1 << 30, k;
    for (k = start; k <= end; k++)
    {
        int d = pe_width(start, k - start) - x;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static void pe_open(void)
{
    char text[4096];

    s_peLen = pe_fold_into(PcCon_PersonaLoad(s_prof, text, (int)sizeof(text)) ? text : PcCon_DefaultPersona(s_prof),
                           s_peBuf);
    s_peCur = s_peAnchor = s_peLen;
    s_peTop      = 0;
    s_peHaveUndo = 0;
    s_peHave     = 0;
    pe_wrap();
    pe_keep_visible();
    s_pe = 1;
    cm_beep(Sfx_MenuConfirm);
}

static void pe_close(int save)
{
    if (save)
    {
        char  rel[96], def[PE_MAX + 1];
        FILE* f;
        PcCon_PersonaPath(s_prof, rel, (int)sizeof(rel));
        while (s_peLen > 0 && s_peBuf[s_peLen - 1] == ' ')
            s_peBuf[--s_peLen] = '\0';
        pe_fold_into(PcCon_DefaultPersona(s_prof), def);
        /* Empty or untouched: no file, so it follows the built-in one. */
        if (s_peLen == 0 || !strcmp(s_peBuf, def))
        {
            remove(rel);
        }
        else if ((f = fopen(rel, "wb")) != NULL)
        {
            fputs(s_peBuf, f);
            fputs("\n", f);
            fclose(f);
        }
        else
        {
            cm_beep(Sfx_MenuError);
            return;
        }
        SH_DBG("[CONSCIOUS] menu: %s personality saved (%d chars)", PcCon_ProfName(s_prof), s_peLen);
    }
    cm_beep(save ? Sfx_MenuConfirm : Sfx_MenuCancel);
    s_pe = 0;
    cm_build(PAGE_PROF, 9);
    BindNav_Reset(&s_nav);
}

static int pe_repeat(const Uint8* ks, int sc, int slot)
{
    const Uint32 now = SDL_GetTicks();
    if (!ks[sc])
        return 0;
    if (!s_pePrev[sc])
    {
        s_peRep[slot] = now + 400u;
        return 1;
    }
    if (now >= s_peRep[slot])
    {
        s_peRep[slot] = now + 35u;
        return 1;
    }
    return 0;
}

static void pe_update(const Uint8* ks)
{
    const int shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    const int ctrl  = ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
    int       a, b, sc, moved = -1, edited = 0;

#define PE_EDGE(k) (ks[k] && !s_pePrev[k])
    if (!s_peHave)
    {
        memcpy(s_pePrev, ks, SDL_NUM_SCANCODES);
        s_peHave = 1;
        return;
    }
    if (PE_EDGE(SDL_SCANCODE_ESCAPE))
    {
        pe_close(0);
        return;
    }
    if (PE_EDGE(SDL_SCANCODE_RETURN) || PE_EDGE(SDL_SCANCODE_KP_ENTER))
    {
        pe_close(1);
        return;
    }

    if (ctrl)
    {
        if (PE_EDGE(SDL_SCANCODE_A))
        {
            s_peAnchor = 0;
            s_peCur    = s_peLen;
        }
        if ((PE_EDGE(SDL_SCANCODE_C) || PE_EDGE(SDL_SCANCODE_X)) && pe_sel(&a, &b))
        {
            char clip[PE_MAX + 1];
            memcpy(clip, &s_peBuf[a], (size_t)(b - a));
            clip[b - a] = '\0';
            SDL_SetClipboardText(clip);
            if (PE_EDGE(SDL_SCANCODE_X))
            {
                pe_snapshot();
                pe_delete_range(a, b);
                edited = 1;
            }
        }
        if (PE_EDGE(SDL_SCANCODE_V))
        {
            char* clip = SDL_GetClipboardText();
            if (clip && clip[0])
            {
                pe_snapshot();
                pe_insert(clip);
                edited = 1;
            }
            SDL_free(clip);
        }
        if (PE_EDGE(SDL_SCANCODE_Z) && s_peHaveUndo)
        {
            char swap[PE_MAX + 1];
            int  len = s_peLen, cur = s_peCur;
            memcpy(swap, s_peBuf, sizeof(swap));
            memcpy(s_peBuf, s_peUndo, sizeof(s_peBuf));
            s_peLen = s_peUndoLen;
            s_peCur = s_peAnchor = s_peUndoCur;
            memcpy(s_peUndo, swap, sizeof(s_peUndo));
            s_peUndoLen = len;
            s_peUndoCur = cur;
            edited = 1;
        }
    }
    else
    {
        for (sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        {
            char c[2];
            if (!PE_EDGE(sc))
                continue;
            c[0] = PcConChat_Glyph(sc, shift);
            c[1] = '\0';
            if (c[0] && pe_fold(c[0]))
            {
                if (!edited)
                    pe_snapshot();
                pe_insert(c);
                edited = 1;
            }
        }
    }

    if (pe_repeat(ks, SDL_SCANCODE_BACKSPACE, 0))
    {
        pe_snapshot();
        if (pe_sel(&a, &b))
            pe_delete_range(a, b);
        else if (s_peCur > 0)
            pe_delete_range(ctrl ? pe_word_left(s_peCur) : s_peCur - 1, s_peCur);
        edited = 1;
    }
    if (pe_repeat(ks, SDL_SCANCODE_DELETE, 1))
    {
        pe_snapshot();
        if (pe_sel(&a, &b))
            pe_delete_range(a, b);
        else if (s_peCur < s_peLen)
            pe_delete_range(s_peCur, ctrl ? pe_word_right(s_peCur) : s_peCur + 1);
        edited = 1;
    }
    if (edited)
        pe_wrap();

    /* Movement: with Shift the anchor stays put and the selection grows. */
    if (pe_repeat(ks, SDL_SCANCODE_LEFT, 2))
    {
        if (!shift && pe_sel(&a, &b))
            moved = a;
        else
            moved = ctrl ? pe_word_left(s_peCur) : (s_peCur > 0 ? s_peCur - 1 : 0);
    }
    if (pe_repeat(ks, SDL_SCANCODE_RIGHT, 3))
    {
        if (!shift && pe_sel(&a, &b))
            moved = b;
        else
            moved = ctrl ? pe_word_right(s_peCur) : (s_peCur < s_peLen ? s_peCur + 1 : s_peLen);
    }
    if (pe_repeat(ks, SDL_SCANCODE_UP, 4) || pe_repeat(ks, SDL_SCANCODE_DOWN, 5))
    {
        const int l  = pe_line_of(s_peCur);
        const int x  = pe_width(s_peLine[l].start, s_peCur - s_peLine[l].start);
        const int to = ks[SDL_SCANCODE_UP] ? l - 1 : l + 1;
        if (to < 0)
            moved = 0;
        else if (to >= s_peLineN)
            moved = s_peLen;
        else
            moved = pe_at_x(to, x);
    }
    if (PE_EDGE(SDL_SCANCODE_HOME))
        moved = ctrl ? 0 : s_peLine[pe_line_of(s_peCur)].start;
    if (PE_EDGE(SDL_SCANCODE_END))
        moved = ctrl ? s_peLen : pe_line_end(pe_line_of(s_peCur));
    if (moved >= 0)
    {
        s_peCur = moved;
        if (!shift)
            s_peAnchor = moved;
    }
    if (edited || moved >= 0)
        pe_keep_visible();
#undef PE_EDGE
    memcpy(s_pePrev, ks, SDL_NUM_SCANCODES);
}

/* A flat box in OT2: tag 9 sits between the black backing and the text,
 * tag 7 above the text. */
static void pe_fill(int x, int y, int w, int h, int r, int g, int bl, int tag)
{
    TILE*     t    = (TILE*)GsOUT_PACKET_P;
    const int tall = g_GameWork.gsScreenHeight == FRAMEBUFFER_HEIGHT_INTERLACED;

    setTile(t);
    setRGB0(t, r, g, bl);
    setXY0(t, x - 160, tall ? (y - 112) * 2 : y - 112);
    setWH(t, w, tall ? h * 2 : h);
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[tag], t);
    GsOUT_PACKET_P = (PACKET*)(t + 1);
}

static void pe_draw(void)
{
    char head[64], name[48], count[24], disp[PE_LINE_CAP + 1];
    int  i, a, b;
    const int hasSel = pe_sel(&a, &b);
    const int caretL = pe_line_of(s_peCur);

    Nm_SanitizeGlyphs(PcCon_ProfName(s_prof), name, sizeof(name), 1);
    snprintf(head, sizeof(head), "%s_PERSONALITY", name);
    Nm_TextDrawCentered(160, CM_HEAD_Y, NM_TEXT_TALL, StringColorId_White, head);
    Nm_RuleDraw(CM_RULE_X0, CM_RULE_X1, CM_RULE1_Y, 0);

    for (i = s_peTop; i < s_peLineN && i < s_peTop + PE_LINES; i++)
    {
        const int start = s_peLine[i].start;
        const int len   = s_peLine[i].len;
        const int y     = PE_TOP + (i - s_peTop) * CM_PITCH;
        int       k;

        if (hasSel && a < start + len && b > start)
        {
            const int s0 = a > start ? a : start;
            const int s1 = b < start + len ? b : start + len;
            const int x0 = PE_X + pe_width(start, s0 - start);
            const int x1 = PE_X + pe_width(start, s1 - start);
            pe_fill(x0, y, x1 - x0 > 0 ? x1 - x0 : 3, 15, 40, 70, 140, 9);
        }
        for (k = 0; k < len && k < PE_LINE_CAP; k++)
            disp[k] = s_peBuf[start + k] == ' ' ? '_' : s_peBuf[start + k];
        disp[k] = '\0';
        if (k > 0)
            Nm_TextDrawVerbatim(PE_X, y, NM_TEXT_TALL, StringColorId_White, disp);
        if (i == caretL && (SDL_GetTicks() / 450) % 2 == 0)
            pe_fill(PE_X + pe_width(start, s_peCur - start), y, 1, 14, 200, 170, 60, 7);
    }
    if (s_peTop > 0)
        Nm_ScrollArrowDraw(160, CM_ARROW_UP_Y - 2, 1, 1);
    if (s_peTop + PE_LINES < s_peLineN)
        Nm_ScrollArrowDraw(160, PE_ARROW_Y, 0, 1);

    Nm_TextDrawCentered(160, PE_KEYS_Y, NM_TEXT_TALL, StringColorId_DarkGrey, "Shift_selects,_Ctrl+A/C/X/V/Z");
    Nm_RuleDraw(CM_RULE_X0, CM_RULE_X1, CM_RULE2_Y, 0);
    Nm_TextDraw(8, CM_FOOTER_Y, NM_TEXT_TALL, StringColorId_LightGrey, "Enter_saves,_Esc_cancels");
    snprintf(count, sizeof(count), "%d/%d", s_peLen, PE_MAX);
    Nm_TextDrawRight(312, CM_FOOTER_Y, NM_TEXT_TALL, s_peLen >= PE_MAX ? StringColorId_Red : StringColorId_LightGrey,
                     count);
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
                                                          "Night_Flutter", "Dark_Harry" };
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
        cm_add("Call_Dark_Harry", RK_STEP, ID_CALL);
        cm_add("Creativity", RK_STEP, ID_TEMP);
        break;

    default:
        /* His switch is whether he exists at all: off, and nothing can put
         * him in the map. */
        cm_add(s_prof == PCCON_PROF_DARK ? "Enabled" : "Talks", RK_TOGGLE, ID_TALK);
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
        const int talk = g_PcConProf[id - ID_PROF0].talk;
        if (id - ID_PROF0 == PCCON_PROF_DARK)
            SDL_strlcpy(out, cm_onoff(talk), (size_t)cap);
        else
            SDL_strlcpy(out, talk ? "Talks" : "Silent", (size_t)cap);
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
    case ID_CALL:     snprintf(out, (size_t)cap, "%d in 100", (int)(g_PcCon.darkHarryCallChance * 100.0f + 0.5f)); break;
    case ID_TEMP:     snprintf(out, (size_t)cap, "%.1f", g_PcCon.llmTemperature); break;

    case ID_TALK:     SDL_strlcpy(out, cm_onoff(p->talk), (size_t)cap); break;
    case ID_VOICE:
    {
        int v = cm_voice_index(p->voice);
        if (v == -1 && s_prof == PCCON_PROF_DARK)
        {
            /* Nothing chosen: the account's Harry, named if the list is in. */
            int a = cm_voice_index(Pc_Conscious_DarkVoice());
            if (a >= 0)
                snprintf(out, (size_t)cap, "%s (auto)", s_voices[a].name);
            else
                SDL_strlcpy(out, "Harry (auto)", (size_t)cap);
        }
        else if (v == -1)
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
    case ID_CALL:     g_PcCon.darkHarryCallChance = cm_stepf(g_PcCon.darkHarryCallChance, dir, 0.01f, 0.0f, 1.0f); break;
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
    s_pe = 0;
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
    case ID_PERSONA:       pe_open(); break;
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
    if (s_pe)
    {
        pe_update(ks);
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
        return "Enter_to_edit";
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
    if (s_pe)
    {
        pe_draw();
        return;
    }
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
