/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_net.c - the one worker thread that talks to the services.
 *
 * The game thread queues jobs and polls results once a frame; nothing here
 * blocks it. A turn is three requests in a row -- what they say, whether that
 * line means they attack, and the line as speech -- so the chat line and the
 * voice arrive together instead of the text racing ahead of the audio.
 *
 * The services are the ones Morpheus already uses: an OpenAI-compatible chat
 * endpoint (DeepSeek by default, a local model works the same), ElevenLabs
 * text-to-speech, and ElevenLabs Scribe for speech-to-text. TTS is asked for
 * raw pcm_22050 so the answer goes straight into an OpenAL buffer.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "pc_conscious.h"
#include "sh_log.h"

#define NET_QUEUE 6
#define TTS_RATE  22050

typedef struct
{
    int            kind;
    unsigned int   gen;
    unsigned char* wav;
    unsigned long  wavLen;
    char*          replyMsgs;
    char*          judgeText;
    int            forced;
    char           voiceId[64];
} NetJob;

static SDL_Thread* s_thread;
static SDL_mutex*  s_lock;
static SDL_cond*   s_cond;
static NetJob      s_jobs[NET_QUEUE];
static int         s_jobCount;
static PcConResult s_results[NET_QUEUE];
static int         s_resultCount;
static int         s_inFlight;

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

