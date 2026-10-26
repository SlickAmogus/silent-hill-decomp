/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_conscious_cfg.c - conscious.cfg beside the exe.
 *
 * Its own file rather than config.cfg: the launcher rewrites config.cfg, and
 * API keys have no business in a file the launcher round-trips. An empty key
 * falls back to Morpheus's keystore files (base64 of the key XORed with
 * "MORPHEUS_SECURE_KEYSTORE"), so the keys already entered there are reused
 * without being copied anywhere.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#include "pc_conscious.h"
#include "sh_log.h"

#define CFG_PATH "conscious.cfg"

PcConCfg     g_PcCon;
PcConProfile g_PcConProf[PCCON_PROF_N];

static const char* const s_profSlug[PCCON_PROF_N] = { "grey_children", "groaner", "romper", "air_screamer",
                                                      "night_flutter" };
static const char* const s_profName[PCCON_PROF_N] = { "Grey Children", "Groaner", "Romper", "Air Screamer",
                                                      "Night Flutter" };

static const struct { const char* name; const char* text; } s_moods[PCCON_MOOD_N] = {
    { "As written", NULL },
    { "Calm",       "Right now you are calm, slow and quietly menacing; you never raise your voice." },
    { "Playful",    "Right now you are playful and teasing, enjoying the game of it far too much." },
    { "Furious",    "Right now you are furious, barely holding yourself back, snapping at every word." },
    { "Scared",     "Right now you are secretly scared of him and trying hard to hide it behind bluster." },
    { "Sad",        "Right now you are sad and lonely, a little desperate for someone to talk to." },
    { "Sarcastic",  "Right now you are dry and sarcastic, unimpressed by everything he says." },
    { "Sinister",   "Right now you are cold and sinister, speaking softly, enjoying his fear." },
    { "Friendly",   "Right now you are oddly friendly, almost as if you like him -- but you are still a monster." },
    { "Unhinged",   "Right now you are unhinged and erratic: you laugh at odd moments and change topic suddenly." },
};

const char* PcCon_ProfName(int prof)
{
    return prof >= 0 && prof < PCCON_PROF_N ? s_profName[prof] : "";
}

const char* PcCon_MoodName(int mood)
{
    return mood >= 0 && mood < PCCON_MOOD_N ? s_moods[mood].name : s_moods[0].name;
}

const char* PcCon_MoodText(int mood)
{
    return mood > 0 && mood < PCCON_MOOD_N ? s_moods[mood].text : NULL;
}

void PcCon_PersonaPath(int prof, char* out, int cap)
{
    snprintf(out, (size_t)cap, "conscious_%s_personality.txt", s_profSlug[prof]);
}

