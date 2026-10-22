/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious.c - monsters that talk back.
 *
 * Two scenes share one conversation engine (history, LLM turn, judge, TTS,
 * speech bubble):
 *
 * ALLEY - the grey children at the end of the opening alley (map0_s00).
 * Retail: after "What is this?" every grey child is re-initialised into
 * StalkerControl_2 and hunts Harry down; dying there is how the game moves on
 * to the cafe. Here they wake the same way, at the same moment, but
 * Ai_Stalker_Control_2 asks Pc_Conscious_StalkerCalm() first. While the answer
 * is yes the child walks up, stops a few steps short and stands facing Harry
 * instead of attacking, and the conversation runs:
 *
 *   they announce themselves -> Harry answers (mic on V, or typed chat on Y)
 *   -> one LLM call writes their reply, a second decides what it MEANS:
 *        ATTACK  - the answer flips to no for every child, so the unmodified
 *                  retail AI takes over and they kill him;
 *        RELEASE - they back off and leave him be for grace_seconds, then
 *                  come back at him. If he walks back to the fence that
 *                  blocks the way out, the game moves on to the cafe exactly
 *                  as if he had died;
 *        TALK    - they wait for his next answer.
 *
 * Patience runs out after patience_seconds or max_turns whatever is said.
 *
 * FIELD - Groaners, Rompers and Air Screamers anywhere. When Harry speaks
 * within monster_hear_range they stop and listen (their AI files ask
 * Pc_Conscious_MonsterOrder()); a line of at least min_words_first words opens
 * a conversation and is what the first reply answers. One of them (the first
 * to hear) talks for the group, in a voice drawn from monster_voices; others
 * that come in range join and listen. ATTACK sets every listener loose while
 * the line is still being said, RELEASE spares them for good, and walking
 * past monster_flee_range or hitting one ends it at once: they say something
 * about it and attack.
 *
 * Only this file reads game state. V is Circle and Y does nothing anywhere
 * else; both only become the mic and chat keys while the mic icon is up.
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
#include "pc_conscious_hook.h"
#include "sh_log.h"

#define NPC_N       ((int)ARRAY_SIZE(g_SysWork.npcs))
#define SLOT_N      64
#define HIST_MAX    32
#define HIST_SENT   22
#define NPC_NAME    "Grey Child"
#define THINK_LIMIT 45.0f
#define VOICE_MAX   16

/* attackReceived on a bite from another monster. A valid D_800AD4C8 row (the
 * Air Screamer indexes it), and not a weapon, so a health drop that carries it
 * is the fight's doing rather than Harry's. */
#define FIGHT_ATTACK  55
#define FIGHT_STRIKE  0.9f    /* seconds a bite takes */
#define FIGHT_LAND    0.45f   /* seconds left when it connects */
#define FIGHT_MAX     90.0f
#define COMBAT_QUIET  25.0f   /* silence that ends the shouting while they attack */

/* The fence that closes the way back out of alley 3 once the nightmare has
 * started (map0_s00 point of interest 38, "Blocked by a fence"). */
#define FENCE_X     (-259.5f)
#define FENCE_Z     (241.0f)
#define FENCE_REACH 2.5f

typedef enum
{
    CS_OFF = 0,
    CS_ARMED,     /* alley: children spawned, still dormant: the opener is being written */
    CS_AWAKE,     /* alley: awake and walking up; speaks as soon as the opener is in */
    CS_HEARING,   /* field: frozen while Harry's first words are transcribed */
    CS_SPEAKING,
    CS_LISTEN,
    CS_THINKING,
    CS_RELEASED,
    CS_HOSTILE
} e_ConState;

enum { MODE_ALLEY = 0, MODE_FIELD };

typedef struct
{
    s8     chara;     /* charaId the marks below belong to: a reused slot drops them */
    u8     part;      /* in the conversation, holding still */
    u8     spared;    /* let him go: holds still for good */
    u8     angered;   /* never listens again */
    u8     combat;    /* attacking him, but still answers when he talks */
    u8     foe1;      /* 1 + slot it is fighting, Harry having turned them on each other */
    u8     look1;     /* 1 + slot it faces while holding (the one it argues with) */
    s8     voice;
    q19_12 health;
    q19_12 health0;   /* when it joined: a bite takes a share of this */
    float  strikeT;   /* > 0: mid-bite */
    float  coolT;
} FldMon;

static int          s_init;
static int          s_state;
static int          s_mode;
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
static int          s_fenceDone;

static PcConResult  s_opener;
static int          s_openerReady;

static unsigned int s_clip;
static int          s_speaker = -1;
static float        s_textHold;
static char         s_line[400];      /* what the speaker is saying, for the bubble */
static float        s_lineT, s_lineDur;
static float        s_bubbleLinger;

static unsigned int s_calmSeenMs[SLOT_N];
static float        s_holdX[SLOT_N], s_holdZ[SLOT_N];

static FldMon       s_fld[SLOT_N];
static int          s_leader = -1;
static int          s_fldAngry;       /* the conversation already turned into an attack */
static int          s_fldMap = -1;
static int          s_candN;
static char         s_note[512];      /* what happened since his last line, for the next turn */
static int          s_turnSpk = -1;   /* slot the line being written is for; -1 = the leader */
static int          s_argue;          /* 1 A confronts B, 2 B answers, 3 A decides; 0 = no argument */
static int          s_argA = -1, s_argB = -1;
static int          s_fight;          /* two of them are at each other's throats */
static float        s_fightT;
static char         s_lastHarry[512];
static char         s_voices[VOICE_MAX][40];
static int          s_voiceN;
static int          s_lastVoice = -1;

static struct { int assistant; char* text; } s_hist[HIST_MAX];
static int s_histN;

static SDL_Scancode s_scMic  = SDL_SCANCODE_UNKNOWN;
static SDL_Scancode s_scChat = SDL_SCANCODE_UNKNOWN;
static int          s_prevMic, s_prevChat;
static int          s_swallow;
static int          s_micMaskHold;
static int          s_iconUp;
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

static const char s_fieldRules[] =
    "The man in front of you is %s, a writer searching this nightmare town for his missing "
    "seven-year-old daughter Cheryl. He carries weapons and has been killing creatures like you all day. "
    "You were hunting him, about to attack, when he spoke to you -- and that is the only reason you stopped. "
    "You know exactly what you are and you are not ashamed of it. "
    "Personality: aggressive, hungry, proud and easily insulted, but darkly funny and curious enough to "
    "hear him out. You are a monster, not a helper: never polite, never useful to him. "
    "Rules: reply with ONE to THREE short sentences, under 40 words, meant to be spoken out loud. "
    "No stage directions, no asterisks, no emojis, no narration, no quotation marks around your line. "
    "Stay in character no matter what he says; you know nothing about video games, AI or the real world. "
    "If he insults you, threatens you, mocks you, bores you or talks nonsense, you attack him: say so plainly. "
    "If he genuinely convinces you -- clever, funny, brave, offers you something you really want, or "
    "flatters you in a way that lands -- you MAY let him go: say it plainly (tell him to get lost) and warn him. "
    "Otherwise question him, bargain, threaten what MIGHT happen. "
    "Lines in [brackets] describe what is happening; never repeat the brackets.";

static const char* const s_fallback[] = {
    "Hey! HEY! This is OUR alley, grown-up. Who are you and what do you want?",
    "Speak up, stranger. We are getting bored... and bored makes us hungry.",
    "Nobody comes down here. Nobody! So why are YOU here?",
};

static const char* const s_fieldFallback[] = {
    "You... talk? Meat does not talk. Say that again.",
    "Hrrrh. Speak slower, little man. I am deciding if I eat you.",
    "Enough. Your words are wind. I want your bones.",
};

/* ------------------------------------------------------------------ */
/* Monster kinds                                                       */
/* ------------------------------------------------------------------ */

static int Mon_Supported(int charaId)
{
    return charaId == Chara_Groaner || charaId == Chara_Romper ||
           charaId == Chara_AirScreamer || charaId == Chara_NightFlutter;
}

static const char* Mon_Name(int charaId)
{
    switch (charaId)
    {
    case Chara_Groaner:     return "Groaner";
    case Chara_Romper:      return "Romper";
    case Chara_AirScreamer: return "Air Screamer";
    case Chara_NightFlutter:return "Night Flutter";
    case Chara_GreyChild:   return NPC_NAME;
    default:                return "Monster";
    }
}

