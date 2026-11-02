/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious.h - talking grey children (silenthill-conscious branch).
 *
 * Internal to src/conscious/. Only pc_conscious.c reads game state; the
 * worker (pc_conscious_net.c) and the HTTP TU never touch it, and the HTTP TU
 * is the only one allowed to see <windows.h>.
 */
#ifndef PC_CONSCIOUS_H
#define PC_CONSCIOUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- config (pc_conscious_cfg.c) ---------------------------------- */

typedef struct
{
    int   enabled;
    char  llmUrl[256], llmModel[96], llmKey[192];
    char  judgeUrl[256], judgeModel[96], judgeKey[192];
    float llmTemperature;
    int   llmMaxTokens;
    int   tts;                 /* 0 none (text only), 1 ElevenLabs */
    int   stt;                 /* 0 none (typed chat only), 1 ElevenLabs */
    char  elevenKey[192];
    char  voiceId[64], ttsModel[64];
    float voicePitch, voiceStability, voiceSimilarity, voiceStyle;
    char  morpheusDir[260];
    float listenSec, patienceSec;
    int   minWordsFirst, minWords, maxTurns;
    int   monsters;            /* other enemies can be talked to too */
    float monsterHearRange;    /* metres: they stop when Harry speaks this close */
    float monsterFleeRange;    /* metres: past this mid-conversation he is running away */
    int   monsterMaxTurns;
    char  monsterVoices[640];  /* comma-separated ElevenLabs voice ids */
    float monsterInciteChance; /* 0..1: that turning them on each other works at all */
    float darkHarryCallChance; /* 0..1: per monster turn, that it calls Dark Harry for help */
    int   vision;              /* send what the camera sees with each of Harry's lines */
    int   visionWidth;         /* pixels; the shot is scaled down to this */
    int   visionSave;          /* also write the last shot to conscious_view.jpg */
    float hearRange;           /* metres: full volume within 3, silent beyond */
    char  keyMic[32], keyChat[32];
    int   micStartOn;
    float vadThreshold;        /* RMS 0..1 that counts as speech */
    float vadHangSec;          /* silence that ends an utterance */
    char  playerName[32];
    int   chatLog;             /* the chat feed on screen; off = speech bubbles only */
    int   bubbles;             /* speech bubbles over their heads */
    int   prompts;             /* the centred "wants an answer..." line */
    float voiceVolume;         /* 0..1.5 on top of the distance falloff */
    char  keyMenu[32];         /* opens the conscious menu in game */
} PcConCfg;

extern PcConCfg g_PcCon;

/* One per kind that talks: who it is, how it sounds, what mood it is in. */
enum
{
    PCCON_PROF_GANG = 0,
    PCCON_PROF_GROANER,
    PCCON_PROF_ROMPER,
    PCCON_PROF_SCREAMER,
    PCCON_PROF_FLUTTER,
    PCCON_PROF_DARK,           /* Dark Harry: talk = he can appear at all */
    PCCON_PROF_N
};

typedef struct
{
    int   talk;                /* this kind stops and talks at all */
    char  voice[64];           /* ElevenLabs voice id; "" = voice_id (gang) or one from monster_voices */
    int   mood;                /* index into the mood table, 0 = as written */
    float pitch;               /* playback rate of its voice */
    float stability, style, speed;
    char  extra[240];          /* added to who it is */
} PcConProfile;

extern PcConProfile g_PcConProf[PCCON_PROF_N];

void        PcCon_CfgLoad(void);
/* Writes back what the menu changes. Every other line of conscious.cfg is
 * kept as it is, so the API key lines stay whatever the user left there. */
void        PcCon_CfgSave(void);
const char* PcCon_ProfName(int prof);     /* "Grey Children" */
#define PCCON_MOOD_N 10
const char* PcCon_MoodName(int mood);
const char* PcCon_MoodText(int mood);     /* NULL for "as written" */
/* conscious_<kind>_personality.txt beside the exe. */
void        PcCon_PersonaPath(int prof, char* out, int cap);
/* Fills out with the custom personality and returns 1, or 0 when there is none. */
int         PcCon_PersonaLoad(int prof, char* out, int cap);
/* The personality as written, for seeding that file (pc_conscious.c). */
const char* PcCon_DefaultPersona(int prof);

/* ---- HTTP (pc_conscious_http.c) ----------------------------------- */

/* Blocking. headers: CRLF-separated "Name: value" lines, may be NULL.
 * Returns the HTTP status (0 = transport failure). *outBody is malloc'd and
 * NUL-terminated (the caller frees), even for a binary body. */
int PcCon_Http(const char* method, const char* url, const char* headers,
               const void* body, unsigned long bodyLen,
               char** outBody, unsigned long* outLen);

/* ---- worker (pc_conscious_net.c) ---------------------------------- */

typedef enum
{
    PCCON_DECIDE_TALK = 0,
    PCCON_DECIDE_RELEASE,
    PCCON_DECIDE_ATTACK,
    PCCON_DECIDE_INCITE,   /* no line: he turned them on each other, the brain stages it */
    PCCON_DECIDE_FIGHT,    /* end of an argument: they fight each other */
    PCCON_DECIDE_UNITE     /* end of an argument: they go for him together */
} e_PcConDecision;

typedef struct
{
    int          kind;          /* PCCON_JOB_* */
    unsigned int gen;
    int          ok;
    char*        text;          /* transcript, or the line they say */
    int          decision;      /* e_PcConDecision, turns only */
    short*       pcm;           /* mono s16, NULL when TTS is off or failed */
    unsigned int samples;
    int          rate;
} PcConResult;