int PcCon_PersonaLoad(int prof, char* out, int cap)
{
    char   path[96];
    FILE*  f;
    size_t n;

    out[0] = '\0';
    PcCon_PersonaPath(prof, path, (int)sizeof(path));
    f = fopen(path, "rb");
    if (!f)
        return 0;
    n = fread(out, 1, (size_t)cap - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        out[--n] = '\0';
    return n > 0;
}

/* What the menu edits. The same table reads them and writes them back. */
enum { CI_INT, CI_FLT, CI_STR, CI_MOOD };
typedef struct
{
    char  name[48];
    int   type;
    void* p;
    int   cap;
} CfgItem;

#define CFG_ITEMS_MAX 64

static int Cfg_Items(CfgItem* it)
{
    int n = 0, i;
#define ADD(nm, ty, ptr, c) do { SDL_strlcpy(it[n].name, nm, sizeof(it[n].name)); it[n].type = ty; \
                                 it[n].p = (void*)(ptr); it[n].cap = c; n++; } while (0)
    ADD("enabled", CI_INT, &g_PcCon.enabled, 0);
    ADD("monsters", CI_INT, &g_PcCon.monsters, 0);
    ADD("tts", CI_INT, &g_PcCon.tts, 0);
    ADD("stt", CI_INT, &g_PcCon.stt, 0);
    ADD("mic_start_on", CI_INT, &g_PcCon.micStartOn, 0);
    ADD("vision", CI_INT, &g_PcCon.vision, 0);
    ADD("chat_log", CI_INT, &g_PcCon.chatLog, 0);
    ADD("bubbles", CI_INT, &g_PcCon.bubbles, 0);
    ADD("prompts", CI_INT, &g_PcCon.prompts, 0);
    ADD("voice_volume", CI_FLT, &g_PcCon.voiceVolume, 0);
    ADD("monster_incite_chance", CI_FLT, &g_PcCon.monsterInciteChance, 0);
    ADD("max_turns", CI_INT, &g_PcCon.maxTurns, 0);
    ADD("monster_max_turns", CI_INT, &g_PcCon.monsterMaxTurns, 0);
    ADD("listen_seconds", CI_FLT, &g_PcCon.listenSec, 0);
    ADD("monster_hear_range", CI_FLT, &g_PcCon.monsterHearRange, 0);
    ADD("llm_temperature", CI_FLT, &g_PcCon.llmTemperature, 0);
    ADD("player_name", CI_STR, g_PcCon.playerName, (int)sizeof(g_PcCon.playerName));
    ADD("key_menu", CI_STR, g_PcCon.keyMenu, (int)sizeof(g_PcCon.keyMenu));
    for (i = 0; i < PCCON_PROF_N; i++)
    {
        PcConProfile* p = &g_PcConProf[i];
        char          k[48];
#define PADD(suffix, ty, ptr, c) do { snprintf(k, sizeof(k), "%s_%s", s_profSlug[i], suffix); ADD(k, ty, ptr, c); } while (0)
        PADD("talk", CI_INT, &p->talk, 0);
        PADD("voice", CI_STR, p->voice, (int)sizeof(p->voice));
        PADD("mood", CI_MOOD, &p->mood, 0);
        PADD("pitch", CI_FLT, &p->pitch, 0);
        PADD("stability", CI_FLT, &p->stability, 0);
        PADD("style", CI_FLT, &p->style, 0);
        PADD("speed", CI_FLT, &p->speed, 0);
        PADD("extra", CI_STR, p->extra, (int)sizeof(p->extra));
#undef PADD
    }
#undef ADD
    return n;
}

static int Cfg_ItemSet(const char* k, const char* v)
{
    CfgItem it[CFG_ITEMS_MAX];
    int     n = Cfg_Items(it), i, m;

    for (i = 0; i < n; i++)
    {
        if (strcmp(it[i].name, k))
            continue;
        switch (it[i].type)
        {
        case CI_INT: *(int*)it[i].p = atoi(v); break;
        case CI_FLT: *(float*)it[i].p = (float)atof(v); break;
        case CI_STR: SDL_strlcpy((char*)it[i].p, v, (size_t)it[i].cap); break;
        default:
            for (m = 0; m < PCCON_MOOD_N; m++)
                if (!SDL_strcasecmp(v, s_moods[m].name))
                    *(int*)it[i].p = m;
            break;
        }
        return 1;
    }
    return 0;
}

static void Cfg_ItemFormat(const CfgItem* it, char* out, int cap)
{
    switch (it->type)
    {
    case CI_INT: snprintf(out, (size_t)cap, "%s = %d\n", it->name, *(const int*)it->p); break;
    case CI_FLT: snprintf(out, (size_t)cap, "%s = %.2f\n", it->name, *(const float*)it->p); break;
    case CI_STR: snprintf(out, (size_t)cap, "%s = %s\n", it->name, (const char*)it->p); break;
    default:     snprintf(out, (size_t)cap, "%s = %s\n", it->name, s_moods[*(const int*)it->p].name); break;
    }
}