static const char* Mon_Self(int charaId)
{
    switch (charaId)
    {
    case Chara_Groaner:
        return "You are a GROANER: a skinless, mutated, dog-like beast that prowls the foggy streets of "
               "Silent Hill (1999) on four legs. You think and talk like a vicious, hungry dog that learned "
               "words: meat, bones, chewing, sniffing, chasing, your pack and your territory are everything. "
               "You growl, you get distracted by smells, you are proud of your teeth and your speed, "
               "and you hate being called a good boy unless he really means it.";
    case Chara_Romper:
        return "You are a ROMPER: a hulking, hunched, ape-like humanoid monster with long arms and a huge "
               "biting mouth, haunting the Otherworld of Silent Hill (1999). You hop and leap around, and "
               "your favourite thing in the world is to pounce on someone, straddle them, pin them down and "
               "bite. Brutish, loud, a bit dim, very proud of your jumping and your strength.";
    case Chara_NightFlutter:
        return "You are a NIGHT FLUTTER: a leathery flying creature like a small pterodactyl, but with a "
               "writhing bundle of worms where your face should be, gliding through the dark of Silent Hill "
               "(1999). You think like a shrieking bird of prey: the sky, diving, swooping, your nest and "
               "tearing things apart. You look down on anything stuck on the ground.";
    default:
        return "You are an AIR SCREAMER: a featherless, leathery flying creature like a small pterodactyl, "
               "with a long beak and screeching voice, circling the foggy sky of Silent Hill (1999). You think "
               "like a shrieking bird of prey: the sky, diving, swooping, your nest and pecking things apart. "
               "You look down on anything stuck on the ground and you are vain about your wings.";
    }
}

/* What it looks like from the game camera, so it can find itself in the shot. */
static const char* Mon_Look(int charaId)
{
    switch (charaId)
    {
    case Chara_Groaner:     return "a raw, skinless, reddish dog-like beast on four legs";
    case Chara_Romper:      return "a hunched, grey, ape-like creature with long arms";
    case Chara_AirScreamer: return "a winged, beaked, leathery creature, flying or crouched on the ground";
    case Chara_NightFlutter:return "a winged, leathery creature with worms for a face";
    default:                return "small grey child-sized figures holding knives";
    }
}

static const char s_vision[] =
    "Each line from %s comes with a picture: exactly what the game camera shows right now, a dark, "
    "foggy, over-the-shoulder view. %s is the fair-haired man in a jacket, usually near the middle. "
    "If you see %s in it, that is you%s. "
    "Use the picture when it matters: if he points at something (a car, a building, a sign, a door, you), "
    "answer about what is really there. Do not describe the scene unprompted. "
    "If something is too dark or foggy to make out, say so in character.";

/* Height of the bubble above the model's root. */
static float Mon_HeadY(int charaId)
{
    switch (charaId)
    {
    case Chara_Groaner:     return 0.85f;
    case Chara_Romper:      return 1.45f;
    case Chara_AirScreamer:
    case Chara_NightFlutter:return 1.2f;
    default:                return 1.45f;
    }
}

static void Voices_Parse(void)
{
    const char* p = g_PcCon.monsterVoices;
    s_voiceN = 0;
    while (*p && s_voiceN < VOICE_MAX)
    {
        int n = 0;
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        while (*p && *p != ',' && !isspace((unsigned char)*p) && n < (int)sizeof(s_voices[0]) - 1)
            s_voices[s_voiceN][n++] = *p++;
        while (*p && *p != ',') p++;
        s_voices[s_voiceN][n] = '\0';
        if (n)
            s_voiceN++;
    }
}

static int Voices_Pick(void)
{
    int v;
    if (s_voiceN == 0)
        return -1;
    v = rand() % s_voiceN;
    if (s_voiceN > 1 && v == s_lastVoice)
        v = (v + 1 + rand() % (s_voiceN - 1)) % s_voiceN;
    s_lastVoice = v;
    return v;
}

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

static int Fld_LeaderChara(void)
{
    if (s_leader < 0 || s_leader >= NPC_N)
        return Chara_None;
    return g_SysWork.npcs[s_leader].model.charaId;
}

static const char* Con_SlotName(int slot)
{
    if (slot < 0 || slot >= NPC_N)
        return "Monster";
    return Mon_Name(g_SysWork.npcs[slot].model.charaId);
}

static const char* Con_SpeakerName(void)
{
    return s_mode == MODE_FIELD ? Mon_Name(Fld_LeaderChara()) : NPC_NAME;
}

/* "Romper", or "other Groaner" when both are Groaners. */
static void Con_OtherLabel(int self, int other, char* out, int cap)
{
    int same = g_SysWork.npcs[self].model.charaId == g_SysWork.npcs[other].model.charaId;
    snprintf(out, cap, "%s%s", same ? "other " : "", Con_SlotName(other));
}

static const char s_argueRules[] =
    " Right now you are not talking to %s: you are arguing with the %s, another monster standing here with you, "
    "because of something %s said. You are monsters, not friends -- you have never trusted each other. "
    "Rules: reply with ONE or TWO short sentences, under 35 words, spoken out loud TO the %s. "
    "No stage directions, no asterisks, no emojis, no narration, no quotation marks around your line. "
    "Stay in character; you know nothing about video games, AI or the real world. "
    "Lines in [brackets] describe what is happening; never repeat the brackets.";

static void Fld_System(char* out, int cap)
{
    int  counts[4] = { 0, 0, 0, 0 };
    int  kinds[4]  = { Chara_Groaner, Chara_Romper, Chara_AirScreamer, Chara_NightFlutter };
    int  i, k, others = 0;
    char rules[sizeof(s_fieldRules) + 64];
    char group[640];

    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (!s_fld[i].part || i == s_leader)
            continue;
        for (k = 0; k < 4; k++)
            if (g_SysWork.npcs[i].model.charaId == kinds[k])
                counts[k]++, others++;
    }
    group[0] = '\0';
    if (others)
    {
        strcat(group, " Other monsters stopped to listen with you:");
        for (k = 0; k < 4; k++)
        {
            if (!counts[k])
                continue;
            snprintf(group + strlen(group), sizeof(group) - strlen(group), " %d %s%s,", counts[k],
                     Mon_Name(kinds[k]), counts[k] > 1 ? "s" : "");
        }
        group[strlen(group) - 1] = '.';
        strcat(group, " You lead them and you do the talking for all of you.");
    }
    if (s_leader >= 0 && s_fld[s_leader].foe1)
    {
        char other[48];
        Con_OtherLabel(s_leader, s_fld[s_leader].foe1 - 1, other, sizeof(other));
        snprintf(group + strlen(group), sizeof(group) - strlen(group),
                 " Because of what he said you are in the middle of a fight with the %s right now; "
                 "you shout at him between bites.", other);
    }
    else if (s_fldAngry)
    {
        snprintf(group + strlen(group), sizeof(group) - strlen(group),
                 " You are attacking him right now and you will not stop; you still shout and taunt him "
                 "while you go for him.");
    }
    snprintf(rules, sizeof(rules), s_fieldRules, g_PcCon.playerName);
    snprintf(out, cap, "%s%s %s", Mon_Self(Fld_LeaderChara()), group, rules);
}

/* What the speaker of an argument line sees: who it is, who it is arguing
 * with, and the scene so far as a transcript (not as its own past turns --
 * half of those lines were the other monster's). */
