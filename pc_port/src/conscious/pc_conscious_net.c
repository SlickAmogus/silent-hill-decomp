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

#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

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
    unsigned char* shot;
    int            shotW, shotH;
    int            judgeKind;
    char*          incite;
    int            inciteFalls;
    PcConVoiceTune tune;
} NetJob;

static SDL_Thread* s_thread;
static SDL_mutex*  s_lock;
static SDL_cond*   s_cond;
static NetJob      s_jobs[NET_QUEUE];
static int         s_jobCount;
static PcConResult s_results[NET_QUEUE];
static int         s_resultCount;
static int         s_inFlight;
static int         s_visionRefused;

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

/* Last mention wins: a reasoning model may list every word before deciding. */
static int Net_LastWord(char* ans, const char* const* words, const int* decisions, int n, int fallback)
{
    const char* best = NULL;
    int         d    = fallback;
    int         i;
    char*       p;

    for (p = ans; *p; p++) *p = (char)toupper((unsigned char)*p);
    for (i = 0; i < n; i++)
    {
        const char* q;
        for (q = strstr(ans, words[i]); q; q = strstr(q + 1, words[i]))
            if (!best || q > best) { best = q; d = decisions[i]; }
    }
    return d;
}

static char* Net_OneShot(const char* sys, const char* user, int maxTokens)
{
    size_t cap = strlen(sys) * 2 + strlen(user) * 2 + 96;
    char*  msgs = (char*)malloc(cap);
    char*  ans;

    if (!msgs)
        return NULL;
    msgs[0] = '\0';
    strcat(msgs, "[{\"role\":\"system\",\"content\":\"");
    PcCon_JsonEscapeCat(msgs, (int)cap - 64, sys);
    strcat(msgs, "\"},{\"role\":\"user\",\"content\":\"");
    PcCon_JsonEscapeCat(msgs, (int)cap - 8, user);
    strcat(msgs, "\"}]");
    ans = Net_Chat(g_PcCon.judgeUrl, g_PcCon.judgeModel, g_PcCon.judgeKey, msgs, maxTokens, 0.0f);
    free(msgs);
    if (ans)
        Text_Clean(ans);
    return ans;
}

static int Net_JudgeArgue(const char* judgeText)
{
    static const char sys[] =
        "You read a scene from a horror game. A man named Harry tried to turn two monsters against each other, "
        "and they argued about it. Look ONLY at the FIRST monster's LATEST line and decide what it does now. "
        "Answer with exactly one word: FIGHT or UNITE.\n"
        "FIGHT = it believes Harry, it turns on the other monster, it attacks / kills / bites the other one.\n"
        "UNITE = it sees Harry tricked them, it sides with the other monster, they go after Harry together.";
    static const char* const words[] = { "FIGHT", "UNITE" };
    static const int         dec[]   = { PCCON_DECIDE_FIGHT, PCCON_DECIDE_UNITE };
    char* ans = Net_OneShot(sys, judgeText, 400);
    int   d;

    if (!ans)
        return PCCON_DECIDE_UNITE;
    d = Net_LastWord(ans, words, dec, 2, PCCON_DECIDE_UNITE);
    SH_DBG("[CONSCIOUS] argue judge said '%.60s' -> %d", ans, d);
    free(ans);
    return d;
}

/* Is he trying to set them on each other? Decided before the reply is
 * written, because the reply has to know whether it worked. */