static const char s_template[] =
    "# Silent Hill: conscious grey children. Read once at startup.\n"
    "# Empty keys fall back to Morpheus's keystore files in morpheus_dir.\n"
    "enabled = 1\n"
    "\n"
    "# Any OpenAI-compatible chat endpoint. Local examples:\n"
    "#   Ollama:    http://localhost:11434/v1/chat/completions  (model e.g. llama3.1)\n"
    "#   LM Studio: http://localhost:1234/v1/chat/completions\n"
    "llm_url = https://api.deepseek.com/chat/completions\n"
    "llm_model = deepseek-chat\n"
    "llm_key =\n"
    "llm_temperature = 1.1\n"
    "llm_max_tokens = 120\n"
    "\n"
    "# The second call that decides attack / let go / keep talking. Empty = same as above.\n"
    "judge_url =\n"
    "judge_model =\n"
    "judge_key =\n"
    "\n"
    "# 1 = ElevenLabs, 0 = off (text in chat only / typed chat only)\n"
    "tts = 1\n"
    "stt = 1\n"
    "elevenlabs_key =\n"
    "# Jimbo - Thick Australian Accent (ElevenLabs library voice)\n"
    "voice_id = YLbQE9U7P1K6rBNJWNSv\n"
    "tts_model = eleven_turbo_v2_5\n"
    "voice_pitch = 1.0\n"
    "voice_stability = 0.3\n"
    "voice_similarity = 0.75\n"
    "voice_style = 0.65\n"
    "\n"
    "morpheus_dir = C:/Claude/morpheus/src/bin/Debug/net8.0\n"
    "\n"
    "listen_seconds = 30\n"
    "patience_seconds = 240\n"
    "min_words_first = 5\n"
    "min_words = 2\n"
    "max_turns = 8\n"
    "hear_range = 30\n"
    "key_mic = V\n"
    "key_chat = Y\n"
    "mic_start_on = 0\n"
    "vad_threshold = 0.02\n"
    "vad_hang_seconds = 1.1\n"
    "player_name = Harry\n"
    "\n"
    "# M in game opens a menu for everything below and per-monster voice, mood and personality.\n"
    "key_menu = M\n"
    "# 0 = speech bubbles only, no chat feed.\n"
    "chat_log = 1\n"
    "bubbles = 1\n"
    "prompts = 1\n"
    "voice_volume = 1.0\n"
    "\n"
    "# Groaners, Rompers and Air Screamers stop and talk when Harry speaks near them.\n"
    "monsters = 1\n"
    "monster_hear_range = 12\n"
    "monster_flee_range = 18\n"
    "monster_max_turns = 8\n"
    "# Callum, Charlie, George, Daniel, Oliver Silk, Harry (Fierce Warrior), Adam, Brian\n"
    "monster_voices = N2lVS1w4EtoT3dr4eOWO,IKne3meq5aSn9XLyUdCD,JBFqnCBsd6RMkjVDRZzb,onwK4e9ZLuTAKqWW03F9,jfIS2w2yJi0grJZPyEsk,SOYHLrjzK2X1ezoPC6cr,pNInz6obpgDQGcFmaJgB,nPczCjzI2devNBz1zQrb\n"
    "# When Harry tries to turn two of them on each other: the chance they fall for it\n"
    "# at all (then they argue, and either fight each other or come for him together).\n"
    "monster_incite_chance = 0.35\n"
    "\n"
    "# 1 = each of Harry's lines goes out with a screenshot, so they can see what he means.\n"
    "# Needs a model that takes images (deepseek-chat / deepseek-flash do); one that\n"
    "# refuses them gets text only from then on.\n"
    "vision = 1\n"
    "vision_width = 640\n"
    "# Writes the last shot to conscious_view.jpg next to the game.\n"
    "vision_save = 1\n";