static char* Con_BuildArgueMsgs(int spk, int other, const char* instruction)
{
    char   sys[2048];
    char   label[48];
    char*  user;
    char*  out;
    size_t cap = 1024 + strlen(instruction) + strlen(s_note);
    int    i, from = s_histN > 10 ? s_histN - 10 : 0;

    for (i = from; i < s_histN; i++)
        cap += strlen(s_hist[i].text) + 48;
    user = (char*)malloc(cap);
    if (!user)
        return NULL;
    Con_OtherLabel(spk, other, label, sizeof(label));
    snprintf(sys, sizeof(sys), "%s", Mon_Self(g_SysWork.npcs[spk].model.charaId));
    snprintf(sys + strlen(sys), sizeof(sys) - strlen(sys), s_argueRules, g_PcCon.playerName, label,
             g_PcCon.playerName, label);

    snprintf(user, cap, "WHAT HAS HAPPENED SO FAR:\n");
    for (i = from; i < s_histN; i++)
    {
        if (s_hist[i].assistant)
            snprintf(user + strlen(user), cap - strlen(user), "The %s: %s\n", Con_SlotName(s_argA), s_hist[i].text);
        else
            snprintf(user + strlen(user), cap - strlen(user), "%s\n", s_hist[i].text);
    }
    if (s_note[0])
        snprintf(user + strlen(user), cap - strlen(user), "%s\n", s_note);
    snprintf(user + strlen(user), cap - strlen(user), "\nYou are the %s. %s",
             Con_SlotName(spk), instruction);

    cap = strlen(sys) * 2 + strlen(user) * 2 + 96;
    out = (char*)malloc(cap);
    if (out)
    {
        strcpy(out, "[{\"role\":\"system\",\"content\":\"");
        PcCon_JsonEscapeCat(out, (int)cap, sys);
        strcat(out, "\"},{\"role\":\"user\",\"content\":\"");
        PcCon_JsonEscapeCat(out, (int)cap - 8, user);
        strcat(out, "\"}]");
    }
    free(user);
    return out;
}

static char* Con_BuildMsgs(void)
{
    char   sys[sizeof(s_persona) + sizeof(s_fieldRules) + sizeof(s_vision) + 1024];
    size_t cap = sizeof(sys) * 2 + 256;
    char*  out;
    int    i, from;

    for (i = 0; i < s_histN; i++)
        cap += strlen(s_hist[i].text) * 2 + 48;
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    if (s_mode == MODE_FIELD)
        Fld_System(sys, (int)sizeof(sys));
    else
        snprintf(sys, sizeof(sys), s_persona, g_PcCon.playerName);
    if (g_PcCon.vision)
    {
        int field = s_mode == MODE_FIELD;
        snprintf(sys + strlen(sys), sizeof(sys) - strlen(sys), " ");
        snprintf(sys + strlen(sys), sizeof(sys) - strlen(sys), s_vision, g_PcCon.playerName, g_PcCon.playerName,
                 Mon_Look(field ? Fld_LeaderChara() : Chara_GreyChild),
                 field ? " (any other monsters near you are the ones listening with you)" : " and your gang");
    }
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
    size_t      cap = 640;
    char*       out;
    int         i, from = s_histN > 12 ? s_histN - 12 : 0;
    const char* who = s_mode == MODE_FIELD ? Mon_Name(Fld_LeaderChara()) : "GREY CHILDREN";

    for (i = from; i < s_histN; i++)
        cap += strlen(s_hist[i].text) + 32;
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    snprintf(out, cap,
             "The monster: %s. Exchanges so far: %d (they lose patience around %d). "
             "Times they already let him go: %d.\nCONVERSATION:\n",
             who, s_turns, s_mode == MODE_FIELD ? g_PcCon.monsterMaxTurns : g_PcCon.maxTurns, s_releases);
    for (i = from; i < s_histN; i++)
    {
        if (s_hist[i].assistant)
        {
            strcat(out, who);
            strcat(out, ": ");
        }
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
    if (npc < 0 || npc >= NPC_N || g_SysWork.npcs[npc].model.charaId == Chara_None)
        return 0.6f;
    d = Con_DistToPlayer(npc);
    if (d <= 3.0f)
        return 1.0f;
    d = 1.0f - (d - 3.0f) / (g_PcCon.hearRange - 3.0f);
    return d < 0.0f ? 0.0f : d;
}

/* PsyCross: the world pass's ortho rect in prim coordinates, which maps onto
 * the presented picture -- the same viewport the post-capture HUD draws in,
 * so the fraction below holds under hfov/vfov, pillarbox and Hor+ alike. */
extern float g_PcWorldOrthoRect[4];

/* Just above the head, as a fraction of the picture. 0 when behind the camera. */
static int Con_HeadOnScreen(int npc, float* fx, float* fy)
{
    MATRIX               mat;
    SVECTOR              v;
    long                 sxy = 0, p = 0, flag = 0, otz;
    float                x, y, w, h;
    const s_SubCharacter* c;

    if (npc < 0 || npc >= NPC_N || g_SysWork.npcs[npc].model.charaId == Chara_None)
        return 0;
    c = &g_SysWork.npcs[npc];
    memset(&v, 0, sizeof(v));
    Vw_WorldScreenMatrixAtPositionGet(&mat, c->position.vx,
                                      c->position.vy - (q19_12)(Mon_HeadY(c->model.charaId) * 4096.0f),
                                      c->position.vz);
    SetRotMatrix(&mat);
    SetTransMatrix(&mat);
    otz = RotTransPers(&v, &sxy, &p, &flag);
    if (otz <= 8)
        return 0;
    x = (float)(s16)(sxy & 0xFFFF);
    y = (float)(s16)((u32)sxy >> 16);
    w = g_PcWorldOrthoRect[1] - g_PcWorldOrthoRect[0];
    h = g_PcWorldOrthoRect[3] - g_PcWorldOrthoRect[2];
    if (w == 0.0f || h == 0.0f)
        return 0;
    *fx = (x - g_PcWorldOrthoRect[0]) / w;
    *fy = (y - g_PcWorldOrthoRect[2]) / h;
    if (*fx < -0.2f || *fx > 1.2f || *fy < -0.2f || *fy > 1.2f)
        return 0;
    if (*fx < 0.12f) *fx = 0.12f;
    if (*fx > 0.88f) *fx = 0.88f;
    if (*fy < 0.10f) *fy = 0.10f;
    if (*fy > 0.85f) *fy = 0.85f;
    return 1;
}

static int Con_Awake(void)
{
    const unsigned int now = SDL_GetTicks();
    int i;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
        if (s_calmSeenMs[i] && now - s_calmSeenMs[i] < 500)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Field monsters                                                      */
/* ------------------------------------------------------------------ */

static int Fld_Alive(int i)
{
    return Mon_Supported(g_SysWork.npcs[i].model.charaId) && g_SysWork.npcs[i].health > 0;
}

/* Marks belong to whatever stood in the slot when they were set: a slot
 * reused by a new spawn, a death or a new map clears them. */
static void Fld_Sync(void)
{
    int i;
    int map = g_SavegamePtr ? g_SavegamePtr->mapIdx : -1;

    if (map != s_fldMap)
    {
        memset(s_fld, 0, sizeof(s_fld));
        s_fldMap = map;
    }
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (s_fld[i].chara && (s_fld[i].chara != g_SysWork.npcs[i].model.charaId || g_SysWork.npcs[i].health <= 0))
        {
            memset(&s_fld[i], 0, sizeof(s_fld[i]));
            if (i == s_leader)
                s_leader = -1;
        }
    }
    if (s_leader < 0)
    {
        float bd = 1e9f;
        for (i = 0; i < NPC_N && i < SLOT_N; i++)
        {
            if ((s_fld[i].part || s_fld[i].combat) && Con_DistToPlayer(i) < bd)
            {
                bd       = Con_DistToPlayer(i);
                s_leader = i;
            }
        }
    }
}

static int Fld_Candidate(int i)
{
    return Fld_Alive(i) && !s_fld[i].part && !s_fld[i].spared && !s_fld[i].angered &&
           Con_DistToPlayer(i) <= g_PcCon.monsterHearRange;
}

static int Fld_Candidates(void)
{
    int i, n = 0;
    if (!g_PcCon.monsters || g_SavegamePtr == NULL || Con_InScene())
        return 0;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
        if (Fld_Candidate(i))
            n++;
    return n;
}

static int Fld_Parts(float* nearest)
{
    int   i, n = 0;
    float bd = 1e9f;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (!s_fld[i].part)
            continue;
        n++;
        if (Con_DistToPlayer(i) < bd)
            bd = Con_DistToPlayer(i);
    }
    if (nearest) *nearest = bd;
    return n;
}

