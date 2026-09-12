/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See sh_net_chat.h. Game-thread only. */

#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "sh_net.h"
#include "sh_net_proto.h"
#include "sh_net_chat.h"
#include "sh_log.h"

/* How long a line stays fully lit, then how long it fades, once nothing new
 * has arrived and the box is closed. While the box is open every recent line
 * is held at full. */
#define CHAT_SHOW_MS 9000
#define CHAT_FADE_MS 1500
#define CHAT_LOG_MAX 12       /* lines kept; the last few are shown */
#define CHAT_SHOWN   6        /* lines drawn when idle */
#define CHAT_INPUT_MAX 128    /* typed bytes, < SHNET_CHAT_MAX for name+": " */

typedef struct
{
    char         text[SHNET_CHAT_MAX + SHNET_NAME_MAX + 4];
    int          scope;
    unsigned int ms;          /* arrival, for the fade */
} ChatLine;

static ChatLine s_log[CHAT_LOG_MAX];
static int      s_logCount;

static int  s_open;
static int  s_scope = SHNET_CHAT_GAME; /* Y "brings up game chat" */
static int  s_hidden;
static char s_input[CHAT_INPUT_MAX];
static int  s_inputLen;

static unsigned char s_prevKeys[SDL_NUM_SCANCODES];
static int           s_haveKeys;

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

static void Chat_Push(int scope, const char* line)
{
    ChatLine* l;
    if (s_logCount == CHAT_LOG_MAX)
    {
        memmove(&s_log[0], &s_log[1], sizeof(s_log[0]) * (CHAT_LOG_MAX - 1));
        s_logCount--;
    }
    l = &s_log[s_logCount++];
    SDL_strlcpy(l->text, line, sizeof(l->text));
    l->scope = scope;
    l->ms    = SDL_GetTicks();
}

void ShNetChat_AddSystem(const char* text)
{
    if (text && text[0])
    {
        Chat_Push(SHNET_CHAT_GLOBAL, text);
    }
}

void ShNetChat_Tick(void)
{
    int          scope;
    unsigned int fromId;
    char         name[SHNET_NAME_MAX];
    char         text[SHNET_CHAT_MAX];
    char         line[sizeof(s_log[0].text)];

    while (ShNet_PopChat(&scope, &fromId, name, sizeof(name), text, sizeof(text)))
    {
        snprintf(line, sizeof(line), "%s: %s", name[0] ? name : "?", text);
        Chat_Push(scope, line);
    }
}

/* ------------------------------------------------------------------ */
/* Compose                                                             */
/* ------------------------------------------------------------------ */

void ShNetChat_Open(void)
{
    s_open      = 1;
    s_inputLen  = 0;
    s_input[0]  = '\0';
    s_haveKeys  = 0; /* so the key that opened us is not read as a keystroke */
}

void ShNetChat_CycleScope(void)
{
    s_scope = (s_scope == SHNET_CHAT_GLOBAL) ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL;
    if (!s_open)
    {
        ShNetChat_Open();
    }
}

void ShNetChat_Close(void)
{
    s_open     = 0;
    s_inputLen = 0;
    s_input[0] = '\0';
}

int ShNetChat_IsOpen(void)
{
    return s_open;
}

void ShNetChat_SetHidden(int hidden)
{
    s_hidden = hidden ? 1 : 0;
    if (s_hidden)
    {
        ShNetChat_Close();
    }
}

int ShNetChat_Hidden(void)
{
    return s_hidden;
}

static void Chat_Submit(void)
{
    if (s_inputLen > 0)
    {
        if (ShNet_Status() == SHNET_ST_CONNECTED)
        {
            /* No local echo: the server sends the line back to everyone
             * including us, so it lands in the same list, in order. */
            ShNet_SendChat(s_scope, s_input);
        }
        else
        {
            ShNetChat_AddSystem("[not connected to a server]");
        }
    }
    ShNetChat_Close();
}

/* ------------------------------------------------------------------ */
/* Keyboard -> text                                                    */
/* ------------------------------------------------------------------ */

/* SDL scancode -> the glyph it types, lower and shifted. Only the printable
 * keys a chat line needs; everything else is ignored. Digits and the symbol
 * row are spelled out because SDL's scancode order is US-keyboard positional,
 * not ASCII. */
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
    case SDL_SCANCODE_LEFTBRACKET:  return shift ? '{' : '[';
    case SDL_SCANCODE_RIGHTBRACKET: return shift ? '}' : ']';
    case SDL_SCANCODE_BACKSLASH:    return shift ? '|' : '\\';
    case SDL_SCANCODE_SEMICOLON:    return shift ? ':' : ';';
    case SDL_SCANCODE_APOSTROPHE:   return shift ? '"' : '\'';
    case SDL_SCANCODE_GRAVE:        return shift ? '~' : '`';
    case SDL_SCANCODE_COMMA:        return shift ? '<' : ',';
    case SDL_SCANCODE_PERIOD:       return shift ? '>' : '.';
    case SDL_SCANCODE_SLASH:        return shift ? '?' : '/';
    default:                        return '\0';
    }
}