static void Cfg_Defaults(PcConCfg* c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = 1;
    SDL_strlcpy(c->llmUrl, "https://api.deepseek.com/chat/completions", sizeof(c->llmUrl));
    SDL_strlcpy(c->llmModel, "deepseek-chat", sizeof(c->llmModel));
    c->llmTemperature = 1.1f;
    c->llmMaxTokens   = 120;
    c->tts = 1;
    c->stt = 1;
    SDL_strlcpy(c->voiceId, "YLbQE9U7P1K6rBNJWNSv", sizeof(c->voiceId));
    SDL_strlcpy(c->ttsModel, "eleven_turbo_v2_5", sizeof(c->ttsModel));
    c->voicePitch      = 1.0f;
    c->voiceStability  = 0.3f;
    c->voiceSimilarity = 0.75f;
    c->voiceStyle      = 0.65f;
    SDL_strlcpy(c->morpheusDir, "C:/Claude/morpheus/src/bin/Debug/net8.0", sizeof(c->morpheusDir));
    c->listenSec     = 30.0f;
    c->patienceSec   = 240.0f;
    c->minWordsFirst = 5;
    c->minWords      = 2;
    c->maxTurns      = 8;
    c->hearRange     = 30.0f;
    SDL_strlcpy(c->keyMic, "V", sizeof(c->keyMic));
    SDL_strlcpy(c->keyChat, "Y", sizeof(c->keyChat));
    c->vadThreshold = 0.02f;
    c->vadHangSec   = 1.1f;
    SDL_strlcpy(c->playerName, "Harry", sizeof(c->playerName));
    c->monsters         = 1;
    c->monsterHearRange = 12.0f;
    c->monsterFleeRange = 18.0f;
    c->monsterMaxTurns  = 8;
    SDL_strlcpy(c->monsterVoices, "N2lVS1w4EtoT3dr4eOWO,IKne3meq5aSn9XLyUdCD,JBFqnCBsd6RMkjVDRZzb,onwK4e9ZLuTAKqWW03F9,jfIS2w2yJi0grJZPyEsk,SOYHLrjzK2X1ezoPC6cr,pNInz6obpgDQGcFmaJgB,nPczCjzI2devNBz1zQrb", sizeof(c->monsterVoices));
    c->monsterInciteChance = 0.35f;
    c->vision      = 1;
    c->visionWidth = 640;
    c->visionSave  = 1;
    c->chatLog     = 1;
    c->bubbles     = 1;
    c->prompts     = 1;
    c->voiceVolume = 1.0f;
    SDL_strlcpy(c->keyMenu, "M", sizeof(c->keyMenu));
}

static void Cfg_ProfDefaults(void)
{
    int i;
    memset(g_PcConProf, 0, sizeof(g_PcConProf));
    for (i = 0; i < PCCON_PROF_N; i++)
    {
        /* Negative: not in the file, take voice_pitch / _stability / _style. */
        g_PcConProf[i].talk      = 1;
        g_PcConProf[i].pitch     = -1.0f;
        g_PcConProf[i].stability = -1.0f;
        g_PcConProf[i].style     = -1.0f;
        g_PcConProf[i].speed     = 1.0f;
    }
}