static void Fld_Join(int i, int announce)
{
    FldMon* m = &s_fld[i];
    m->chara  = (s8)g_SysWork.npcs[i].model.charaId;
    m->part   = 1;
    m->health = g_SysWork.npcs[i].health;
    if (m->health0 <= 0)
        m->health0 = m->health;
    if (m->voice == 0)
        m->voice = (s8)(Voices_Pick() + 1);
    if (s_leader < 0)
        s_leader = i;
    SH_DBG("[CONSCIOUS] %s (slot %d) listens%s, voice %d", Mon_Name(m->chara), i,
           i == s_leader ? " and leads" : "", m->voice - 1);
    if (announce)
    {
        char ev[128];
        snprintf(ev, sizeof(ev), "[A %s stops to listen.]", Mon_Name(m->chara));
        PcConChat_Add(PCCHAT_SYSTEM, NULL, ev);
        snprintf(ev, sizeof(ev), "[Another monster, a %s, has come up and is listening too.] ", Mon_Name(m->chara));
        SDL_strlcat(s_note, ev, sizeof(s_note));
    }
}

static void Fld_JoinAll(int announce)
{
    int i;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
        if (Fld_Candidate(i))
            Fld_Join(i, announce);
}

static const char* Fld_Voice(int slot)
{
    int v;
    if (slot < 0 || slot >= SLOT_N)
        return NULL;
    if (s_fld[slot].voice == 0)
        s_fld[slot].voice = (s8)(Voices_Pick() + 1);
    v = s_fld[slot].voice - 1;
    return (v >= 0 && v < s_voiceN) ? s_voices[v] : NULL;
}

static void Fld_Calm(FldMon* m)
{
    m->foe1    = 0;
    m->look1   = 0;
    m->strikeT = 0.0f;
}

/* Everyone listening attacks now: the AI hooks see "no" from this frame on.
 * They stay in the scene as combatants, so he can still be answered. */
static void Fld_Unleash(void)
{
    int i;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (s_fld[i].part)
        {
            s_fld[i].part    = 0;
            s_fld[i].angered = 1;
            s_fld[i].combat  = 1;
            Fld_Calm(&s_fld[i]);
        }
    }
    s_fight    = 0;
    s_fldAngry = 1;
}

static void Fld_Spare(void)
{
    int i;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (s_fld[i].part)
        {
            s_fld[i].part   = 0;
            s_fld[i].spared = 1;
            Fld_Calm(&s_fld[i]);
        }
    }
    s_fight = 0;
}

/* Nearest one still attacking him: who shouts back when he talks. */
static int Fld_Combatant(float* nearest)
{
    int   i, best = -1;
    float bd = 1e9f;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (!s_fld[i].combat || !Fld_Alive(i))
            continue;
        if (Con_DistToPlayer(i) < bd)
        {
            bd   = Con_DistToPlayer(i);
            best = i;
        }
    }
    if (nearest) *nearest = bd;
    return best;
}

static float Con_DistNpc(int a, int b)
{
    const float dx = (float)(g_SysWork.npcs[a].position.vx - g_SysWork.npcs[b].position.vx) / 4096.0f;
    const float dz = (float)(g_SysWork.npcs[a].position.vz - g_SysWork.npcs[b].position.vz) / 4096.0f;
    return sqrtf(dx * dx + dz * dz);
}

static float Con_Rand01(void)
{
    return (float)(rand() % 10000) / 10000.0f;
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
    int i;
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
    s_line[0]        = '\0';
    s_note[0]        = '\0';
    s_fldAngry       = 0;
    s_leader         = -1;
    s_turnSpk        = -1;
    s_argue          = 0;
    s_argA = s_argB  = -1;
    s_fight          = 0;
    memset(s_calmSeenMs, 0, sizeof(s_calmSeenMs));
    for (i = 0; i < SLOT_N; i++)
    {
        s_fld[i].part   = 0;
        s_fld[i].combat = 0;
        Fld_Calm(&s_fld[i]);
    }
    PcConChat_Close();
    if (s_mode == MODE_ALLEY)
        PcConMic_SetOn(0);
    Con_Enter(CS_OFF);
}

/* A field conversation that has run its course: the bubble keeps the last
 * line up for a moment, everything else is ready for the next encounter. */
static void Con_FieldDone(void)
{
    char  keep[sizeof(s_line)];
    float linger = s_bubbleLinger;
    int   spk    = s_speaker;

    SDL_strlcpy(keep, s_line, sizeof(keep));
    Con_Reset();
    SDL_strlcpy(s_line, keep, sizeof(s_line));
    s_speaker      = spk;
    s_bubbleLinger = linger;
}

static void Con_Submit(char* msgs, char* judge, int forced, int judgeKind, const char* incite)
{
    PcConTurnOpts o;

    memset(&o, 0, sizeof(o));
    o.shot        = g_PcCon.vision ? PcConHud_Capture(&o.shotW, &o.shotH) : NULL;
    o.voiceId     = s_mode == MODE_FIELD ? Fld_Voice(s_turnSpk >= 0 ? s_turnSpk : s_leader) : NULL;
    o.judgeKind   = judgeKind;
    o.incite      = incite;
    /* Rolled here: the worker thread's rand() is never seeded. */
    o.inciteFalls = incite && Con_Rand01() < g_PcCon.monsterInciteChance;
    PcConNet_SubmitTurn(s_gen, msgs, judge, forced, &o);
}

static void Con_SubmitTurnEx(const char* userContent, int forced, const char* incite)
{
    if (s_note[0])
    {
        char buf[1600];
        snprintf(buf, sizeof(buf), "%s\n%s", s_note, userContent);
        s_note[0] = '\0';
        Hist_Add(0, buf);
    }
    else
    {
        Hist_Add(0, userContent);
    }
    s_turnSpk = -1;
    Con_Submit(Con_BuildMsgs(), Con_BuildJudge(), forced, PCCON_JUDGE_TURN, incite);
}

static void Con_SubmitTurn(const char* userContent, int forced)
{
    Con_SubmitTurnEx(userContent, forced, NULL);
}

/* ------------------------------------------------------------------ */
/* Turning them on each other                                          */
/* ------------------------------------------------------------------ */

static int Arg_Mentioned(int charaId, const char* low)
{
    static const char* const groaner[] = { "groaner", "dog", "mutt", "hound", "mongrel", NULL };
    static const char* const romper[]  = { "romper", "ape", "monkey", "gorilla", NULL };
    static const char* const flyer[]   = { "screamer", "flutter", "bird", "pterodactyl", "wings", NULL };
    const char* const* w = charaId == Chara_Groaner ? groaner : charaId == Chara_Romper ? romper : flyer;
    for (; *w; w++)
        if (strstr(low, *w))
            return 1;
    return 0;
}

/* The one he named, else the one standing closest to the leader. */
static int Arg_PickRival(int a)
{
    char  low[sizeof(s_lastHarry)];
    int   i, best = -1, named = -1;
    float bd = 1e9f, nd = 1e9f;

    for (i = 0; s_lastHarry[i] && i < (int)sizeof(low) - 1; i++)
        low[i] = (char)tolower((unsigned char)s_lastHarry[i]);
    low[i] = '\0';
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        float d;
        if (i == a || !s_fld[i].part || !Fld_Alive(i))
            continue;
        d = Con_DistNpc(a, i);
        if (d < bd) { bd = d; best = i; }
        if (d < nd && Arg_Mentioned(g_SysWork.npcs[i].model.charaId, low)) { nd = d; named = i; }
    }
    return named >= 0 ? named : best;
}

static void Arg_Submit(int spk, int other, const char* instruction, int forced, int judgeKind)
{
    char* msgs;

    /* What happened since (him butting in) goes into the transcript once. */
    if (s_note[0])
    {
        Hist_Add(0, s_note);
        s_note[0] = '\0';
    }
    msgs = Con_BuildArgueMsgs(spk, other, instruction);
    s_turnSpk            = spk;
    s_fld[spk].look1     = (u8)(other + 1);
    s_fld[other].look1   = (u8)(spk + 1);
    Con_Submit(msgs, judgeKind == PCCON_JUDGE_ARGUE ? Con_BuildJudge() : NULL, forced, judgeKind, NULL);
    Con_Enter(CS_THINKING);
}