static void Chat_Append(char c)
{
    if (c && s_inputLen < CHAT_INPUT_MAX - 1)
    {
        s_input[s_inputLen++] = c;
        s_input[s_inputLen]   = '\0';
    }
}

void ShNetChat_FeedKeys(const unsigned char* ks)
{
    int sc;
    int shift;
    int ctrl;

    if (!s_open || !ks)
    {
        return;
    }
    /* First frame after opening: seed prev from live so the opening key press
     * (Y / U) is not itself typed, and nothing held from gameplay leaks in. */
    if (!s_haveKeys)
    {
        memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
        s_haveKeys = 1;
        return;
    }

    shift = ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT];
    ctrl  = ks[SDL_SCANCODE_LCTRL]  || ks[SDL_SCANCODE_RCTRL];

    if (ks[SDL_SCANCODE_ESCAPE] && !s_prevKeys[SDL_SCANCODE_ESCAPE])
    {
        ShNetChat_Close();
        goto done;
    }
    if ((ks[SDL_SCANCODE_RETURN] && !s_prevKeys[SDL_SCANCODE_RETURN]) ||
        (ks[SDL_SCANCODE_KP_ENTER] && !s_prevKeys[SDL_SCANCODE_KP_ENTER]))
    {
        Chat_Submit();
        goto done;
    }
    /* Tab always cycles the channel; U cycles too, but only while nothing has
     * been typed yet, so it stays a normal letter once you are writing. */
    if (ks[SDL_SCANCODE_TAB] && !s_prevKeys[SDL_SCANCODE_TAB])
    {
        s_scope = (s_scope == SHNET_CHAT_GLOBAL) ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL;
    }
    else if (s_inputLen == 0 && ks[SDL_SCANCODE_U] && !s_prevKeys[SDL_SCANCODE_U])
    {
        s_scope = (s_scope == SHNET_CHAT_GLOBAL) ? SHNET_CHAT_GAME : SHNET_CHAT_GLOBAL;
        goto done; /* consume it: don't also type 'u' */
    }

    if (ks[SDL_SCANCODE_BACKSPACE] && !s_prevKeys[SDL_SCANCODE_BACKSPACE] && s_inputLen > 0)
    {
        s_input[--s_inputLen] = '\0';
    }

    if (!ctrl)
    {
        for (sc = 0; sc < SDL_NUM_SCANCODES; sc++)
        {
            if (ks[sc] && !s_prevKeys[sc])
            {
                Chat_Append(Chat_Glyph(sc, shift));
            }
        }
    }

done:
    memcpy(s_prevKeys, ks, SDL_NUM_SCANCODES);
    if (!s_open)
    {
        s_haveKeys = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Draw-facing                                                         */
/* ------------------------------------------------------------------ */

static float Chat_Alpha(const ChatLine* l)
{
    unsigned int age;
    if (s_open)
    {
        return 1.0f; /* everything held up while composing */
    }
    age = SDL_GetTicks() - l->ms;
    if (age <= CHAT_SHOW_MS)
    {
        return 1.0f;
    }
    if (age >= CHAT_SHOW_MS + CHAT_FADE_MS)
    {
        return 0.0f;
    }
    return 1.0f - (float)(age - CHAT_SHOW_MS) / (float)CHAT_FADE_MS;
}

/* First visible index into s_log for this frame (older lines fully faded). */
static int Chat_FirstVisible(void)
{
    int start = s_logCount - CHAT_SHOWN;
    int i;
    if (start < 0)
    {
        start = 0;
    }
    if (!s_open)
    {
        while (start < s_logCount && Chat_Alpha(&s_log[start]) <= 0.0f)
        {
            start++;
        }
    }
    (void)i;
    return start;
}

int ShNetChat_DisplayActive(void)
{
    if (s_hidden || !ShNet_Enabled())
    {
        return 0;
    }
    if (s_open)
    {
        return 1;
    }
    return Chat_FirstVisible() < s_logCount;
}

int ShNetChat_LineCount(void)
{
    if (s_hidden)
    {
        return 0;
    }
    return s_logCount - Chat_FirstVisible();
}

static const ChatLine* Chat_At(int i)
{
    int idx = Chat_FirstVisible() + i;
    return (idx >= 0 && idx < s_logCount) ? &s_log[idx] : NULL;
}

const char* ShNetChat_LineText(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? l->text : "";
}

int ShNetChat_LineScope(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? l->scope : SHNET_CHAT_GLOBAL;
}

float ShNetChat_LineAlpha(int i)
{
    const ChatLine* l = Chat_At(i);
    return l ? Chat_Alpha(l) : 0.0f;
}

int ShNetChat_Composing(void)
{
    return s_open;
}

int ShNetChat_Scope(void)
{
    return s_scope;
}

const char* ShNetChat_ComposeText(void)
{
    return s_input;
}
