/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_memo.c - phrase tables, marker text, and the composer state machine.
 *
 * See sh_net_memo.h for why messages are templated rather than typed.
 *
 * The word list is ORDERED BY GROUP and the group boundaries are a separate
 * table of first-indices. That is what lets the picker page by category
 * instead of scrolling sixty-odd entries one at a time, and it means adding a
 * word is a one-line change as long as it goes in its group.
 *
 * INDICES ARE THE WIRE FORMAT. A memo placed today is stored on the server as
 * (phrase 7, word 31) forever, so REORDERING EITHER TABLE REWRITES EVERY MEMO
 * ANYONE HAS EVER LEFT. Append only.
 */

#include "game.h"

#include <stdio.h>
#include <string.h>

#include "sh_net.h"
#include "sh_net_memo.h"
#include "pc_config.h"
#include "sh_log.h"

/* Reading distance. Close enough that you have to walk up to a marker, far
 * enough that you do not have to stand exactly on it. */
#define MEMO_READ_DIST Q12(1.6f)

/* How long "Message left." stays up after a placement. */
#define MEMO_PLACED_FRAMES 150

/* APPEND ONLY - see the file header. */
static const char* const s_phrases[] = {
    "%s ahead",
    "Beware of %s",
    "Try %s",
    "Need %s",
    "No %s",
    "%s here",
    "Look for %s",
    "I can hear %s",
    "Maybe %s",
    "Turn back. %s",
    "Remember %s",
    "It was %s",
    "Why %s?",
    "So cold... %s",
    "%s",
    "Don't trust %s",
    "%s saved me",
    "%s killed me",
    "Nothing but %s",
    "Follow %s",
    "%s, I think",
    "Hidden: %s",
    "Wrong way. %s",
    "Thank you for %s"
};

/* APPEND ONLY, and only ever at the END OF ITS GROUP - see the file header. */
static const char* const s_words[] = {
    /* 0: creatures */
    "the birds", "the dogs", "the nurses", "the children",
    "the crawling things", "something big", "the twitching one",
    "the thing in the dark", "the doctors", "the wings above",
    /* 10: supplies */
    "a health drink", "ammo", "a first aid kit", "an antidote",
    "the handgun", "the shotgun", "the rifle", "the knife",
    "the hammer", "the axe", "a key", "the map",
    /* 22: the world */
    "the door", "the stairs", "the hole", "the elevator",
    "the corridor", "the window", "the gate", "the ladder",
    "the drain", "the locker", "the vent", "the roof",
    "the fence", "the car", "the radio", "the flashlight",
    /* 38: what to do */
    "running", "hiding", "listening", "waiting",
    "looking up", "looking down", "going back", "standing still",
    "the radio static", "turning it off", "reading it", "taking it",
    /* 50: places */
    "the school", "the hospital", "the sewer", "the church",
    "the alley", "the fog", "the other side", "the lighthouse",
    "the motel", "the mall", "the pier", "the amusement park",
    /* 62: feelings */
    "hope", "nothing", "a trap", "silence",
    "blood", "a dead end", "safety", "help",
    "the truth", "a dream", "the end", "a secret"
};

static const struct { int first; const char* name; } s_groups[] = {
    {  0, "THEY" },
    { 10, "SUPPLIES" },
    { 22, "THE PLACE" },
    { 38, "WHAT TO DO" },
    { 50, "WHERE" },
    { 62, "WHAT I FELT" }
};

#define PHRASE_COUNT ((int)(sizeof(s_phrases) / sizeof(s_phrases[0])))
#define WORD_COUNT   ((int)(sizeof(s_words)   / sizeof(s_words[0])))
#define GROUP_COUNT  ((int)(sizeof(s_groups)  / sizeof(s_groups[0])))

int ShNetMemo_PhraseCount(void)
{
    return PHRASE_COUNT;
}

const char* ShNetMemo_Phrase(int i)
{
    return (i >= 0 && i < PHRASE_COUNT) ? s_phrases[i] : "";
}

int ShNetMemo_WordCount(void)
{
    return WORD_COUNT;
}

const char* ShNetMemo_Word(int i)
{
    return (i >= 0 && i < WORD_COUNT) ? s_words[i] : "";
}

const char* ShNetMemo_WordGroup(int i)
{
    int g;
    for (g = 0; g < GROUP_COUNT; g++)
    {
        if (s_groups[g].first == i)
        {
            return s_groups[g].name;
        }
    }
    return NULL;
}

/* Substitutes the word into the phrase by hand rather than through snprintf:
 * the phrase strings are data, and handing user-reachable data to a printf
 * format string is how a table typo becomes a crash. */
