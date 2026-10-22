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
    float listenSec, graceSec, patienceSec;
    int   minWordsFirst, minWords, maxTurns;
    float hearRange;           /* metres: full volume within 3, silent beyond */
    char  keyMic[32], keyChat[32];
    int   micStartOn;
    float vadThreshold;        /* RMS 0..1 that counts as speech */
    float vadHangSec;          /* silence that ends an utterance */
    char  playerName[32];
} PcConCfg;

extern PcConCfg g_PcCon;

void PcCon_CfgLoad(void);

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
    PCCON_DECIDE_ATTACK
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

#define PCCON_JOB_STT  1
#define PCCON_JOB_TURN 2

void PcConNet_Start(void);
/* Both take ownership of the malloc'd buffer they are given. */
void PcConNet_SubmitStt(unsigned int gen, unsigned char* wav, unsigned long wavLen);
/* replyMsgs: a JSON array of chat messages. judgeText: transcript for the
 * judge. forced >= 0 skips the judge and uses that decision. */
void PcConNet_SubmitTurn(unsigned int gen, char* replyMsgs, char* judgeText, int forced);
/* Returns 1 and fills *out when a result is waiting; free with PcConNet_Free. */
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

#ifdef __cplusplus
}
#endif

#endif
