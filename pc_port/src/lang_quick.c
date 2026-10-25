/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lang_quick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#include "game.h"
#include "lang_pack.h"
#include "lang_ru.h"
#include "lang_text.h"
#include "pc_config.h"
#include "sh_log.h"

typedef struct {
    const char* en;
    const char* tr[4]; /* de, fr, es, it -- UTF-8 */
} s_QuickTranslation;

/* PAL columns for the QUICK. strings. The PC Options rows these overlays also
 * show come from lang_menu.c's s_MenuTr instead, so the two menus agree. */
static const s_QuickTranslation s_QuickTr[] = {
#include "lang_quick_pal.inc"
};

typedef struct {
    const char* en;
    const char* ja;
    const char* zh;
} s_QuickCjk;

/* The Japanese and Chinese packs store the game font's own bytes (Shift-JIS,
 * kuten), which these UTF-8 overlays cannot draw, and a Japanese disc's own two
 * languages have no pack at all -- so their overlay text is compiled in. */
#include "lang_quick_cjk.inc"

enum { QL_ENGLISH = 0, QL_RUSSIAN, QL_PACK, QL_PAL, QL_JA, QL_ZH };

/* The script the language is written in, whether or not a face can draw it. */
static int QuickScriptWanted(void)
{
    if (Pc_RuActive())
        return LANG_QUICK_SCRIPT_LATIN;
    if (Pc_LangPackActive())
    {
        int font = Pc_LangPackFont();

        return (font == LANG_PACK_FONT_SJIS)    ? LANG_QUICK_SCRIPT_JAPANESE
             : (font == LANG_PACK_FONT_CHINESE) ? LANG_QUICK_SCRIPT_CHINESE
                                                : LANG_QUICK_SCRIPT_LATIN;
    }
    if (g_GameRegion == Region_JPN)
    {
        return (g_PcConfig.jpLanguage == JP_LANG_JAPANESE) ? LANG_QUICK_SCRIPT_JAPANESE
             : (g_PcConfig.jpLanguage == JP_LANG_CHINESE)  ? LANG_QUICK_SCRIPT_CHINESE
                                                           : LANG_QUICK_SCRIPT_LATIN;
    }
    return LANG_QUICK_SCRIPT_LATIN;
}

/* Oswald carries Latin and Cyrillic, but no font the port ships has a kana or
 * a hanzi, and a CJK face is far too large to ship for an overlay. So the
 * overlays borrow the system's: every Windows and macOS install has one, and
 * most Linux desktops do (Noto CJK or WenQuanYi, at the paths the major
 * distributions use). gamedata/font/cjk.* comes first, for a player whose
 * system has none or who prefers another. Chinese tries the Chinese faces
 * first, since a Japanese one lacks most simplified forms. stb_truetype 1.26
 * reads the CFF-based OpenType these ship as. */
#define CJK_USER_FACES \
    "gamedata/font/cjk.ttc", "gamedata/font/cjk.otf", "gamedata/font/cjk.ttf"
#define CJK_LINUX_FACES \
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",          /* Debian, Ubuntu */ \
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",               /* Arch */ \
    "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",        /* Fedora */ \
    "/usr/share/fonts/opentype/noto-cjk/NotoSansCJK-Regular.ttc", \
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc", \
    "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc", \
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", \
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf"
/* Android has carried a CJK face since its first release; which one depends on
 * the version (Noto CJK from 7.0, Droid fallback before). */
#define CJK_ANDROID_FACES \
    "/system/fonts/NotoSansCJK-Regular.ttc", \
    "/system/fonts/NotoSansSC-Regular.otf", \
    "/system/fonts/NotoSansJP-Regular.otf", \
    "/system/fonts/DroidSansFallbackFull.ttf", \
    "/system/fonts/DroidSansFallback.ttf"

