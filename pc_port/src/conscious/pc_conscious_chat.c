/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_chat.c - the online branch's chat box (sh_net_chat.c) without
 * the network: a local feed of who said what, plus the composer.
 *
 * Spoken lines from the microphone land here as the player's lines, the grey
 * children's replies land here as theirs, and typing works the same way it
 * does online (key_chat opens, Enter sends, Esc cancels). Lines are wrapped on
 * the way in, because a generated reply is far longer than typed chat.
 */

#include <stdio.h>
#include <string.h>
#include <SDL.h>

#include "pc_conscious.h"

#define CHAT_SHOW_MS   12000
#define CHAT_FADE_MS   1500
#define CHAT_LOG_MAX   18
#define CHAT_SHOWN     9
#define CHAT_WRAP      76
#define CHAT_INPUT_MAX 200

typedef struct
{
    char         text[CHAT_WRAP + 40];
    int          kind;
    unsigned int ms;
} ChatLine;

static ChatLine s_log[CHAT_LOG_MAX];
static int      s_logCount;

static int  s_open;
static char s_input[CHAT_INPUT_MAX];
static int  s_inputLen;

static unsigned char s_prevKeys[SDL_NUM_SCANCODES];
static int           s_haveKeys;

static void Chat_Push(int kind, const char* line)
{
    ChatLine* l;
    if (s_logCount == CHAT_LOG_MAX)
    {
        memmove(&s_log[0], &s_log[1], sizeof(s_log[0]) * (CHAT_LOG_MAX - 1));
        s_logCount--;
    }
    l = &s_log[s_logCount++];
    SDL_strlcpy(l->text, line, sizeof(l->text));
    l->kind = kind;
    l->ms   = SDL_GetTicks();
}

void PcConChat_Add(int kind, const char* speaker, const char* text)
{
    char        line[CHAT_WRAP + 40];
    const char* p = text;
    int         first = 1;

    if (!text || !text[0])
        return;
    while (*p)
    {
        int room = first && speaker ? CHAT_WRAP - (int)strlen(speaker) - 2 : CHAT_WRAP - 4;
        int n    = (int)strlen(p);
        if (n > room)
        {
            int cut = room;
            while (cut > room / 2 && p[cut] != ' ') cut--;
            if (p[cut] != ' ') cut = room;
            n = cut;
        }
        if (first && speaker)
            snprintf(line, sizeof(line), "%s: %.*s", speaker, n, p);
        else
            snprintf(line, sizeof(line), "    %.*s", n, p);
        Chat_Push(kind, line);
        p += n;
        while (*p == ' ') p++;
        first = 0;
    }
}

void PcConChat_Clear(void)
{
    s_logCount = 0;
}

void PcConChat_Open(void)
{
    s_open     = 1;
    s_inputLen = 0;
    s_input[0] = '\0';
    s_haveKeys = 0;
}

void PcConChat_Close(void)
{
    s_open     = 0;
    s_inputLen = 0;
    s_input[0] = '\0';
}

int PcConChat_IsOpen(void)
{
    return s_open;
}

/* SDL scancode -> the glyph it types, lower and shifted. Spelled out because
 * SDL's scancode order is US-keyboard positional, not ASCII. */
static char Chat_Glyph(int sc, int shift)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
    {
        char base = (char)('a' + (sc - SDL_SCANCODE_A));
        return shift ? (char)(base - 32) : base;
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
    {
        static const char lo[] = "123456789";
        static const char hi[] = "!@#$%^&*(";
        return shift ? hi[sc - SDL_SCANCODE_1] : lo[sc - SDL_SCANCODE_1];
    }
    switch (sc)
    {
    case SDL_SCANCODE_0:            return shift ? ')' : '0';
    case SDL_SCANCODE_SPACE:        return ' ';
    case SDL_SCANCODE_MINUS:        return shift ? '_' : '-';
    case SDL_SCANCODE_EQUALS:       return shift ? '+' : '=';
    case SDL_SCANCODE_SEMICOLON:    return shift ? ':' : ';';
    case SDL_SCANCODE_APOSTROPHE:   return shift ? '"' : '\'';
    case SDL_SCANCODE_COMMA:        return shift ? '<' : ',';
    case SDL_SCANCODE_PERIOD:       return shift ? '>' : '.';
    case SDL_SCANCODE_SLASH:        return shift ? '?' : '/';
    default:                        return '\0';
    }
}