static void ShNetMemo_Format(int phrase, int word, char* out, int cap)
{
    const char* p = ShNetMemo_Phrase(phrase);
    const char* w = ShNetMemo_Word(word);
    int         o = 0;

    if (cap <= 0)
    {
        return;
    }
    out[0] = '\0';
    if (phrase < 0 || phrase >= PHRASE_COUNT || word < 0 || word >= WORD_COUNT)
    {
        return;
    }

    while (*p && o < cap - 1)
    {
        if (p[0] == '%' && p[1] == 's')
        {
            const char* q = w;
            while (*q && o < cap - 1)
            {
                out[o++] = *q++;
            }
            p += 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
}

void ShNetMemo_Text(const ShNetMemo* m, char* out, int cap)
{
    if (!m || cap <= 0)
    {
        if (out && cap > 0) out[0] = '\0';
        return;
    }
    switch (m->kind)
    {
    case SHNET_MARK_DEATH:
        snprintf(out, (size_t)cap, "%s died here",
                 m->owner[0] ? m->owner : "Someone");
        return;
    case SHNET_MARK_SAVE:
        snprintf(out, (size_t)cap, "%s rested here",
                 m->owner[0] ? m->owner : "Someone");
        return;
    default:
        ShNetMemo_Format((int)m->phraseA, (int)m->wordA, out, cap);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

int ShNetMemo_NearestReadable(void)
{
    const VECTOR3* self;
    int            count = ShNet_MemoCount();
    int            best  = -1;
    s32            bestD = 0;
    int            i;

    if (!g_PcConfig.onlineMemos || count <= 0)
    {
        return -1;
    }
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay)
    {
        return -1;
    }
    self = &g_SysWork.playerWork.player.position;

    for (i = 0; i < count; i++)
    {
        const ShNetMemo* m = ShNet_Memo(i);
        s32              dx, dz, d;

        if (!m)
        {
            continue;
        }
        dx = ABS(m->x - self->vx);
        dz = ABS(m->z - self->vz);
        if (dx > MEMO_READ_DIST || dz > MEMO_READ_DIST)
        {
            continue;
        }
        /* Chebyshev distance is enough to rank two markers the player is
         * already standing between; a real magnitude would need a square root
         * per marker per frame to pick the same one. */
        d = (dx > dz) ? dx : dz;
        if (best < 0 || d < bestD)
        {
            best  = i;
            bestD = d;
        }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* Composer                                                            */
/* ------------------------------------------------------------------ */

static int s_active;
static int s_col;      /* 0 = phrase, 1 = word */
static int s_phrase;
static int s_word;
static int s_placedTimer;

int ShNetMemo_ComposerActive(void)
{
    return s_active;
}

void ShNetMemo_ComposerClose(void)
{
    s_active = 0;
}

void ShNetMemo_ComposerToggle(void)
{
    if (s_active)
    {
        s_active = 0;
        return;
    }
    if (!g_PcConfig.onlineEnabled || !g_PcConfig.onlineMemos)
    {
        return;
    }
    if (ShNet_Status() != SHNET_ST_CONNECTED)
    {
        return;
    }
    if (!(ShNet_ServerFlags() & SHNET_SF_MEMOS))
    {
        return;
    }
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay)
    {
        return;
    }
    s_active = 1;
    s_col    = 0;
}

int ShNetMemo_ComposerColumn(void)
{
    return s_col;
}

int ShNetMemo_ComposerPhrase(void)
{
    return s_phrase;
}

int ShNetMemo_ComposerWord(void)
{
    return s_word;
}

void ShNetMemo_ComposerPreview(char* out, int cap)
{
    ShNetMemo_Format(s_phrase, s_word, out, cap);
}

int ShNetMemo_JustPlaced(void)
{
    return s_placedTimer > 0;
}

void ShNetMemo_Tick(void)
{
    if (s_placedTimer > 0)
    {
        s_placedTimer--;
    }
}

/* Left/Right jumps a whole group in the word column. Sixty-odd words one at a
 * time is not a menu anyone will use twice. */
static void ShNetMemo_WordGroupStep(int dir)
{
    int g, cur = 0;
    for (g = 0; g < GROUP_COUNT; g++)
    {
        if (s_word >= s_groups[g].first)
        {
            cur = g;
        }
    }
    cur += dir;
    if (cur < 0)             cur = GROUP_COUNT - 1;
    if (cur >= GROUP_COUNT)  cur = 0;
    s_word = s_groups[cur].first;
}

void ShNetMemo_ComposerInput(int up, int down, int left, int right,
                             int confirm, int cancel)
{
    if (!s_active)
    {
        return;
    }

    if (cancel)
    {
        s_active = 0;
        return;
    }

    if (s_col == 0)
    {
        if (up)    s_phrase = (s_phrase + PHRASE_COUNT - 1) % PHRASE_COUNT;
        if (down)  s_phrase = (s_phrase + 1) % PHRASE_COUNT;
        if (right) s_col    = 1;
        if (left)  s_col    = 1; /* two columns: either way lands on the other */
    }
    else
    {
        if (up)    s_word = (s_word + WORD_COUNT - 1) % WORD_COUNT;
        if (down)  s_word = (s_word + 1) % WORD_COUNT;
        if (right) ShNetMemo_WordGroupStep(1);
        if (left)  ShNetMemo_WordGroupStep(-1);
    }

    if (confirm)
    {
        if (s_col == 0)
        {
            s_col = 1;
        }
        else
        {
            const VECTOR3* p = &g_SysWork.playerWork.player.position;
            if (ShNet_PlaceMarker(SHNET_MARK_MEMO, (int)p->vx, (int)p->vy, (int)p->vz,
                                  (short)g_SysWork.playerWork.player.rotation.vy,
                                  (unsigned short)s_phrase, (unsigned short)s_word,
                                  0xFFFF, 0xFFFF))
            {
                char text[96];
                ShNetMemo_Format(s_phrase, s_word, text, (int)sizeof(text));
                SH_DBG("[NET] left a message: \"%s\"", text);
                s_placedTimer = MEMO_PLACED_FRAMES;
                /* The server assigns the id and hands it back in the next
                 * marker list, so ask for one instead of guessing. */
                ShNet_RequestMemos();
            }
            s_active = 0;
        }
    }
}