static const char* const s_FacesJa[] = {
    CJK_USER_FACES,
    "C:/Windows/Fonts/YuGothM.ttc",
    "C:/Windows/Fonts/meiryo.ttc",
    "C:/Windows/Fonts/msgothic.ttc",
    "/System/Library/Fonts/\xE3\x83\x92\xE3\x83\xA9\xE3\x82\xAE\xE3\x83\x8E\xE8\xA7\x92\xE3\x82\xB4\xE3\x82\xB7\xE3\x83\x83\xE3\x82\xAF W3.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    CJK_LINUX_FACES,
    CJK_ANDROID_FACES,
};
static const char* const s_FacesZh[] = {
    CJK_USER_FACES,
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simsun.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
    CJK_LINUX_FACES,
    CJK_ANDROID_FACES,
};

/* The first face in the list that really draws the script, by asking it for a
 * glyph only that script has -- a file that exists but lacks them would turn
 * every label into empty boxes. Read once per script per run; NULL if none. */
static const char* CjkFace(int script)
{
    static const char* s_Found[3];
    static int         s_Probed[3];
    const char* const* list = (script == LANG_QUICK_SCRIPT_CHINESE) ? s_FacesZh : s_FacesJa;
    int                count = (script == LANG_QUICK_SCRIPT_CHINESE)
                                   ? (int)(sizeof(s_FacesZh) / sizeof(s_FacesZh[0]))
                                   : (int)(sizeof(s_FacesJa) / sizeof(s_FacesJa[0]));
    /* hiragana A; and the simplified form of "set" (U+8BBE), absent from Japanese faces */
    int                probe = (script == LANG_QUICK_SCRIPT_CHINESE) ? 0x8BBE : 0x3042;
    int                i;

    if (script != LANG_QUICK_SCRIPT_JAPANESE && script != LANG_QUICK_SCRIPT_CHINESE)
        return NULL;
    if (s_Probed[script])
        return s_Found[script];
    s_Probed[script] = 1;

    for (i = 0; i < count && s_Found[script] == NULL; i++)
    {
        FILE*          f = fopen(list[i], "rb");
        long           n;
        unsigned char* data;
        stbtt_fontinfo info;

        if (f == NULL)
            continue;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        data = (n > 0 && n < (128L << 20)) ? (unsigned char*)malloc((size_t)n) : NULL;
        if (data != NULL && fread(data, 1, (size_t)n, f) == (size_t)n &&
            stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0)) &&
            stbtt_FindGlyphIndex(&info, probe) != 0)
        {
            s_Found[script] = list[i];
        }
        free(data);
        fclose(f);
    }

    if (s_Found[script] != NULL)
        SH_LOG("[LANG] overlay font for %s: %s",
               script == LANG_QUICK_SCRIPT_CHINESE ? "Chinese" : "Japanese", s_Found[script]);
    else
        SH_WARN("[LANG] no %s font found on this system (or gamedata/font/cjk.ttc) - "
                "the quick menu and its panels stay in English",
                script == LANG_QUICK_SCRIPT_CHINESE ? "Chinese" : "Japanese");
    return s_Found[script];
}

/* The script the overlays will actually draw: a CJK language without a face
 * that can show it is drawn in English, never as boxes. */
int Pc_LangQuickScript(void)
{
    int script = QuickScriptWanted();

    return (script != LANG_QUICK_SCRIPT_LATIN && CjkFace(script) == NULL)
               ? LANG_QUICK_SCRIPT_LATIN
               : script;
}

int Pc_LangQuickFontPaths(const char** out, int max)
{
    static const char* const latin[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    const char* cjk = CjkFace(Pc_LangQuickScript());
    int         n   = 0;
    int         i;

    if (cjk != NULL && n < max)
        out[n++] = cjk;
    for (i = 0; i < (int)(sizeof(latin) / sizeof(latin[0])) && n < max; i++)
        out[n++] = latin[i];
    return n;
}

static const char* CjkLookup(const s_QuickCjk* tbl, int count, const char* key, int zh)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (tbl[i].en[0] == key[0] && strcmp(tbl[i].en, key) == 0)
            return zh ? tbl[i].zh : tbl[i].ja;
    }
    return NULL;
}