static int Net_Incite(const char* scene)
{
    static const char sys[] =
        "You read a scene from a horror game. A man named Harry is talking to a group of monsters that "
        "stopped to listen to him. Decide whether his LATEST line is trying to turn them against EACH OTHER: "
        "telling one that another betrayed it, mocked it, is after its food or plans to eat it, that it is "
        "weaker or the other one is in charge, or urging them to fight each other. "
        "Insulting or threatening the monsters themselves does NOT count. Answer with exactly one word: YES or NO.";
    static const char* const words[] = { "YES", "NO" };
    static const int         dec[]   = { 1, 0 };
    char* ans = Net_OneShot(sys, scene, 300);
    int   d;

    if (!ans)
        return 0;
    d = Net_LastWord(ans, words, dec, 2, 0);
    SH_DBG("[CONSCIOUS] incite check said '%.40s' -> %d", ans, d);
    free(ans);
    return d;
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
    static const char* const words[] = { "ATTACK", "RELEASE", "TALK" };
    static const int         dec[]   = { PCCON_DECIDE_ATTACK, PCCON_DECIDE_RELEASE, PCCON_DECIDE_TALK };
    char* ans = Net_OneShot(sys, judgeText, 400);
    int   d;

    if (!ans)
        return PCCON_DECIDE_TALK;
    d = Net_LastWord(ans, words, dec, 3, PCCON_DECIDE_TALK);
    SH_DBG("[CONSCIOUS] judge said '%.60s' -> %d", ans, d);
    free(ans);
    return d;
}

/* They already let him go: only a blatant insult undoes that, never a slow
 * or awkward answer. */
static int Net_JudgeSpared(const char* judgeText)
{
    static const char sys[] =
        "You read a scene from a horror game. Monsters already decided to let a man named Harry go, and he is "
        "still chatting with them. They only turn on him again if HARRY'S OWN latest words BLATANTLY insult, "
        "mock or threaten them AND their latest line says they are attacking him now. "
        "Rudeness that is mild, jokes, questions, silence, nonsense or him staying around do NOT count. "
        "Answer with exactly one word: ATTACK or TALK.";
    static const char* const words[] = { "ATTACK", "TALK" };
    static const int         dec[]   = { PCCON_DECIDE_ATTACK, PCCON_DECIDE_TALK };
    char* ans = Net_OneShot(sys, judgeText, 400);
    int   d;

    if (!ans)
        return PCCON_DECIDE_TALK;
    d = Net_LastWord(ans, words, dec, 2, PCCON_DECIDE_TALK);
    SH_DBG("[CONSCIOUS] spared judge said '%.60s' -> %d", ans, d);
    free(ans);
    return d;
}

/* Dark Harry does not bargain: he stops only when he is genuinely talked
 * down, so the default is that he keeps coming. */
static int Net_JudgeDark(const char* judgeText)
{
    static const char sys[] =
        "You read a scene from a horror game. A dark copy of a man named Harry is walking toward the real "
        "Harry to kill him, and Harry keeps talking to make it stop. It is very hard to convince: it stops "
        "ONLY if its LATEST line clearly says it is stopping for good, turning away, leaving, letting him go, "
        "giving up, or that he has won / was right / is the real one. Taunts, questions, doubts, hesitation, "
        "threats, 'maybe', pausing to think, or saying it is still coming all mean it keeps coming. "
        "Answer with exactly one word: STOP or COMING.";
    static const char* const words[] = { "STOP", "COMING" };
    static const int         dec[]   = { PCCON_DECIDE_RELEASE, PCCON_DECIDE_TALK };
    char* ans = Net_OneShot(sys, judgeText, 400);
    int   d;

    if (!ans)
        return PCCON_DECIDE_TALK;
    d = Net_LastWord(ans, words, dec, 2, PCCON_DECIDE_TALK);
    SH_DBG("[CONSCIOUS] dark judge said '%.60s' -> %d", ans, d);
    free(ans);
    return d;
}

static short* Net_Tts(const char* text, const char* voiceId, const PcConVoiceTune* tune, unsigned int* outSamples)
{
    char   url[256], hdr[320];
    char   body[2048];
    char*  resp = NULL;
    unsigned long respLen = 0;
    int    status;
    float  stability = g_PcCon.voiceStability, style = g_PcCon.voiceStyle, speed = 1.0f;

    *outSamples = 0;
    if (tune && tune->speed > 0.0f)
    {
        stability = tune->stability;
        style     = tune->style;
        speed     = tune->speed;
    }
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
             "\"style\":%.2f,\"speed\":%.2f,\"use_speaker_boost\":true}}",
             g_PcCon.ttsModel, stability, g_PcCon.voiceSimilarity, style, speed);

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
/* What they see                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    unsigned char* p;
    size_t         n, cap;
} JpgBuf;

static void Jpg_Put(void* ctx, void* data, int size)
{
    JpgBuf* b = (JpgBuf*)ctx;
    if (b->n + (size_t)size > b->cap)
    {
        size_t         cap = (b->cap ? b->cap * 2 : 65536) + (size_t)size;
        unsigned char* p   = (unsigned char*)realloc(b->p, cap);
        if (!p) return;
        b->p   = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, data, (size_t)size);
    b->n += (size_t)size;
}

/* Bottom-up RGBA -> a "data:image/jpeg;base64,..." URL, box-filtered down to
 * vision_width. The game is dark and foggy, so averaging rather than point
 * sampling keeps what little detail there is. */