#define PCCON_JOB_STT    1
#define PCCON_JOB_TURN   2
#define PCCON_JOB_VOICES 3   /* text: one "id<TAB>name" line per voice on the account */
#define PCCON_JOB_SAY    4   /* a line read out, no model: a voice preview */

typedef struct
{
    float stability, style, speed;
} PcConVoiceTune;

void PcConNet_Start(void);
/* Both take ownership of the malloc'd buffer they are given. */
void PcConNet_SubmitStt(unsigned int gen, unsigned char* wav, unsigned long wavLen);
#define PCCON_JUDGE_TURN  0  /* ATTACK / RELEASE / TALK */
#define PCCON_JUDGE_ARGUE 1  /* FIGHT / UNITE */
#define PCCON_JUDGE_SPARED 2 /* ATTACK / TALK: they let him go, only a blatant insult undoes it */
#define PCCON_JUDGE_DARK   3 /* RELEASE / TALK: Dark Harry stops for good, or keeps coming */

typedef struct
{
    const char*    voiceId;     /* NULL or "" = the configured voice */
    unsigned char* shot;        /* bottom-up RGBA of what the camera sees, NULL = text only; owned */
    int            shotW, shotH;
    int            judgeKind;   /* PCCON_JUDGE_* */
    /* Set: first ask whether Harry's line tries to turn them on each other
     * ("who is listening + what he said"). If it does and inciteFalls, the
     * turn returns PCCON_DECIDE_INCITE with no line; if it does not land,
     * they are told to see through it. */
    const char*    incite;
    int            inciteFalls;
    PcConVoiceTune tune;        /* all zero = the cfg's voice_* settings */
} PcConTurnOpts;

/* replyMsgs: a JSON array of chat messages. judgeText: transcript for the
 * judge. forced >= 0 skips the judge and uses that decision. opts may be NULL. */
void PcConNet_SubmitTurn(unsigned int gen, char* replyMsgs, char* judgeText, int forced, const PcConTurnOpts* opts);
/* Returns 1 and fills *out when a result is waiting; free with PcConNet_Free. */
void PcConNet_SubmitVoices(unsigned int gen);
void PcConNet_SubmitSay(unsigned int gen, const char* text, const char* voiceId, const PcConVoiceTune* tune);
int  PcConNet_Poll(PcConResult* out);
void PcConNet_Free(PcConResult* r);
int  PcConNet_Busy(void);

/* JSON string body (no quotes) of src, appended to dst. */
void PcCon_JsonEscapeCat(char* dst, int cap, const char* src);

/* ---- mic (pc_conscious_mic.c) ------------------------------------- */

void  PcConMic_SetOn(int on);
int   PcConMic_On(void);
int   PcConMic_Speaking(void);    /* inside an utterance right now */
float PcConMic_Level(void);       /* smoothed 0..1 for the HUD */
/* Once a frame. Returns 1 and a malloc'd WAV when an utterance just ended. */
int   PcConMic_Pump(unsigned char** wav, unsigned long* wavLen);
int   PcConMic_Failed(void);
/* Drop everything heard so far, including an utterance in progress. */
void  PcConMic_Discard(void);

/* ---- chat (pc_conscious_chat.c) ----------------------------------- */

typedef enum
{
    PCCHAT_SYSTEM = 0,
    PCCHAT_PLAYER,
    PCCHAT_NPC,
    PCCHAT_DIM
} e_PcChatKind;

void        PcConChat_Add(int kind, const char* speaker, const char* text);
void        PcConChat_Open(void);
void        PcConChat_Close(void);
int         PcConChat_IsOpen(void);
/* While open: owns the keyboard. Returns 1 and copies the line on Enter. */
int         PcConChat_FeedKeys(const unsigned char* ks, char* submitted, int cap);
int         PcConChat_LineCount(void);
const char* PcConChat_LineText(int i);
int         PcConChat_LineKind(int i);
float       PcConChat_LineAlpha(int i);
const char* PcConChat_ComposeText(void);
int         PcConChat_DisplayActive(void);
void        PcConChat_Clear(void);
/* SDL scancode -> the character it types, or 0. */
char        PcConChat_Glyph(int sc, int shift);

/* ---- Dark Harry (pc_conscious.c) ---------------------------------- */

/* The console's AFKEGG / AFKEGGSTOP. Spawn returns NULL or why not. */
const char* Pc_Conscious_DarkSpawn(void);
const char* Pc_Conscious_DarkStop(void);
/* The voice he speaks in when his profile names none: the account's voice
 * called Harry if there is one, else a stock one. */
const char* Pc_Conscious_DarkVoice(void);

/* ---- menu (pc_conscious_menu.c) ----------------------------------- */

void PcConMenu_Voices(const char* list);   /* a PCCON_JOB_VOICES result */
void PcConMenu_Said(PcConResult* r);       /* a PCCON_JOB_SAY result: plays it, frees it */

/* ---- HUD (pc_conscious_hud.c) ------------------------------------- */

typedef struct
{
    int         micIcon;        /* draw the mic indicator */
    int         micOn, micSpeaking, micFailed;
    float       micLevel;
    const char* micKeyName;
    const char* chatKeyName;
    char        status[128];    /* centred prompt, "" for none */
    int         bubble;         /* speech over the speaker's head */
    float       bubbleX, bubbleY;   /* head, as fractions of the picture */
    float       bubbleProgress;     /* 0..1 through the line: drives the scroll */
    float       bubbleAlpha;
    const char* bubbleText;
} PcConHudState;

void PcConHud_Draw(const PcConHudState* st);
/* What is on screen right now, before this frame's HUD: malloc'd bottom-up
 * RGBA, or NULL. Call from inside the overlay hook. */
unsigned char* PcConHud_Capture(int* outW, int* outH);

#ifdef __cplusplus
}
#endif

#endif