/* Mirrors Pc_LangMenuText's order, so an overlay never disagrees with the
 * menu drawn behind it. */
static int QuickLang(void)
{
    int lang = g_PcConfig.language;

    switch (QuickScriptWanted())
    {
        /* No face for it: English, not the pack -- a Japanese or Chinese pack
         * holds game-font bytes, not text this overlay could fall back to. */
        case LANG_QUICK_SCRIPT_JAPANESE:
            return (Pc_LangQuickScript() == LANG_QUICK_SCRIPT_JAPANESE) ? QL_JA : QL_ENGLISH;
        case LANG_QUICK_SCRIPT_CHINESE:
            return (Pc_LangQuickScript() == LANG_QUICK_SCRIPT_CHINESE) ? QL_ZH : QL_ENGLISH;
        default:
            break;
    }
    if (Pc_RuActive())
        return QL_RUSSIAN;
    if (Pc_LangPackActive())
        return QL_PACK;
    if (g_GameRegion == Region_JPN)
        return QL_ENGLISH;
    if ((g_GameRegion == Region_EUR || (g_GameRegion == Region_USA && Pc_FanTextActive())) &&
        lang >= 1 && lang <= 4)
    {
        return QL_PAL;
    }
    return QL_ENGLISH;
}

#define RING_COUNT 8
#define RING_SIZE  512

static char* RingNext(void)
{
    static char s_Ring[RING_COUNT][RING_SIZE];
    static int  s_Slot;

    s_Slot = (s_Slot + 1) % RING_COUNT;
    return s_Ring[s_Slot];
}