static char* Shot_DataUrl(const unsigned char* rgba, int w, int h)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char head[] = "data:image/jpeg;base64,";
    int            dw = w < g_PcCon.visionWidth ? w : g_PcCon.visionWidth;
    int            dh = (int)((long long)h * dw / w);
    unsigned char* rgb;
    JpgBuf         jpg = { NULL, 0, 0 };
    char*          url;
    char*          o;
    size_t         i;
    int            x, y;

    if (dh < 1)
        return NULL;
    rgb = (unsigned char*)malloc((size_t)dw * (size_t)dh * 3);
    if (!rgb)
        return NULL;
    for (y = 0; y < dh; y++)
    {
        int y0 = (int)((long long)y * h / dh), y1 = (int)((long long)(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < dw; x++)
        {
            int           x0 = (int)((long long)x * w / dw), x1 = (int)((long long)(x + 1) * w / dw);
            unsigned long sum[3] = { 0, 0, 0 }, n = 0;
            int           sx, sy;
            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1; sy++)
            {
                const unsigned char* row = rgba + (size_t)(h - 1 - sy) * (size_t)w * 4;
                for (sx = x0; sx < x1; sx++, n++)
                {
                    sum[0] += row[sx * 4 + 0];
                    sum[1] += row[sx * 4 + 1];
                    sum[2] += row[sx * 4 + 2];
                }
            }
            rgb[((size_t)y * dw + x) * 3 + 0] = (unsigned char)(sum[0] / n);
            rgb[((size_t)y * dw + x) * 3 + 1] = (unsigned char)(sum[1] / n);
            rgb[((size_t)y * dw + x) * 3 + 2] = (unsigned char)(sum[2] / n);
        }
    }
    stbi_write_jpg_to_func(Jpg_Put, &jpg, dw, dh, 3, rgb, 80);
    free(rgb);
    if (!jpg.n)
    {
        free(jpg.p);
        return NULL;
    }
    if (g_PcCon.visionSave)
    {
        FILE* f = fopen("conscious_view.jpg", "wb");
        if (f)
        {
            fwrite(jpg.p, 1, jpg.n, f);
            fclose(f);
        }
    }

    url = (char*)malloc(sizeof(head) + (jpg.n + 2) / 3 * 4 + 1);
    if (url)
    {
        strcpy(url, head);
        o = url + sizeof(head) - 1;
        for (i = 0; i < jpg.n; i += 3)
        {
            unsigned v = (unsigned)jpg.p[i] << 16;
            if (i + 1 < jpg.n) v |= (unsigned)jpg.p[i + 1] << 8;
            if (i + 2 < jpg.n) v |= jpg.p[i + 2];
            *o++ = b64[(v >> 18) & 63];
            *o++ = b64[(v >> 12) & 63];
            *o++ = i + 1 < jpg.n ? b64[(v >> 6) & 63] : '=';
            *o++ = i + 2 < jpg.n ? b64[v & 63] : '=';
        }
        *o = '\0';
        SH_DBG("[CONSCIOUS] vision: %dx%d -> %dx%d, %lu byte jpeg", w, h, dw, dh, (unsigned long)jpg.n);
    }
    free(jpg.p);
    return url;
}

/* The same messages with the picture added to the last user message, which is
 * the only place the APIs accept one. NULL if there is no user message. */