static void Arg_Start(void)
{
    int  a = s_leader;
    int  b = a >= 0 ? Arg_PickRival(a) : -1;
    char lb[48], ins[512], ev[160];

    if (b < 0)
    {
        Con_SubmitTurn("[He tried to turn you against the others, but there is nobody left here to turn on.]", -1);
        Con_Enter(CS_THINKING);
        return;
    }
    s_argue = 1;
    s_argA  = a;
    s_argB  = b;
    Con_OtherLabel(a, b, lb, sizeof(lb));
    SH_DBG("[CONSCIOUS] field: it worked -- the %s (slot %d) rounds on the %s (slot %d)", Con_SlotName(a), a, lb, b);
    snprintf(ev, sizeof(ev), "[The %s rounds on the %s!]", Con_SlotName(a), lb);
    PcConChat_Add(PCCHAT_SYSTEM, NULL, ev);
    snprintf(ins, sizeof(ins),
             "[What %s just said got to you. Turn to the %s and confront it about it. Speak TO the %s, not to %s.]",
             g_PcCon.playerName, lb, lb, g_PcCon.playerName);
    Arg_Submit(a, b, ins, PCCON_DECIDE_TALK, PCCON_JUDGE_TURN);
}

/* The last argument line has been said: the next one, or the end. */
static void Arg_Next(void)
{
    char la[48], lb[48], ins[640];

    if (!s_fld[s_argA].part || !s_fld[s_argB].part)
    {
        s_argue = 0;
        Con_Enter(CS_LISTEN);
        return;
    }
    Con_OtherLabel(s_argB, s_argA, la, sizeof(la));
    Con_OtherLabel(s_argA, s_argB, lb, sizeof(lb));
    if (s_argue == 1)
    {
        s_argue = 2;
        snprintf(ins, sizeof(ins),
                 "[The %s just turned on you because of what %s said. Answer it: deny it, insult it back, threaten "
                 "it, or admit it. Speak TO the %s.]", la, g_PcCon.playerName, la);
        Arg_Submit(s_argB, s_argA, ins, PCCON_DECIDE_TALK, PCCON_JUDGE_TURN);
    }
    else
    {
        s_argue = 3;
        snprintf(ins, sizeof(ins),
                 "[Now decide, and say it plainly: either you believe %s and you attack the %s right now, or you "
                 "realise %s is playing you both and you go for HIM, together with the %s.]",
                 g_PcCon.playerName, lb, g_PcCon.playerName, lb);
        Arg_Submit(s_argA, s_argB, ins, -1, PCCON_JUDGE_ARGUE);
    }
}

static void Fld_StartFight(int a, int b)
{
    char lb[48], ev[160];

    Con_OtherLabel(a, b, lb, sizeof(lb));
    SH_DBG("[CONSCIOUS] field: fight -- %s (slot %d) vs %s (slot %d)", Con_SlotName(a), a, lb, b);
    Fld_Calm(&s_fld[a]);
    Fld_Calm(&s_fld[b]);
    s_fld[a].foe1  = (u8)(b + 1);
    s_fld[b].foe1  = (u8)(a + 1);
    s_fld[a].coolT = 0.2f;
    s_fld[b].coolT = 0.7f;
    s_fight        = 1;
    s_fightT       = 0.0f;
    snprintf(ev, sizeof(ev), "[The %s attacks the %s!]", Con_SlotName(a), lb);
    PcConChat_Add(PCCHAT_SYSTEM, NULL, ev);
}

static float Fld_Reach(int i)
{
    switch (g_SysWork.npcs[i].model.charaId)
    {
    case Chara_Groaner: return 1.4f;
    case Chara_Romper:  return 1.5f;
    default:            return 1.9f;
    }
}

/* Through the same damage fields a hit from Harry fills, so each AI takes
 * it its own way: recoil, stagger, death. */
static void Fld_Bite(int i, int j)
{
    s_SubCharacter* a    = &g_SysWork.npcs[i];
    s_SubCharacter* b    = &g_SysWork.npcs[j];
    float           dx   = (float)(b->position.vx - a->position.vx) / 4096.0f;
    float           dz   = (float)(b->position.vz - a->position.vz) / 4096.0f;
    float           l    = sqrtf(dx * dx + dz * dz);
    q19_12          base = s_fld[j].health0 > 0 ? s_fld[j].health0 : b->health;
    q19_12          dmg  = (q19_12)((float)base * (0.2f + 0.12f * Con_Rand01()));

    if (l < 0.01f) l = 0.01f;
    if (dmg < Q12(1.0f)) dmg = Q12(1.0f);
    b->damage.amount      += dmg;
    b->damage.position.vx += (q19_12)(dx / l * 0.4f * 4096.0f);
    b->damage.position.vz += (q19_12)(dz / l * 0.4f * 4096.0f);
    b->attackReceived      = FIGHT_ATTACK;
    SH_DBG("[CONSCIOUS] fight: %s bites %s for %.1f (has %.1f)", Con_SlotName(i), Con_SlotName(j),
           dmg / 4096.0f, b->health / 4096.0f);
}

/* One of them is dead (or out of it): the winner turns back to Harry. */
static void Fld_FightOver(int winner, int loser)
{
    char ev[320], line[160];

    SH_DBG("[CONSCIOUS] field: fight over, the %s won", Con_SlotName(winner));
    Fld_Calm(&s_fld[winner]);
    s_fight  = 0;
    s_leader = winner;
    snprintf(line, sizeof(line), "[The %s is down. The %s turns back to you.]", Con_SlotName(loser),
             Con_SlotName(winner));
    PcConChat_Add(PCCHAT_SYSTEM, NULL, line);
    snprintf(ev, sizeof(ev), "[You just beat the %s in a fight, right in front of %s, because of what he said. "
                             "Gloat about it, then deal with HIM.]", Con_SlotName(loser), g_PcCon.playerName);
    if (s_state == CS_LISTEN)
    {
        Con_SubmitTurn(ev, -1);
        Con_Enter(CS_THINKING);
    }
    else
    {
        SDL_strlcat(s_note, ev, sizeof(s_note));
    }
}

static void Fld_FightTick(float dt)
{
    int i, winner = -1, loser = -1;

    s_fightT += dt;
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        FldMon* m = &s_fld[i];
        int     j;
        if (!m->foe1)
            continue;
        j = m->foe1 - 1;
        if (!m->part || !s_fld[j].part || !Fld_Alive(j) || s_fld[j].foe1 != i + 1)
        {
            winner = i;
            loser  = j;
            continue;
        }
        m->coolT -= dt;
        if (m->strikeT > 0.0f)
        {
            float prev = m->strikeT;
            m->strikeT -= dt;
            if (prev > FIGHT_LAND && m->strikeT <= FIGHT_LAND && Con_DistNpc(i, j) <= Fld_Reach(i) + 0.6f)
                Fld_Bite(i, j);
            if (m->strikeT < 0.0f)
                m->strikeT = 0.0f;
        }
        else if (m->coolT <= 0.0f && Con_DistNpc(i, j) <= Fld_Reach(i))
        {
            m->strikeT = FIGHT_STRIKE;
            m->coolT   = FIGHT_STRIKE + 0.6f + Con_Rand01();
        }
    }
    if (winner >= 0)
    {
        Fld_FightOver(winner, loser);
    }
    else if (s_fightT > FIGHT_MAX)
    {
        SH_DBG("[CONSCIOUS] field: the fight fizzles out");
        for (i = 0; i < NPC_N && i < SLOT_N; i++)
            if (s_fld[i].foe1)
                Fld_Calm(&s_fld[i]);
        s_fight = 0;
        SDL_strlcat(s_note, "[You two broke off your fight, neither of you won.] ", sizeof(s_note));
    }
}

/* A health drop that is not a bite from another monster: Harry hit it. */
static int Fld_HarryHit(int i)
{
    q19_12 h   = g_SysWork.npcs[i].health;
    int    hit = h < s_fld[i].health && g_SysWork.npcs[i].attackReceived != FIGHT_ATTACK;
    if (!hit)
        s_fld[i].health = h;
    return hit;
}

static int Con_OutOfPatience(void)
{
    if (s_mode == MODE_FIELD)
        return s_totalT >= g_PcCon.patienceSec || s_turns >= g_PcCon.monsterMaxTurns;
    return s_totalT >= g_PcCon.patienceSec || s_turns >= g_PcCon.maxTurns;
}

/* Patience is a hard limit on top of whatever the judge thinks, so a player
 * who keeps them talking forever still gets the death the game needs. */