int PcConChat_FeedKeys(const unsigned char* ks, char* submitted, int cap)
{
    int sc, shift, ctrl, sent = 0;

    if (!s_open || !ks)
        return 0;
    /* First frame after opening: seed prev from live so the opening key press
     * is not itself typed, and nothing held from gameplay leaks in. */
    if (!s_haveKeys)
    {
        memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
        s_haveKeys = 1;
        return 0;
    }

    shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    ctrl  = ks[SDL_SCANCODE_LCTRL]  || ks[SDL_SCANCODE_RCTRL];

    if (ks[SDL_SCANCODE_ESCAPE] && !s_prevKeys[SDL_SCANCODE_ESCAPE])
    {
        PcConChat_Close();
        goto done;
    }
    if ((ks[SDL_SCANCODE_RETURN] && !s_prevKeys[SDL_SCANCODE_RETURN]) ||
        (ks[SDL_SCANCODE_KP_ENTER] && !s_prevKeys[SDL_SCANCODE_KP_ENTER]))
    {
        if (s_inputLen > 0 && submitted && cap > 0)
        {
            SDL_strlcpy(submitted, s_input, (size_t)cap);
            sent = 1;
        }
        PcConChat_Close();
        goto done;
    }
    if (ks[SDL_SCANCODE_BACKSPACE] && !s_prevKeys[SDL_SCANCODE_BACKSPACE] && s_inputLen > 0)
        s_input[--s_inputLen] = '\0';

    if (!ctrl)
    {
        for (sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        {
            if (ks[sc] && !s_prevKeys[sc])
            {
                char c = Chat_Glyph(sc, shift);
                if (c && s_inputLen < CHAT_INPUT_MAX - 1)
                {
                    s_input[s_inputLen++] = c;
                    s_input[s_inputLen]   = '\0';
                }
            }
        }
    }

done:
    memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
    if (!s_open)
        s_haveKeys = 0;
    return sent;
}

static float Chat_Alpha(const ChatLine* l)
{
    unsigned int age;
    if (s_open)
        return 1.0f;
    age = SDL_GetTicks() - l->ms;
    if (age < CHAT_SHOW_MS)
        return 1.0f;
    if (age < CHAT_SHOW_MS + CHAT_FADE_MS)
        return 1.0f - (float)(age - CHAT_SHOW_MS) / (float)CHAT_FADE_MS;
    return 0.0f;
}

static int Chat_FirstVisible(void)
{
    return s_logCount > CHAT_SHOWN ? s_logCount - CHAT_SHOWN : 0;
}

int PcConChat_DisplayActive(void)
{
    int i;
    if (s_open)
        return 1;
    for (i = Chat_FirstVisible(); i < s_logCount; i++)
        if (Chat_Alpha(&s_log[i]) > 0.0f)
            return 1;
    return 0;
}

int PcConChat_LineCount(void)
{
    return s_logCount - Chat_FirstVisible();
}

static const ChatLine* Chat_At(int i)
{
    return &s_log[Chat_FirstVisible() + i];
}

const char* PcConChat_LineText(int i)  { return Chat_At(i)->text; }
int         PcConChat_LineKind(int i)  { return Chat_At(i)->kind; }
float       PcConChat_LineAlpha(int i) { return Chat_Alpha(Chat_At(i)); }
const char* PcConChat_ComposeText(void) { return s_input; }