unsigned int Pc_LangUtf8Next(const char** pp)
{
    const unsigned char* p = (const unsigned char*)*pp;
    unsigned int         c = *p++;

    if (c >= 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    {
        c = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    }
    else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80)
    {
        c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    }
    else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80)
    {
        c = ((c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    }

    *pp = (const char*)p;
    return c;
}

static int Utf8Put(unsigned int cp, char* out, int room)
{
    if (cp < 0x80 && room >= 1)
    {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800 && room >= 2)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room >= 3)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

/* Menu-dialect text -> what the overlay draws: '_' is the menu font's space,
 * and the PAL table is Latin-1. */
static const char* FromMenu(const char* s, int latin1)
{
    char* out = RingNext();
    int   n   = 0;

    while (*s != '\0' && n < RING_SIZE - 4)
    {
        unsigned int cp;

        if (*s == '_')
        {
            out[n++] = ' ';
            s++;
            continue;
        }
        if (*s == '\x01')
        {
            s++;
            continue;
        }
        if (latin1)
            cp = (unsigned char)*s++;
        else
            cp = Pc_LangUtf8Next(&s);
        n += Utf8Put(cp, out + n, RING_SIZE - 1 - n);
    }
    out[n] = '\0';
    return out;
}

const char* Pc_LangQuickMenu(const char* us)
{
    const char* tr = NULL;
    int         latin1 = 0;

    if (us == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_RUSSIAN: tr = Pc_RuMenuUtf8(us); break;
        case QL_PACK:
            tr = Pc_LangPackMenuUtf8(us);
            if (tr == NULL)
            {
                tr     = Pc_LangMenuPal(us, -1); /* a de/fr/es/it pack's column */
                latin1 = (tr != NULL);
            }
            break;
        case QL_PAL:     tr = Pc_LangMenuPal(us, -1); latin1 = 1; break;
        case QL_JA:
        case QL_ZH:
        {
            const char* cjk = CjkLookup(s_MenuCjkTr, (int)(sizeof(s_MenuCjkTr) / sizeof(s_MenuCjkTr[0])),
                                        us, QuickLang() == QL_ZH);
            if (cjk != NULL)
                return cjk;
            break;
        }
        default:         break;
    }
    return FromMenu(tr ? tr : us, tr ? latin1 : 0);
}

/* The compiled de/fr/es/it overlay text, by column rather than by the retail
 * language number, so a pack of the same name reads it too (Pc_LangPalColumn).
 * The pack carries the story and the items; this text was written for the PAL
 * disc and is the same language, so there is nothing to duplicate. */
static const char* QuickPal(const char* en)
{
    int col = Pc_LangPalColumn();
    int i;

    if (col < 0)
        return NULL;

    for (i = 0; i < (int)(sizeof(s_QuickTr) / sizeof(s_QuickTr[0])); i++)
    {
        if (s_QuickTr[i].en[0] == en[0] && strcmp(s_QuickTr[i].en, en) == 0)
            return s_QuickTr[i].tr[col];
    }
    return NULL;
}

const char* Pc_LangQuick(const char* en)
{
    int i;

    if (en == NULL)
        return "";

    switch (QuickLang())
    {
        case QL_RUSSIAN:
        {
            /* These TrueType-dialog strings have no disc-font equivalent. */
            static const struct { const char* en; const char* ru; } text[] = {
                { "RESET SETTINGS", "СБРОС НАСТРОЕК" },
                { "Reset all PC options to their defaults?", "Вернуть все настройки ПК к значениям по умолчанию?" },
                { "Yes", "Да" }, { "No", "Нет" },
                { "Left/Right select", "Влево/вправо: выбор" },
                { "[OK] confirm", "[OK] подтвердить" },
                { "[Cancel] back", "[Отмена] назад" }
            };
            for (i = 0; i < (int)(sizeof(text) / sizeof(text[0])); i++)
                if (strcmp(en, text[i].en) == 0) return text[i].ru;
            return en;
        }
        case QL_PACK:
        {
            char key[160];
            int  n = snprintf(key, sizeof(key), "QUICK.");
            const char* tr;

            for (i = 0; en[i] != '\0' && n < (int)sizeof(key) - 1; i++)
                key[n++] = (en[i] == ' ') ? '_' : (en[i] == '=') ? '-' : en[i];
            key[n] = '\0';
            tr = Pc_LangPackUtf8(key);
            if (tr != NULL && tr[0] != '\0')
                return tr;
            /* Not in the pack: a de/fr/es/it pack still has a column. */
            tr = QuickPal(en);
            return (tr != NULL) ? tr : en;
        }
        case QL_PAL:
        {
            const char* tr = QuickPal(en);

            return (tr != NULL) ? tr : en;
        }
        case QL_JA:
        case QL_ZH:
        {
            const char* tr = CjkLookup(s_QuickCjkTr, (int)(sizeof(s_QuickCjkTr) / sizeof(s_QuickCjkTr[0])),
                                       en, QuickLang() == QL_ZH);

            return (tr != NULL) ? tr : en;
        }
        default:
            return en;
    }
}

const char* Pc_LangQuickFill(const char* en, const char* placeholder, const char* value)
{
    const char* tmpl = Pc_LangQuick(en);
    const char* at   = strstr(tmpl, placeholder);
    char*       out  = RingNext();
    size_t      phLen = strlen(placeholder);

    if (at == NULL)
    {
        tmpl = en;
        at   = strstr(tmpl, placeholder);
        if (at == NULL)
        {
            snprintf(out, RING_SIZE, "%s", en);
            return out;
        }
    }
    snprintf(out, RING_SIZE, "%.*s%s%s", (int)(at - tmpl), tmpl, value ? value : "", at + phLen);
    return out;
}

static unsigned int UpperCp(unsigned int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 0x20;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7)
        return c - 0x20;
    /* Latin Extended-A pairs the cases in two parities around U+0138/U+0149. */
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : c - 1;
    if (c >= 0x430 && c <= 0x44F)
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45F)
        return c - 0x50;
    return c;
}

void Pc_LangUtf8Upper(const char* in, char* out, int outSize)
{
    int n = 0;

    if (outSize <= 0)
        return;
    while (in != NULL && *in != '\0' && n < outSize - 4)
        n += Utf8Put(UpperCp(Pc_LangUtf8Next(&in)), out + n, outSize - 1 - n);
    out[n] = '\0';
}