void PcCon_JsonEscapeCat(char* dst, int cap, const char* src)
{
    int n = (int)strlen(dst);
    for (; src && *src && n < cap - 8; src++)
    {
        unsigned char c = (unsigned char)*src;
        switch (c)
        {
        case '"':  dst[n++] = '\\'; dst[n++] = '"';  break;
        case '\\': dst[n++] = '\\'; dst[n++] = '\\'; break;
        case '\n': dst[n++] = '\\'; dst[n++] = 'n';  break;
        case '\r': break;
        case '\t': dst[n++] = ' ';                   break;
        default:
            if (c < 0x20) break;
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
}

static void Utf8_Put(char** o, char* end, unsigned cp)
{
    char* p = *o;
    if (cp < 0x80 && p < end)                 { *p++ = (char)cp; }
    else if (cp < 0x800 && p + 1 < end)       { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    else if (p + 2 < end)                     { *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
    *o = p;
}

/* The string value of the first `"key":` at or after `from`, decoded. */
static char* Json_FindString(const char* json, const char* from, const char* key)
{
    char        pat[64];
    const char* p;
    char*       out;
    char*       o;
    char*       end;
    size_t      cap;

    if (!json) return NULL;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(from ? from : json, pat);
    while (p)
    {
        const char* q = p + strlen(pat);
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q == ':')
        {
            q++;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q != '"')
                return NULL; /* null / number: absent for our purposes */
            q++;
            cap = strlen(q) + 1;
            out = (char*)malloc(cap);
            if (!out) return NULL;
            o   = out;
            end = out + cap - 1;
            while (*q && *q != '"' && o < end)
            {
                if (*q == '\\' && q[1])
                {
                    q++;
                    switch (*q)
                    {
                    case 'n': *o++ = '\n'; break;
                    case 't': *o++ = ' ';  break;
                    case 'r': break;
                    case 'u':
                    {
                        unsigned cp = 0;
                        int      i;
                        for (i = 1; i <= 4 && isxdigit((unsigned char)q[i]); i++)
                            cp = cp * 16 + (unsigned)(isdigit((unsigned char)q[i]) ? q[i] - '0' : (tolower((unsigned char)q[i]) - 'a' + 10));
                        q += i - 1;
                        Utf8_Put(&o, end, cp);
                        break;
                    }
                    default: *o++ = *q; break;
                    }
                    q++;
                }
                else
                {
                    *o++ = *q++;
                }
            }
            *o = '\0';
            return out;
        }
        p = strstr(p + 1, pat);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Text cleanup                                                        */
/* ------------------------------------------------------------------ */

/* Models narrate (*hisses*), quote themselves, think out loud (<think>) and
 * use typographic punctuation the overlay font path cannot draw. A spoken line
 * wants none of that. */
static void Text_Clean(char* s)
{
    char* r = s;
    char* w = s;
    char* think;
    int   inStar = 0;

    while ((think = strstr(s, "<think>")) != NULL)
    {
        char* close = strstr(think, "</think>");
        if (!close) { *think = '\0'; break; }
        memmove(think, close + 8, strlen(close + 8) + 1);
    }

    while (*r)
    {
        unsigned char c = (unsigned char)*r;
        if (c == 0xE2 && (unsigned char)r[1] == 0x80)
        {
            unsigned char t = (unsigned char)r[2];
            char rep = (t == 0x98 || t == 0x99) ? '\'' : (t == 0x9C || t == 0x9D) ? '"' :
                       (t == 0x93 || t == 0x94) ? '-' : (t == 0xA6) ? '.' : ' ';
            if (!inStar) *w++ = rep;
            if (t == 0xA6 && !inStar) { *w++ = '.'; *w++ = '.'; }
            r += 3;
            continue;
        }
        if (c == '*') { inStar = !inStar; r++; continue; }
        if (c >= 0x80) { r++; continue; }
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (!inStar) *w++ = (char)c;
        r++;
    }
    *w = '\0';

    {
        char* a = s;
        size_t n;
        while (*a == ' ' || *a == '"') a++;
        memmove(s, a, strlen(a) + 1);
        n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"')) s[--n] = '\0';
    }
    /* Collapse runs of spaces left behind by stripped asides. */
    for (r = w = s; *r; r++)
    {
        if (*r == ' ' && w > s && w[-1] == ' ') continue;
        *w++ = *r;
    }
    *w = '\0';
    if (strlen(s) > 320)
        s[320] = '\0';
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

static char* Net_Chat(const char* url, const char* model, const char* key,
                      const char* messagesJson, int maxTokens, float temperature)
{
    char   hdr[512];
    char*  body;
    char*  resp = NULL;
    char*  content = NULL;
    size_t cap;
    unsigned long respLen = 0;
    int    status;

    cap  = strlen(messagesJson) + 512;
    body = (char*)malloc(cap);
    if (!body) return NULL;
    snprintf(body, cap,
             "{\"model\":\"%s\",\"stream\":false,\"max_tokens\":%d,\"temperature\":%.2f,\"messages\":%s}",
             model, maxTokens, temperature, messagesJson);
    if (key && key[0])
        snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", key);
    else
        snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\n");

    status = PcCon_Http("POST", url, hdr, body, (unsigned long)strlen(body), &resp, &respLen);
    free(body);
    if (status == 200 && resp)
    {
        const char* choices = strstr(resp, "\"choices\"");
        content = Json_FindString(resp, choices, "content");
    }
    if (!content)
        SH_DBG("[CONSCIOUS] chat request to %s failed: HTTP %d %.200s", url, status, resp ? resp : "");
    free(resp);
    return content;
}

static int Net_Judge(const char* judgeText)
{
    static const char sys[] =
        "You read a scene from a horror game and decide what a pack of monsters does next. "
        "They have been talking with a man named Harry instead of attacking him. "
        "Look ONLY at whether their LATEST line means they are now attacking him, letting him go, "
        "or still talking / waiting for an answer. "
        "Answer with exactly one word: ATTACK, RELEASE, or TALK.\n"
        "ATTACK = they say they will kill/get/eat/hurt him now, they've lost patience, they order the attack.\n"
        "RELEASE = they tell him to leave, get lost, they'll spare him, back off, warn him not to come back.\n"
        "TALK = anything else: questions, threats about what MIGHT happen, demands for an answer.";
    char   msgs[8192];
    char*  ans;
    int    d = PCCON_DECIDE_TALK;
    char*  p;

    msgs[0] = '\0';
    strcat(msgs, "[{\"role\":\"system\",\"content\":\"");
    PcCon_JsonEscapeCat(msgs, (int)sizeof(msgs) - 64, sys);
    strcat(msgs, "\"},{\"role\":\"user\",\"content\":\"");
    PcCon_JsonEscapeCat(msgs, (int)sizeof(msgs) - 8, judgeText);
    strcat(msgs, "\"}]");

    ans = Net_Chat(g_PcCon.judgeUrl, g_PcCon.judgeModel, g_PcCon.judgeKey, msgs, 400, 0.0f);
    if (!ans)
        return d;
    Text_Clean(ans);
    for (p = ans; *p; p++) *p = (char)toupper((unsigned char)*p);
    /* Last mention wins: a reasoning model may list all three before deciding. */
    {
        const char* a = strstr(ans, "ATTACK");
        const char* r = strstr(ans, "RELEASE");
        const char* t = strstr(ans, "TALK");
        const char* best = NULL;
        const char* q;
        for (q = a; q; q = strstr(q + 1, "ATTACK"))  if (!best || q > best) { best = q; d = PCCON_DECIDE_ATTACK; }
        for (q = r; q; q = strstr(q + 1, "RELEASE")) if (!best || q > best) { best = q; d = PCCON_DECIDE_RELEASE; }
        for (q = t; q; q = strstr(q + 1, "TALK"))    if (!best || q > best) { best = q; d = PCCON_DECIDE_TALK; }
    }
    SH_DBG("[CONSCIOUS] judge said '%.60s' -> %d", ans, d);
    free(ans);
    return d;
}

static short* Net_Tts(const char* text, const char* voiceId, unsigned int* outSamples)
{
    char   url[256], hdr[320];
    char   body[2048];
    char*  resp = NULL;
    unsigned long respLen = 0;
    int    status;

    *outSamples = 0;
    if (!g_PcCon.tts || !g_PcCon.elevenKey[0] || !text[0])
        return NULL;
    snprintf(url, sizeof(url), "https://api.elevenlabs.io/v1/text-to-speech/%s?output_format=pcm_%d",
             voiceId[0] ? voiceId : g_PcCon.voiceId, TTS_RATE);
    snprintf(hdr, sizeof(hdr), "Content-Type: application/json\r\nAccept: audio/pcm\r\nxi-api-key: %s\r\n",
             g_PcCon.elevenKey);
    body[0] = '\0';
    strcat(body, "{\"text\":\"");
    PcCon_JsonEscapeCat(body, (int)sizeof(body) - 256, text);
    snprintf(body + strlen(body), sizeof(body) - strlen(body),
             "\",\"model_id\":\"%s\",\"voice_settings\":{\"stability\":%.2f,\"similarity_boost\":%.2f,"
             "\"style\":%.2f,\"use_speaker_boost\":true}}",
             g_PcCon.ttsModel, g_PcCon.voiceStability, g_PcCon.voiceSimilarity, g_PcCon.voiceStyle);

    status = PcCon_Http("POST", url, hdr, body, (unsigned long)strlen(body), &resp, &respLen);
    if (status != 200 || respLen < 2)
    {
        SH_DBG("[CONSCIOUS] TTS failed: HTTP %d %.200s", status, resp ? resp : "");
        free(resp);
        return NULL;
    }
    *outSamples = (unsigned int)(respLen / 2);
    return (short*)resp;
}

static char* Net_Stt(const unsigned char* wav, unsigned long wavLen)
{
    static const char B[] = "----ShConsciousBoundary7f3a";
    char   hdr[320];
    char   head[640];
    char   tail[64];
    char*  body;
    char*  resp = NULL;
    char*  text = NULL;
    unsigned long respLen = 0, bodyLen;
    size_t hl, tl;
    int    status;

    if (!g_PcCon.stt || !g_PcCon.elevenKey[0])
        return NULL;
    snprintf(hdr, sizeof(hdr), "Content-Type: multipart/form-data; boundary=%s\r\nxi-api-key: %s\r\n",
             B, g_PcCon.elevenKey);
    snprintf(head, sizeof(head),
             "--%s\r\nContent-Disposition: form-data; name=\"model_id\"\r\n\r\nscribe_v1\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"tag_audio_events\"\r\n\r\nfalse\r\n"
             "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
             "Content-Type: audio/wav\r\n\r\n",
             B, B, B);
    snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", B);
    hl = strlen(head);
    tl = strlen(tail);
    bodyLen = (unsigned long)(hl + wavLen + tl);
    body = (char*)malloc(bodyLen);
    if (!body)
        return NULL;
    memcpy(body, head, hl);
    memcpy(body + hl, wav, wavLen);
    memcpy(body + hl + wavLen, tail, tl);

    status = PcCon_Http("POST", "https://api.elevenlabs.io/v1/speech-to-text", hdr, body, bodyLen, &resp, &respLen);
    free(body);
    if (status == 200 && resp)
        text = Json_FindString(resp, NULL, "text");
    if (!text)
        SH_DBG("[CONSCIOUS] STT failed: HTTP %d %.200s", status, resp ? resp : "");
    free(resp);
    if (text)
        Text_Clean(text);
    return text;
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

static void Net_Run(const NetJob* j, PcConResult* r)
{
    memset(r, 0, sizeof(*r));
    r->kind = j->kind;
    r->gen  = j->gen;
    r->rate = TTS_RATE;

    if (j->kind == PCCON_JOB_STT)
    {
        r->text = Net_Stt(j->wav, j->wavLen);
        r->ok   = (r->text != NULL);
        return;
    }

    r->text = Net_Chat(g_PcCon.llmUrl, g_PcCon.llmModel, g_PcCon.llmKey, j->replyMsgs,
                       g_PcCon.llmMaxTokens, g_PcCon.llmTemperature);
    if (!r->text)
        return;
    Text_Clean(r->text);
    if (!r->text[0])
        return;
    r->ok = 1;

    if (j->forced >= 0)
    {
        r->decision = j->forced;
    }
    else
    {
        size_t n = strlen(j->judgeText) + strlen(r->text) + 64;
        char*  t = (char*)malloc(n);
        if (t)
        {
            snprintf(t, n, "%s\nTHEIR LATEST LINE: \"%s\"", j->judgeText, r->text);
            r->decision = Net_Judge(t);
            free(t);
        }
    }
    r->pcm = Net_Tts(r->text, j->voiceId, &r->samples);
}

static int SDLCALL Net_Thread(void* unused)
{
    (void)unused;
    for (;;)
    {
        NetJob      job;
        PcConResult res;

        SDL_LockMutex(s_lock);
        while (s_jobCount == 0)
            SDL_CondWait(s_cond, s_lock);
        job = s_jobs[0];
        memmove(&s_jobs[0], &s_jobs[1], sizeof(s_jobs[0]) * (size_t)(s_jobCount - 1));
        s_jobCount--;
        s_inFlight = 1;
        SDL_UnlockMutex(s_lock);

        Net_Run(&job, &res);
        free(job.wav);
        free(job.replyMsgs);
        free(job.judgeText);

        SDL_LockMutex(s_lock);
        if (s_resultCount < NET_QUEUE)
            s_results[s_resultCount++] = res;
        else
            PcConNet_Free(&res);
        s_inFlight = 0;
        SDL_UnlockMutex(s_lock);
    }
    return 0;
}

void PcConNet_Start(void)
{
    if (s_thread)
        return;
    s_lock   = SDL_CreateMutex();
    s_cond   = SDL_CreateCond();
    s_thread = SDL_CreateThread(Net_Thread, "conscious-net", NULL);
    if (s_thread)
        SDL_DetachThread(s_thread);
    SH_DBG("[CONSCIOUS] worker %s", s_thread ? "started" : "FAILED to start");
}

static void Net_Push(const NetJob* j)
{
    if (!s_thread)
        PcConNet_Start();
    SDL_LockMutex(s_lock);
    if (s_jobCount < NET_QUEUE)
    {
        s_jobs[s_jobCount++] = *j;
    }
    else
    {
        free(j->wav);
        free(j->replyMsgs);
        free(j->judgeText);
    }
    SDL_CondSignal(s_cond);
    SDL_UnlockMutex(s_lock);
}

void PcConNet_SubmitStt(unsigned int gen, unsigned char* wav, unsigned long wavLen)
{
    NetJob j;
    memset(&j, 0, sizeof(j));
    j.kind   = PCCON_JOB_STT;
    j.gen    = gen;
    j.wav    = wav;
    j.wavLen = wavLen;
    Net_Push(&j);
}

void PcConNet_SubmitTurn(unsigned int gen, char* replyMsgs, char* judgeText, int forced, const char* voiceId)
{
    NetJob j;
    memset(&j, 0, sizeof(j));
    j.kind      = PCCON_JOB_TURN;
    j.gen       = gen;
    j.replyMsgs = replyMsgs;
    j.judgeText = judgeText;
    j.forced    = forced;
    if (voiceId)
        SDL_strlcpy(j.voiceId, voiceId, sizeof(j.voiceId));
    Net_Push(&j);
}

int PcConNet_Poll(PcConResult* out)
{
    int got = 0;
    if (!s_lock)
        return 0;
    SDL_LockMutex(s_lock);
    if (s_resultCount > 0)
    {
        *out = s_results[0];
        memmove(&s_results[0], &s_results[1], sizeof(s_results[0]) * (size_t)(s_resultCount - 1));
        s_resultCount--;
        got = 1;
    }
    SDL_UnlockMutex(s_lock);
    return got;
}

int PcConNet_Busy(void)
{
    int b;
    if (!s_lock)
        return 0;
    SDL_LockMutex(s_lock);
    b = s_inFlight || s_jobCount > 0;
    SDL_UnlockMutex(s_lock);
    return b;
}

void PcConNet_Free(PcConResult* r)
{
    free(r->text);
    free(r->pcm);
    r->text = NULL;
    r->pcm  = NULL;
}