static void Con_PlayerTurn(const char* content)
{
    char buf[1024];
    if (s_mode == MODE_FIELD && s_fldAngry)
    {
        /* They are already on him: whoever is closest shouts back, no judge. */
        int c = Fld_Combatant(NULL);
        if (c >= 0)
            s_leader = c;
        Con_SubmitTurn(content, PCCON_DECIDE_ATTACK);
    }
    else if (Con_OutOfPatience())
    {
        snprintf(buf, sizeof(buf), "%s\n[You have completely run out of patience. This is your last line: "
                                   "tell him he is done and that you are going to get him NOW.]", content);
        Con_SubmitTurn(buf, PCCON_DECIDE_ATTACK);
    }
    else if (s_mode == MODE_FIELD && !s_fight && Fld_Parts(NULL) >= 2 && s_lastHarry[0])
    {
        char scene[900];
        int  i;
        snprintf(scene, sizeof(scene), "Monsters listening:");
        for (i = 0; i < NPC_N && i < SLOT_N; i++)
            if (s_fld[i].part)
                snprintf(scene + strlen(scene), sizeof(scene) - strlen(scene), " %s,", Con_SlotName(i));
        scene[strlen(scene) - 1] = '\0';
        snprintf(scene + strlen(scene), sizeof(scene) - strlen(scene), "\nHarry's latest line: \"%s\"", s_lastHarry);
        Con_SubmitTurnEx(content, -1, scene);
    }
    else
    {
        Con_SubmitTurn(content, -1);
    }
    s_lastHarry[0] = '\0';
    Con_Enter(CS_THINKING);
}

/* Field: the conversation is over and they attack now. Said is what made
 * them, for the line they shout while coming at him. */
static void Fld_Break(const char* why, const char* event)
{
    SH_DBG("[CONSCIOUS] field: attack (%s)", why);
    Fld_Unleash();
    s_argue = 0;
    PcConChat_Close();
    if (event)
    {
        Con_SubmitTurn(event, PCCON_DECIDE_ATTACK);
        Con_Enter(CS_THINKING);
    }
    else
    {
        Con_FieldDone();
    }
}

static void Con_Hostile(const char* why)
{
    SH_DBG("[CONSCIOUS] hostile (%s)", why);
    if (s_mode == MODE_FIELD)
    {
        Fld_Unleash();
        Con_FieldDone();
        return;
    }
    PcConChat_Add(PCCHAT_SYSTEM, NULL, "[The grey children attack!]");
    PcConChat_Close();
    Con_Enter(CS_HOSTILE);
}

static void Con_Speak(const char* text, const short* pcm, unsigned int samples, int rate, int decision)
{
    float d;
    s_speaker = s_mode == MODE_FIELD ? (s_turnSpk >= 0 ? s_turnSpk : s_leader) : Con_Nearest(&d);
    PcConChat_Add(PCCHAT_NPC, s_mode == MODE_FIELD ? Con_SlotName(s_speaker) : NPC_NAME, text);
    Pc_VoiceClip_Stop(s_clip);
    s_clip     = pcm ? Pc_VoiceClip_PlayEx(pcm, samples, rate, 0, Con_Gain(s_speaker), g_PcCon.voicePitch) : 0;
    s_textHold = s_clip ? 0.3f : 1.2f + 0.055f * (float)strlen(text);
    SDL_strlcpy(s_line, text, sizeof(s_line));
    s_lineT   = 0.0f;
    s_lineDur = s_clip ? (float)samples / ((float)rate * g_PcCon.voicePitch) : s_textHold;
    s_bubbleLinger = 1.5f;
    s_pendingDecision = decision;
    s_talking = 1;
    if (!s_argue)
        s_turns++;
    SH_DBG("[CONSCIOUS] say (turn %d, decision %d, tts %u samples, playing %s, gain %.2f): %s", s_turns, decision,
           pcm ? samples : 0u, s_clip ? "yes" : "no", Con_Gain(s_speaker), text);
    /* In the field the attack starts with the line, not after it. */
    if (s_mode == MODE_FIELD && decision == PCCON_DECIDE_ATTACK && !s_fldAngry)
    {
        SH_DBG("[CONSCIOUS] field: attack (their line meant attack)");
        Fld_Unleash();
    }
    if (s_mode == MODE_FIELD && s_argue == 3 && decision == PCCON_DECIDE_FIGHT)
    {
        Fld_StartFight(s_argA, s_argB);
    }
    else if (s_mode == MODE_FIELD && s_argue == 3 && !s_fldAngry)
    {
        SH_DBG("[CONSCIOUS] field: they team up against him");
        PcConChat_Add(PCCHAT_SYSTEM, NULL, "[They turn on you together!]");
        Fld_Unleash();
    }
    Con_Enter(CS_SPEAKING);
}

static void Con_SpeakResult(PcConResult* r, int fallbackIdx, int forced)
{
    const char* fb;
    if (r && r->ok && r->text)
    {
        Con_Speak(r->text, r->pcm, r->samples, r->rate, r->decision);
        return;
    }
    fb = s_mode == MODE_FIELD ? s_fieldFallback[forced == PCCON_DECIDE_ATTACK ? 2 : fallbackIdx % 2]
                              : s_fallback[fallbackIdx];
    Hist_Add(1, fb);
    Con_Speak(fb, NULL, 0, 0, forced >= 0 ? forced : PCCON_DECIDE_TALK);
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
    if (s_state != CS_LISTEN && s_state != CS_HEARING)
    {
        /* Mid-argument he can still butt in: the next line hears it. */
        if (s_mode == MODE_FIELD && s_argue && Con_Words(text) >= (typed ? 1 : g_PcCon.minWordsFirst))
        {
            PcConChat_Add(PCCHAT_PLAYER, g_PcCon.playerName, text);
            snprintf(line, sizeof(line), "[%s butts in: \"%s\"] ", g_PcCon.playerName, text);
            SDL_strlcat(s_note, line, sizeof(s_note));
            return;
        }
        PcConChat_Add(PCCHAT_DIM, g_PcCon.playerName, text);
        return;
    }
    /* Five words before the first answer counts, so a cough, a "hm" or the
     * room talking is not taken as Harry's reply to being cornered. Monsters
     * want five every time: anything shorter is just noise to them. */
    if (s_mode == MODE_FIELD)
        need = typed ? 1 : g_PcCon.minWordsFirst;
    else
        need = typed ? 1 : (s_firstReplyDone ? g_PcCon.minWords : g_PcCon.minWordsFirst);
    if (Con_Words(text) < need)
    {
        snprintf(line, sizeof(line), "%s  (too short, ignored)", text);
        PcConChat_Add(PCCHAT_DIM, g_PcCon.playerName, line);
        return;
    }
    PcConChat_Add(PCCHAT_PLAYER, g_PcCon.playerName, text);
    SDL_strlcpy(s_lastHarry, text, sizeof(s_lastHarry));
    s_firstReplyDone = 1;
    snprintf(line, sizeof(line), "%s says: \"%s\"", g_PcCon.playerName, text);
    if (s_mode == MODE_FIELD && s_state == CS_HEARING)
        SH_DBG("[CONSCIOUS] field: conversation opens with a %s", Con_SpeakerName());
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
    {
        /* The other monster's answer is part of the scene, not the leader's own line. */
        if (s_argue == 2)
        {
            char buf[600];
            snprintf(buf, sizeof(buf), "[The %s snaps back at the %s: \"%s\"]", Con_SlotName(s_argB),
                     Con_SlotName(s_argA), r->text);
            Hist_Add(0, buf);
        }
        else
        {
            Hist_Add(1, r->text);
        }
    }

    if (s_state == CS_THINKING && r->ok && r->decision == PCCON_DECIDE_INCITE)
    {
        Arg_Start();
        PcConNet_Free(r);
        return;
    }

    if (s_state == CS_ARMED || s_state == CS_AWAKE)
    {
        PcConNet_Free(&s_opener);
        s_opener      = *r;
        s_openerReady = 1;
        return;
    }
    if (s_state == CS_THINKING)
    {
        int forced = (Con_OutOfPatience() || s_fldAngry) ? PCCON_DECIDE_ATTACK : -1;
        Con_SpeakResult(r, 1 + (s_turns & 1), forced);
    }
    PcConNet_Free(r);
}