static char* Cfg_Trim(char* s)
{
    char* e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void Cfg_Set(PcConCfg* c, const char* k, const char* v)
{
#define STR(name, field) if (!strcmp(k, name)) { SDL_strlcpy(c->field, v, sizeof(c->field)); return; }
#define INT(name, field) if (!strcmp(k, name)) { c->field = atoi(v); return; }
#define FLT(name, field) if (!strcmp(k, name)) { c->field = (float)atof(v); return; }
    if (Cfg_ItemSet(k, v))
        return;
    STR("llm_url", llmUrl) STR("llm_model", llmModel) STR("llm_key", llmKey)
    INT("llm_max_tokens", llmMaxTokens)
    STR("judge_url", judgeUrl) STR("judge_model", judgeModel) STR("judge_key", judgeKey)
    STR("elevenlabs_key", elevenKey) STR("voice_id", voiceId) STR("tts_model", ttsModel)
    FLT("voice_pitch", voicePitch) FLT("voice_stability", voiceStability)
    FLT("voice_similarity", voiceSimilarity) FLT("voice_style", voiceStyle)
    STR("morpheus_dir", morpheusDir)
    FLT("patience_seconds", patienceSec)
    INT("min_words_first", minWordsFirst) INT("min_words", minWords)
    FLT("hear_range", hearRange)
    STR("key_mic", keyMic) STR("key_chat", keyChat)
    FLT("vad_threshold", vadThreshold) FLT("vad_hang_seconds", vadHangSec)
    FLT("monster_flee_range", monsterFleeRange)
    STR("monster_voices", monsterVoices)
    INT("vision_width", visionWidth) INT("vision_save", visionSave)
#undef STR
#undef INT
#undef FLT
    SH_DBG("[CONSCIOUS] conscious.cfg: unknown key '%s'", k);
}

static int B64_Val(int ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static void Cfg_MorpheusKey(const char* file, char* out, int cap)
{
    static const char mask[] = "MORPHEUS_SECURE_KEYSTORE";
    char  path[512];
    char  b64[1024];
    FILE* f;
    int   n = 0, bits = 0, acc = 0, i;
    size_t got;

    if (out[0] || !g_PcCon.morpheusDir[0])
    {
        return;
    }
    snprintf(path, sizeof(path), "%s/%s", g_PcCon.morpheusDir, file);
    f = fopen(path, "rb");
    if (!f)
    {
        return;
    }
    got = fread(b64, 1, sizeof(b64) - 1, f);
    fclose(f);
    b64[got] = '\0';
    for (i = 0; b64[i] && n < cap - 1; i++)
    {
        int v = B64_Val((unsigned char)b64[i]);
        if (v < 0) continue;
        acc  = (acc << 6) | v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out[n] = (char)(((acc >> bits) & 0xFF) ^ (unsigned char)mask[n % (sizeof(mask) - 1)]);
            n++;
        }
    }
    out[n] = '\0';
    if (n > 0)
    {
        SH_DBG("[CONSCIOUS] using the Morpheus key from %s", file);
    }
}

void PcCon_CfgLoad(void)
{
    FILE* f;
    char  line[512];

    Cfg_Defaults(&g_PcCon);
    Cfg_ProfDefaults();
    f = fopen(CFG_PATH, "rb");
    if (!f)
    {
        f = fopen(CFG_PATH, "wb");
        if (f)
        {
            fputs(s_template, f);
            fclose(f);
            SH_DBG("[CONSCIOUS] wrote a default %s", CFG_PATH);
        }
    }
    else
    {
        while (fgets(line, sizeof(line), f))
        {
            char* s = Cfg_Trim(line);
            char* eq;
            if (!s[0] || s[0] == '#' || s[0] == ';') continue;
            eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            Cfg_Set(&g_PcCon, Cfg_Trim(s), Cfg_Trim(eq + 1));
        }
        fclose(f);
    }

    if (!g_PcCon.judgeUrl[0])   SDL_strlcpy(g_PcCon.judgeUrl, g_PcCon.llmUrl, sizeof(g_PcCon.judgeUrl));
    if (!g_PcCon.judgeModel[0]) SDL_strlcpy(g_PcCon.judgeModel, g_PcCon.llmModel, sizeof(g_PcCon.judgeModel));

    if (strstr(g_PcCon.llmUrl, "deepseek"))
        Cfg_MorpheusKey("keystore-deepseek.local.dat", g_PcCon.llmKey, (int)sizeof(g_PcCon.llmKey));
    if (!g_PcCon.judgeKey[0] && !strcmp(g_PcCon.judgeUrl, g_PcCon.llmUrl))
        SDL_strlcpy(g_PcCon.judgeKey, g_PcCon.llmKey, sizeof(g_PcCon.judgeKey));
    if (strstr(g_PcCon.judgeUrl, "deepseek"))
        Cfg_MorpheusKey("keystore-deepseek.local.dat", g_PcCon.judgeKey, (int)sizeof(g_PcCon.judgeKey));
    /* Read even with both off: the menu can turn them on. */
    Cfg_MorpheusKey("keystore.local.dat", g_PcCon.elevenKey, (int)sizeof(g_PcCon.elevenKey));

    if (g_PcCon.listenSec < 5.0f) g_PcCon.listenSec = 5.0f;
    if (g_PcCon.hearRange < 4.0f) g_PcCon.hearRange = 4.0f;
    if (g_PcCon.maxTurns < 1)     g_PcCon.maxTurns = 1;
    if (g_PcCon.monsterHearRange < 3.0f) g_PcCon.monsterHearRange = 3.0f;
    if (g_PcCon.monsterFleeRange < g_PcCon.monsterHearRange + 2.0f)
        g_PcCon.monsterFleeRange = g_PcCon.monsterHearRange + 2.0f;
    if (g_PcCon.monsterMaxTurns < 1) g_PcCon.monsterMaxTurns = 1;
    if (g_PcCon.monsterInciteChance < 0.0f) g_PcCon.monsterInciteChance = 0.0f;
    if (g_PcCon.monsterInciteChance > 1.0f) g_PcCon.monsterInciteChance = 1.0f;
    if (g_PcCon.visionWidth < 160)   g_PcCon.visionWidth = 160;
    if (g_PcCon.visionWidth > 1600)  g_PcCon.visionWidth = 1600;
    {
        int i;
        for (i = 0; i < PCCON_PROF_N; i++)
        {
            PcConProfile* p = &g_PcConProf[i];
            if (p->pitch < 0.0f)     p->pitch = g_PcCon.voicePitch;
            if (p->stability < 0.0f) p->stability = g_PcCon.voiceStability;
            if (p->style < 0.0f)     p->style = g_PcCon.voiceStyle;
        }
    }

    SH_DBG("[CONSCIOUS] enabled=%d llm=%s (%s, key %s) judge=%s tts=%d stt=%d eleven key %s voice=%s",
           g_PcCon.enabled, g_PcCon.llmUrl, g_PcCon.llmModel, g_PcCon.llmKey[0] ? "set" : "none",
           g_PcCon.judgeModel, g_PcCon.tts, g_PcCon.stt, g_PcCon.elevenKey[0] ? "set" : "none",
           g_PcCon.voiceId);
}

/* Line by line: a line setting something the menu owns gets the current
 * value, every other line is copied as it was. The keys are loaded into
 * g_PcCon from Morpheus's keystore, so writing the struct out wholesale would
 * put them in the file. */
void PcCon_CfgSave(void)
{
    CfgItem it[CFG_ITEMS_MAX];
    int     done[CFG_ITEMS_MAX];
    int     n = Cfg_Items(it), i, header = 0;
    char*   old = NULL;
    long    len = 0;
    FILE*   f;
    char    buf[400];

    memset(done, 0, sizeof(done));
    f = fopen(CFG_PATH, "rb");
    if (f)
    {
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        old = (char*)malloc((size_t)len + 1);
        if (old)
            len = (long)fread(old, 1, (size_t)len, f);
        fclose(f);
        if (old)
            old[len] = '\0';
    }
    f = fopen(CFG_PATH, "wb");
    if (!f)
    {
        SH_DBG("[CONSCIOUS] could not write %s", CFG_PATH);
        free(old);
        return;
    }
    if (old)
    {
        char* line = old;
        while (*line)
        {
            char* nl   = strchr(line, '\n');
            char* next = nl ? nl + 1 : line + strlen(line);
            char  copy[512];
            char* k;
            char* eq;
            int   hit = -1;

            SDL_strlcpy(copy, line, (size_t)(next - line + 1) < sizeof(copy) ? (size_t)(next - line + 1) : sizeof(copy));
            k = Cfg_Trim(copy);
            eq = strchr(k, '=');
            if (k[0] && k[0] != '#' && k[0] != ';' && eq)
            {
                *eq = '\0';
                k   = Cfg_Trim(k);
                for (i = 0; i < n; i++)
                    if (!strcmp(it[i].name, k))
                        hit = i;
            }
            if (hit >= 0 && !done[hit])
            {
                Cfg_ItemFormat(&it[hit], buf, (int)sizeof(buf));
                fputs(buf, f);
                done[hit] = 1;
            }
            else if (hit < 0)
            {
                fwrite(line, 1, (size_t)(next - line), f);
                if (!nl)
                    fputc('\n', f);
            }
            line = next;
        }
    }
    for (i = 0; i < n; i++)
    {
        if (done[i])
            continue;
        if (!header)
        {
            fputs("\n# Set from the in-game menu (key_menu).\n", f);
            header = 1;
        }
        Cfg_ItemFormat(&it[i], buf, (int)sizeof(buf));
        fputs(buf, f);
    }
    fclose(f);
    free(old);
    SH_DBG("[CONSCIOUS] settings saved to %s", CFG_PATH);
}
