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

PcConCfg g_PcCon;

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
    "grace_seconds = 30\n"
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
    c->graceSec      = 30.0f;
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
    INT("enabled", enabled)
    STR("llm_url", llmUrl) STR("llm_model", llmModel) STR("llm_key", llmKey)
    FLT("llm_temperature", llmTemperature) INT("llm_max_tokens", llmMaxTokens)
    STR("judge_url", judgeUrl) STR("judge_model", judgeModel) STR("judge_key", judgeKey)
    INT("tts", tts) INT("stt", stt)
    STR("elevenlabs_key", elevenKey) STR("voice_id", voiceId) STR("tts_model", ttsModel)
    FLT("voice_pitch", voicePitch) FLT("voice_stability", voiceStability)
    FLT("voice_similarity", voiceSimilarity) FLT("voice_style", voiceStyle)
    STR("morpheus_dir", morpheusDir)
    FLT("listen_seconds", listenSec) FLT("grace_seconds", graceSec) FLT("patience_seconds", patienceSec)
    INT("min_words_first", minWordsFirst) INT("min_words", minWords) INT("max_turns", maxTurns)
    FLT("hear_range", hearRange)
    STR("key_mic", keyMic) STR("key_chat", keyChat) INT("mic_start_on", micStartOn)
    FLT("vad_threshold", vadThreshold) FLT("vad_hang_seconds", vadHangSec)
    STR("player_name", playerName)
    INT("monsters", monsters) FLT("monster_hear_range", monsterHearRange)
    FLT("monster_flee_range", monsterFleeRange) INT("monster_max_turns", monsterMaxTurns)
    STR("monster_voices", monsterVoices) FLT("monster_incite_chance", monsterInciteChance)
    INT("vision", vision) INT("vision_width", visionWidth) INT("vision_save", visionSave)
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
    if (g_PcCon.tts || g_PcCon.stt)
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

    SH_DBG("[CONSCIOUS] enabled=%d llm=%s (%s, key %s) judge=%s tts=%d stt=%d eleven key %s voice=%s",
           g_PcCon.enabled, g_PcCon.llmUrl, g_PcCon.llmModel, g_PcCon.llmKey[0] ? "set" : "none",
           g_PcCon.judgeModel, g_PcCon.tts, g_PcCon.stt, g_PcCon.elevenKey[0] ? "set" : "none",
           g_PcCon.voiceId);
}