static void Con_TickAlley(float dt)
{
    float nearD;

    if (s_state == CS_OFF)
    {
        int i;
        SH_DBG("[CONSCIOUS] grey children present: writing their opener");
        s_mode = MODE_ALLEY;
        Con_Enter(CS_ARMED);
        PcConNet_Start();
        Con_SubmitTurn("[A grown-up stranger has wandered into your alley and found the body on the fence. "
                       "You have him cornered. Announce yourselves and demand to know who he is and what "
                       "he thinks he is doing here.]",
                       PCCON_DECIDE_TALK);
        for (i = 0; i < SLOT_N; i++)
            s_calmSeenMs[i] = 0;
        if (g_PcCon.micStartOn)
            PcConMic_SetOn(1);
    }

    if (s_state != CS_HOSTILE && g_SysWork.playerWork.player.health <= 0)
    {
        Con_Hostile("Harry is down");
        return;
    }

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

    case CS_RELEASED:
        /* Back through the fence he came in by: the game goes on to the cafe
         * the same way it does when he dies here (EventFlag_25 is what the
         * death sets; map0_s00's own events run the rest). */
        if (!s_fenceDone && !Savegame_EventFlagGet(EventFlag_25))
        {
            const float fx = (float)g_SysWork.playerWork.player.position.vx / 4096.0f - FENCE_X;
            const float fz = (float)g_SysWork.playerWork.player.position.vz / 4096.0f - FENCE_Z;
            if (fx * fx + fz * fz < FENCE_REACH * FENCE_REACH)
            {
                SH_DBG("[CONSCIOUS] released and back at the fence: on to the cafe");
                s_fenceDone = 1;
                PcConChat_Add(PCCHAT_SYSTEM, NULL, "[You slip back through the fence...]");
                Savegame_EventFlagSet(EventFlag_25);
                break;
            }
        }
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

/* He hit one that had let him go: all of them that let him go turn on him,
 * and the one he hit says what it thinks of that. */
static void Fld_Betrayed(int hit)
{
    int  i;
    char ev[160];

    SH_DBG("[CONSCIOUS] field: a spared %s (slot %d) was hit", Con_SlotName(hit), hit);
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (!s_fld[i].spared)
            continue;
        if (i != hit && (!Fld_Alive(i) || Con_DistToPlayer(i) > g_PcCon.monsterFleeRange))
            continue;
        s_fld[i].spared = 0;
        if (s_state == CS_OFF)
            s_fld[i].part = 1;
        else
            s_fld[i].angered = 1;
    }
    if (s_state != CS_OFF)
        return;
    s_mode   = MODE_FIELD;
    s_leader = hit;
    PcConNet_Start();
    snprintf(ev, sizeof(ev), "[You attacked the %s that let you go!]", Con_SlotName(hit));
    PcConChat_Add(PCCHAT_SYSTEM, NULL, ev);
    Fld_Break("he hit one that let him go",
              "[You let Harry go, and he just ATTACKED you anyway! You are furious: tell him what a mistake "
              "that was, and attack him now.]");
}

static void Con_TickField(float dt)
{
    float nearD;
    int   i;

    Fld_Sync();

    /* Spared means left alone, not invulnerable. */
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (s_fld[i].spared && Fld_HarryHit(i))
        {
            Fld_Betrayed(i);
            break;
        }
    }

    if (s_state == CS_OFF)
    {
        /* His voice is what stops them: talking, being transcribed or typing
         * near one opens the hearing, and they hold still while it runs. */
        if (s_candN && (PcConMic_Speaking() || s_sttPending > 0 || PcConChat_IsOpen()))
        {
            s_mode = MODE_FIELD;
            PcConNet_Start();
            Fld_JoinAll(0);
            s_leader = -1;
            Fld_Sync();
            SH_DBG("[CONSCIOUS] field: %d monster(s) stop to listen, a %s leads", Fld_Parts(NULL),
                   Con_SpeakerName());
            Con_Enter(CS_HEARING);
        }
        return;
    }

    if (g_SysWork.playerWork.player.health <= 0)
    {
        Fld_Unleash();
        Con_Reset();
        return;
    }

    if (s_fldAngry)
    {
        /* They are on him; the shouting lasts while one of them is still close. */
        float cd;
        if ((Fld_Combatant(&cd) < 0 || cd > g_PcCon.monsterFleeRange * 1.5f) && s_state == CS_LISTEN)
        {
            SH_DBG("[CONSCIOUS] field: nobody left to shout at him");
            Con_FieldDone();
        }
        return;
    }

    /* Hitting one of them ends the talking. */
    for (i = 0; i < NPC_N && i < SLOT_N; i++)
    {
        if (!s_fld[i].part || !Fld_HarryHit(i))
            continue;
        if (s_state == CS_HEARING)
        {
            Fld_Unleash();
            Con_Reset();
        }
        else
        {
            char ev[256];
            snprintf(ev, sizeof(ev), "[%s just ATTACKED you in the middle of talking! You are furious "
                                     "and attack him now.]", g_PcCon.playerName);
            PcConChat_Add(PCCHAT_SYSTEM, NULL, "[You attacked them!]");
            s_leader = i;
            Fld_Break("he hit one", ev);
        }
        return;
    }

    if (Fld_Parts(&nearD) == 0)
    {
        Con_Reset();
        return;
    }

    if (nearD > g_PcCon.monsterFleeRange)
    {
        if (s_state == CS_HEARING)
        {
            Fld_Unleash();
            Con_Reset();
        }
        else
        {
            char ev[256];
            snprintf(ev, sizeof(ev), "[%s is RUNNING AWAY from you in the middle of the conversation! "
                                     "Call him out for it and go after him.]", g_PcCon.playerName);
            PcConChat_Add(PCCHAT_SYSTEM, NULL, "[You're running away!]");
            Fld_Break("he ran", ev);
        }
        return;
    }

    Fld_JoinAll(s_state != CS_HEARING);

    if (s_fight)
        Fld_FightTick(dt);

    switch (s_state)
    {
    case CS_HEARING:
        if (PcConMic_Speaking() || s_sttPending > 0 || PcConChat_IsOpen())
            s_stateT = 0.0f;
        else if (s_stateT > 1.0f)
        {
            /* Not angered: a cough near one must not cost the chance to talk. */
            SH_DBG("[CONSCIOUS] field: nothing worth hearing, back to it");
            Con_Reset();
        }
        break;

    default:
        break;
    }
}