static char* Msgs_WithImage(const char* msgs, const char* url)
{
    static const char tag[]  = "{\"role\":\"user\",\"content\":\"";
    static const char lead[] = "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"";
    static const char mid[]  = "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"";
    static const char shut[] = "\"}}]";
    const char* last = NULL;
    const char* p;
    const char* text;
    const char* q;
    char*       out;
    size_t      cap;

    for (p = strstr(msgs, tag); p; p = strstr(p + 1, tag))
        last = p;
    if (!last)
        return NULL;
    text = last + sizeof(tag) - 1;
    for (q = text; *q && *q != '"'; q++)
        if (*q == '\\' && q[1])
            q++;
    if (!*q)
        return NULL;

    cap = strlen(msgs) + strlen(url) + sizeof(lead) + sizeof(mid) + sizeof(shut);
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    memcpy(out, msgs, (size_t)(last - msgs));
    out[last - msgs] = '\0';
    strcat(out, lead);
    strncat(out, text, (size_t)(q - text));
    strcat(out, mid);
    strcat(out, url);
    strcat(out, shut);
    strcat(out, q + 1);
    return out;
}

/* The same messages with note added to the end of the last user message. */
static char* Msgs_WithNote(const char* msgs, const char* note)
{
    static const char tag[] = "{\"role\":\"user\",\"content\":\"";
    const char* last = NULL;
    const char* p;
    const char* q;
    char*       out;
    size_t      cap, n;

    for (p = strstr(msgs, tag); p; p = strstr(p + 1, tag))
        last = p;
    if (!last)
        return NULL;
    for (q = last + sizeof(tag) - 1; *q && *q != '"'; q++)
        if (*q == '\\' && q[1])
            q++;
    if (!*q)
        return NULL;
    cap = strlen(msgs) + strlen(note) * 2 + 16;
    out = (char*)malloc(cap);
    if (!out)
        return NULL;
    n = (size_t)(q - msgs);
    memcpy(out, msgs, n);
    out[n] = '\0';
    strcat(out, "\\n");
    PcCon_JsonEscapeCat(out, (int)cap - (int)strlen(q) - 2, note);
    strcat(out, q);
    return out;
}

/* ------------------------------------------------------------------ */
/* Thread                                                              */
/* ------------------------------------------------------------------ */

/* The account's voices, for the menu to pick from: "id<TAB>name" lines. */
static char* Net_VoiceList(void)
{
    char          hdr[256];
    char*         resp = NULL;
    char*         out;
    const char*   p;
    unsigned long respLen = 0;
    size_t        cap, n = 0;
    int           status;

    if (!g_PcCon.elevenKey[0])
        return NULL;
    snprintf(hdr, sizeof(hdr), "Accept: application/json\r\nxi-api-key: %s\r\n", g_PcCon.elevenKey);
    status = PcCon_Http("GET", "https://api.elevenlabs.io/v1/voices", hdr, NULL, 0, &resp, &respLen);
    if (status != 200 || !resp)
    {
        SH_DBG("[CONSCIOUS] voice list failed: HTTP %d %.200s", status, resp ? resp : "");
        free(resp);
        return NULL;
    }
    cap = respLen / 4 + 64;
    out = (char*)malloc(cap);
    if (!out)
    {
        free(resp);
        return NULL;
    }
    out[0] = '\0';
    /* Each voice object opens with its id, and its name is the next "name". */
    for (p = strstr(resp, "\"voice_id\""); p; p = strstr(p + 10, "\"voice_id\""))
    {
        char* id   = Json_FindString(resp, p, "voice_id");
        char* name = Json_FindString(resp, p, "name");
        if (id && name && n + strlen(id) + strlen(name) + 3 < cap)
            n += (size_t)snprintf(out + n, cap - n, "%s\t%s\n", id, name);
        free(id);
        free(name);
    }
    free(resp);
    SH_DBG("[CONSCIOUS] voice list: %u bytes", (unsigned)n);
    return out;
}