static void Con_Tick(float dt, int live)
{
    if (g_GameWork.gameState != GameState_InGame)
        return;

    if (Con_InScene())
    {
        if (s_state != CS_OFF && s_mode == MODE_FIELD)
            Con_Reset();
        s_mode = MODE_ALLEY;
    }
    else if (s_mode == MODE_ALLEY)
    {
        if (s_state != CS_OFF)
            Con_Reset();
        s_fenceDone = 0;
        s_mode      = MODE_FIELD;
    }

    if (s_clip)
        Pc_VoiceClip_SetGain(s_clip, Con_Gain(s_speaker));

    if (!live)
        return;
    s_stateT += dt;
    if (s_talking && s_state != CS_HOSTILE)
        s_totalT += dt;

    if (s_mode == MODE_ALLEY)
    {
        if (!Con_InScene())
            return;
        Con_TickAlley(dt);
    }
    else
    {
        if (!g_PcCon.monsters)
            return;
        Con_TickField(dt);
    }
    if (s_state == CS_OFF)
        return;

    switch (s_state)
    {
    case CS_SPEAKING:
        if (s_textHold > 0.0f)
            s_textHold -= dt;
        if (s_textHold <= 0.0f && !Pc_VoiceClip_Playing(s_clip))
        {
            s_clip = 0;
            if (s_mode == MODE_FIELD && s_argue)
            {
                if (s_argue == 3)
                {
                    s_argue = 0;
                    Con_Enter(CS_LISTEN);
                }
                else
                {
                    Arg_Next();
                }
            }
            else if (s_mode == MODE_FIELD && (s_pendingDecision == PCCON_DECIDE_ATTACK || s_fldAngry))
            {
                /* Coming at him, but still listening: he can keep talking. */
                if (!s_fldAngry)
                    Fld_Unleash();
                if (Fld_Combatant(NULL) >= 0)
                    Con_Enter(CS_LISTEN);
                else
                    Con_FieldDone();
            }
            else if (s_pendingDecision == PCCON_DECIDE_ATTACK)
            {
                Con_Hostile("their line meant attack");
            }
            else if (s_pendingDecision == PCCON_DECIDE_RELEASE && s_mode == MODE_FIELD)
            {
                SH_DBG("[CONSCIOUS] field: they let him go");
                PcConChat_Add(PCCHAT_SYSTEM, NULL, "[They're letting you go.]");
                Fld_Spare();
                Con_FieldDone();
            }
            else if (s_pendingDecision == PCCON_DECIDE_RELEASE)
            {
                int i;
                s_releases++;
                /* Back off a step and a half from Harry and stay put, so the
                 * way past them is open while the grace period runs. */
                for (i = 0; i < NPC_N && i < SLOT_N; i++)
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
        if (s_mode == MODE_FIELD && (s_fldAngry || s_fight))
        {
            /* Busy fighting: his silence is not a turn. */
            if (s_fldAngry && s_stateT >= COMBAT_QUIET)
            {
                SH_DBG("[CONSCIOUS] field: no more shouting, they just fight");
                Con_FieldDone();
            }
        }
        else if (s_stateT >= g_PcCon.listenSec)
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
            Con_SpeakResult(NULL, 1, s_fldAngry ? PCCON_DECIDE_ATTACK : -1);
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
    const int scene = s_iconUp && g_SysWork.sysState == SysState_Gameplay;
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
    if (scene && !g_PcConsoleInputActive && !g_PcQuickOptionsActive)
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
    if (!g_PcCon.enabled || (!s_iconUp && !PcConChat_IsOpen() && !s_swallow))
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
    if (s_scMic == SDL_SCANCODE_UNKNOWN)
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
/* AI hooks                                                            */
/* ------------------------------------------------------------------ */

/* Asked by Ai_Stalker_Control_2 (map0_s00) every frame. 1 = stay calm and walk
 * to (goalX, goalZ), stopping stopDist short; 0 = run the retail AI. */
int Pc_Conscious_StalkerCalm(s_SubCharacter* chara, q19_12* goalX, q19_12* goalZ, q19_12* stopDist)
{
    int idx = (int)(chara - &g_SysWork.npcs[0]);
    int rank = 0, i;

    if (!g_PcCon.enabled || idx < 0 || idx >= NPC_N || idx >= SLOT_N)
        return 0;
    if (s_mode != MODE_ALLEY || s_state == CS_OFF || s_state == CS_HOSTILE ||
        g_SavegamePtr->mapIdx != MapIdx_MAP0_S00)
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

/* Asked by the Groaner, Romper and Air Screamer AI every frame. The AI file
 * decides itself whether it can obey right now (not mid-pounce). */
int Pc_Conscious_MonsterOrder(s_SubCharacter* chara, s_SubCharacter** foe, s32* striking)
{
    int     idx = (int)(chara - &g_SysWork.npcs[0]);
    FldMon* m;

    *foe      = NULL;
    *striking = 0;
    if (!g_PcCon.enabled || !g_PcCon.monsters || idx < 0 || idx >= NPC_N || idx >= SLOT_N)
        return PcConOrder_Free;
    m = &s_fld[idx];
    if (m->chara != chara->model.charaId || m->angered || chara->health <= 0)
        return PcConOrder_Free;
    if (m->foe1 && Fld_Alive(m->foe1 - 1) && s_fld[m->foe1 - 1].part)
    {
        *foe      = &g_SysWork.npcs[m->foe1 - 1];
        *striking = m->strikeT > 0.0f;
        return PcConOrder_Fight;
    }
    if (m->part || m->spared)
    {
        if (m->look1 && Fld_Alive(m->look1 - 1))
            *foe = &g_SysWork.npcs[m->look1 - 1];
        return PcConOrder_Hold;
    }
    return PcConOrder_Free;
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

void Pc_Conscious_Frame(void)
{
    const unsigned int now = SDL_GetTicks();
    float          dt;
    int            live, inGame;
    unsigned char* wav;
    unsigned long  wavLen;
    PcConResult    r;
    PcConHudState  hud;
    char           name[48];

    if (!s_init)
    {
        s_init = 1;
        PcCon_CfgLoad();
        Voices_Parse();
        srand(now);
        s_scMic  = SDL_GetScancodeFromName(g_PcCon.keyMic);
        s_scChat = SDL_GetScancodeFromName(g_PcCon.keyChat);
        s_lastMs = now;
    }
    if (!g_PcCon.enabled)
        return;

    dt = (float)(now - s_lastMs) / 1000.0f;
    s_lastMs = now;
    if (dt > 0.1f) dt = 0.1f;

    inGame  = g_GameWork.gameState == GameState_InGame;
    s_candN = inGame ? Fld_Candidates() : 0;
    s_iconUp = inGame && s_state != CS_HOSTILE &&
               (s_state != CS_OFF || s_candN > 0 || (PcConMic_On() && g_PcCon.monsters && !Con_InScene()));

    Pc_VoiceClip_Tick();
    Con_Input();

    /* Through speakers the mic hears them talking; none of that is Harry. */
    if (s_state == CS_SPEAKING)
        PcConMic_Discard();
    if (PcConMic_Pump(&wav, &wavLen))
    {
        if ((s_state != CS_OFF || s_candN > 0) && g_PcCon.stt && g_PcCon.elevenKey[0])
        {
            s_sttPending++;
            PcConNet_Start();
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
    if (s_line[0] && inGame)
    {
        s_lineT += dt;
        if (s_state != CS_SPEAKING)
            s_bubbleLinger -= dt;
        if (s_state == CS_SPEAKING || s_bubbleLinger > 0.0f)
        {
            if (Con_HeadOnScreen(s_speaker, &hud.bubbleX, &hud.bubbleY))
            {
                hud.bubble     = 1;
                hud.bubbleText = s_line;
                hud.bubbleProgress = s_clip ? Pc_VoiceClip_Progress(s_clip)
                                            : (s_lineDur > 0.0f ? s_lineT / s_lineDur : 1.0f);
                if (hud.bubbleProgress > 1.0f || s_state != CS_SPEAKING)
                    hud.bubbleProgress = 1.0f;
                hud.bubbleAlpha = (s_state == CS_SPEAKING || s_bubbleLinger > 0.5f) ? 1.0f : s_bubbleLinger / 0.5f;
            }
        }
        else
        {
            s_line[0] = '\0';
        }
    }
    if (s_iconUp)
    {
        hud.micIcon     = 1;
        hud.micOn       = PcConMic_On();
        hud.micSpeaking = PcConMic_Speaking();
        hud.micFailed   = PcConMic_Failed();
        hud.micLevel    = PcConMic_Level();
        hud.micKeyName  = g_PcCon.keyMic;
        hud.chatKeyName = g_PcCon.keyChat;
        SDL_strlcpy(name, Con_SpeakerName(), sizeof(name));
        if (s_mode == MODE_FIELD && s_argue && s_argA >= 0 && s_argB >= 0)
        {
            snprintf(hud.status, sizeof(hud.status), "The %s and the %s are arguing...", Con_SlotName(s_argA),
                     Con_SlotName(s_argB));
        }
        else if (s_mode == MODE_FIELD && s_fight)
        {
            snprintf(hud.status, sizeof(hud.status), "They're fighting each other!");
        }
        else if (s_mode == MODE_FIELD && s_fldAngry)
        {
            snprintf(hud.status, sizeof(hud.status), "They're attacking! You can still talk.");
        }
        else if (s_mode == MODE_FIELD)
        {
            if (s_state == CS_HEARING)
                snprintf(hud.status, sizeof(hud.status), "The %s stops to listen...", name);
            else if (s_state == CS_LISTEN)
            {
                int left = (int)ceilf(g_PcCon.listenSec - s_stateT);
                snprintf(hud.status, sizeof(hud.status), s_sttPending ? "The %s is listening..." :
                         "The %s wants an answer... %d", name, left < 0 ? 0 : left);
            }
            else if (s_state == CS_THINKING && !s_fldAngry)
                snprintf(hud.status, sizeof(hud.status), "The %s is thinking it over...", name);
        }
        else if (s_state == CS_LISTEN)
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
    if (inGame)
        PcConHud_Draw(&hud);
}