static void Net_Run(NetJob* j, PcConResult* r)
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
    if (j->kind == PCCON_JOB_VOICES)
    {
        r->text = Net_VoiceList();
        r->ok   = (r->text != NULL);
        return;
    }
    if (j->kind == PCCON_JOB_SAY)
    {
        r->pcm = Net_Tts(j->replyMsgs, j->voiceId, &j->tune, &r->samples);
        r->ok  = (r->pcm != NULL);
        return;
    }

    if (j->incite && Net_Incite(j->incite))
    {
        if (j->inciteFalls)
        {
            SH_DBG("[CONSCIOUS] incite: it lands");
            r->ok       = 1;
            r->decision = PCCON_DECIDE_INCITE;
            return;
        }
        else
        {
            char* m = Msgs_WithNote(j->replyMsgs,
                                    "[He is trying to turn you against the others. You see right through it: call "
                                    "it out, mock him for trying, and keep dealing with HIM.]");
            SH_DBG("[CONSCIOUS] incite: they see through it");
            if (m)
            {
                free(j->replyMsgs);
                j->replyMsgs = m;
            }
        }
    }

    if (j->shot && !s_visionRefused)
    {
        char* url  = Shot_DataUrl(j->shot, j->shotW, j->shotH);
        char* msgs = url ? Msgs_WithImage(j->replyMsgs, url) : NULL;
        free(url);
        if (msgs)
        {
            r->text = Net_Chat(g_PcCon.llmUrl, g_PcCon.llmModel, g_PcCon.llmKey, msgs,
                               g_PcCon.llmMaxTokens, g_PcCon.llmTemperature);
            free(msgs);
            if (!r->text)
            {
                r->text = Net_Chat(g_PcCon.llmUrl, g_PcCon.llmModel, g_PcCon.llmKey, j->replyMsgs,
                                   g_PcCon.llmMaxTokens, g_PcCon.llmTemperature);
                /* Text alone got through, so it was the picture: this model takes none. */
                if (r->text)
                {
                    s_visionRefused = 1;
                    SH_DBG("[CONSCIOUS] vision: %s refused the picture, text only from now on", g_PcCon.llmModel);
                }
            }
        }
    }
    if (!r->text)
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
            r->decision = j->judgeKind == PCCON_JUDGE_ARGUE    ? Net_JudgeArgue(t)
                          : j->judgeKind == PCCON_JUDGE_SPARED ? Net_JudgeSpared(t)
                          : j->judgeKind == PCCON_JUDGE_DARK   ? Net_JudgeDark(t)
                                                               : Net_Judge(t);
            free(t);
        }
    }
    r->pcm = Net_Tts(r->text, j->voiceId, &j->tune, &r->samples);
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
        free(job.shot);
        free(job.incite);

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
        free(j->shot);
        free(j->incite);
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

void PcConNet_SubmitTurn(unsigned int gen, char* replyMsgs, char* judgeText, int forced, const PcConTurnOpts* opts)
{
    NetJob j;
    memset(&j, 0, sizeof(j));
    j.kind      = PCCON_JOB_TURN;
    j.gen       = gen;
    j.replyMsgs = replyMsgs;
    j.judgeText = judgeText;
    j.forced    = forced;
    if (opts)
    {
        j.shot        = opts->shot;
        j.shotW       = opts->shotW;
        j.shotH       = opts->shotH;
        j.judgeKind   = opts->judgeKind;
        j.incite      = opts->incite ? SDL_strdup(opts->incite) : NULL;
        j.inciteFalls = opts->inciteFalls;
        j.tune        = opts->tune;
        if (opts->voiceId)
            SDL_strlcpy(j.voiceId, opts->voiceId, sizeof(j.voiceId));
    }
    Net_Push(&j);
}

void PcConNet_SubmitVoices(unsigned int gen)
{
    NetJob j;
    memset(&j, 0, sizeof(j));
    j.kind = PCCON_JOB_VOICES;
    j.gen  = gen;
    Net_Push(&j);
}

void PcConNet_SubmitSay(unsigned int gen, const char* text, const char* voiceId, const PcConVoiceTune* tune)
{
    NetJob j;
    memset(&j, 0, sizeof(j));
    j.kind      = PCCON_JOB_SAY;
    j.gen       = gen;
    j.replyMsgs = SDL_strdup(text);
    if (voiceId)
        SDL_strlcpy(j.voiceId, voiceId, sizeof(j.voiceId));
    if (tune)
        j.tune = *tune;
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
