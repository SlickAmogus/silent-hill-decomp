/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Additional Characters core: catalog, local scan, downloads. See pc_chara_dl.h. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include <SDL.h>

#include "miniz.h"
#include "pc_chara_dl.h"
#include "lang_quick.h"
#include "pc_config.h"
#include "sh_log.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#elif defined(__ANDROID__) || defined(SH_IOS)
#include "pc_qol_mods.h"
#define CD_HTTP_FILE 1
#else
#include "pc_ra_http.h"
#endif

#define CD_RELEASE_BASE "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/"
#define CD_CATALOG_FILE "characters.json"
/* The web build reaches the release through the same relay the QoL list uses
 * (GitHub sends no CORS headers). It forwards any plain asset name. */
#define CD_WEB_RELAY    "https://us-central1-sh1pc-8d53f.cloudfunctions.net/qolMods/"
#define CD_TMP_DIR      "gamedata/.chr"
#define CD_PREVIEW_DIR  "gamedata/.chr_preview"
#define CD_CHARA_DIR    "gamedata/load/CHARA"
#define CD_OFF_DIR      "gamedata/load/CHARA_off"
#define CD_RECORDS      "gamedata/chara_downloads.txt"
#define CD_TEST_DIR     "gamedata/chara_catalog_test"

#define CD_MAX_ENTRIES 1024
#define CD_MAX_LOCAL   1024
#define CD_MAX_GROUPS  256
/* Above the catalog size: ticking every series queues one job per character. */
#define CD_MAX_JOBS    (CD_MAX_ENTRIES + 64)

/* ------------------------------------------------------------------------ */
/* small file helpers                                                        */

static void cd_mkdir(const char* p)
{
#if defined(_WIN32)
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

static int cd_exists(const char* p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int cd_is_dir(const char* p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR);
}

static int cd_copy(const char* from, const char* to)
{
    FILE* a = fopen(from, "rb");
    FILE* b;
    char  buf[64 * 1024];
    size_t n;
    int   ok = 1;

    if (a == NULL)
        return 0;
    b = fopen(to, "wb");
    if (b == NULL)
    {
        fclose(a);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, n, b) != n)
            ok = 0;
    fclose(a);
    fclose(b);
    return ok;
}

/* Copy, then delete -- never rename(). PsyCross implements the PSX memory
 * card API under libc's names in this same image (libapi.c), and its rename()
 * answers 0, libc's "success", for any path not on a card while moving
 * nothing. remove() and fopen() are libc's own. */
static int cd_move(const char* from, const char* to)
{
    remove(to);
    if (!cd_copy(from, to))
    {
        remove(to);
        return 0;
    }
    remove(from);
    return 1;
}

static int cd_ieq(const char* a, const char* b)
{
    return SDL_strcasecmp(a, b) == 0;
}

static void cd_trim(char* s)
{
    char* e;
    char* b = s;

    while (*b == ' ' || *b == '\t')
        b++;
    if (b != s)
        memmove(s, b, strlen(b) + 1);
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
}

/* HERO_<x>.glb -> HERO_<x>; 0 for anything that is not a selectable swap. */
static int cd_hero_base(const char* fileName, char* out, int outSz)
{
    const char* dot = strrchr(fileName, '.');
    int         n;

    if (dot == NULL || (!cd_ieq(dot, ".glb") && !cd_ieq(dot, ".gltf")))
        return 0;
    n = (int)(dot - fileName);
    if (n <= 4 || n >= outSz || SDL_strncasecmp(fileName, "HERO", 4) != 0)
        return 0;
    memcpy(out, fileName, (size_t)n);
    out[n] = '\0';
    return 1;
}

/* ------------------------------------------------------------------------ */
/* ini metadata                                                              */

static void cd_ini_meta_path(const char* path, char* name, int nameSz, char* category, int catSz,
                             char* author, int authSz, char* version, int verSz)
{
    FILE* f;
    char  line[256];

    if (name)     name[0]     = '\0';
    if (category) category[0] = '\0';
    if (author)   author[0]   = '\0';
    if (version)  version[0]  = '\0';
    f = fopen(path, "r");
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char* eq = strchr(line, '=');
        char  key[32];
        char* val;

        if (line[0] == '#' || line[0] == ';' || eq == NULL)
            continue;
        *eq = '\0';
        snprintf(key, sizeof(key), "%s", line);
        cd_trim(key);
        val = eq + 1;
        cd_trim(val);
        if (name && cd_ieq(key, "name"))
            snprintf(name, (size_t)nameSz, "%s", val);
        else if (category && cd_ieq(key, "category"))
            snprintf(category, (size_t)catSz, "%s", val);
        else if (author && cd_ieq(key, "author"))
            snprintf(author, (size_t)authSz, "%s", val);
        else if (version && cd_ieq(key, "version"))
            snprintf(version, (size_t)verSz, "%s", val);
    }
    fclose(f);
}

void Cd_IniMeta(const char* hero, char* name, int nameSz, char* category, int catSz,
                char* author, int authSz, char* version, int verSz)
{
    char path[256];

    snprintf(path, sizeof(path), CD_CHARA_DIR "/%s", hero);
    {
        char rb[256];

        snprintf(path, sizeof(path), "%s.ini", Cd_ResolveBase(path, rb, (int)sizeof(rb)));
    }
    cd_ini_meta_path(path, name, nameSz, category, catSz, author, authSz, version, verSz);
}

static void cd_name_from_file(const char* hero, char* out, int outSz)
{
    const char* n = hero + 4;

    if (*n == '_' || *n == '-' || *n == ' ')
        n++;
    snprintf(out, (size_t)outSz, "%s", *n ? n : hero);
}

void Cd_DisplayName(const char* hero, char* out, int outSz)
{
    Cd_IniMeta(hero, out, outSz, NULL, 0, NULL, 0, NULL, 0);
    if (out[0] == '\0')
        cd_name_from_file(hero, out, outSz);
}

/* ------------------------------------------------------------------------ */
/* tiny JSON: flat objects of strings, numbers and booleans                   */

/* Reads one JSON string at *pp (just past its opening quote) into out. */
static const char* cd_json_str(const char* p, char* out, int outSz)
{
    int n = 0;

    while (*p && *p != '"')
    {
        char c = *p++;

        if (c == '\\' && *p)
        {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u')
            {
                /* Only ASCII survives the native font anyway. */
                unsigned v = 0;
                int      k;
                for (k = 0; k < 4 && isxdigit((unsigned char)*p); k++, p++)
                    v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                c = (v < 128) ? (char)v : '?';
            }
        }
        if (n < outSz - 1)
            out[n++] = c;
    }
    out[n] = '\0';
    return *p == '"' ? p + 1 : p;
}

typedef void (*CdJsonField)(void* ctx, const char* key, const char* val);

/* Walks one {...} at p (pointing at '{'), calling fn for each scalar member.
 * Nested arrays/objects are skipped whole. Returns the char after '}'. */
static const char* cd_json_object(const char* p, CdJsonField fn, void* ctx)
{
    char key[48], val[1024];

    if (*p != '{')
        return p;
    p++;
    for (;;)
    {
        while (*p && *p != '"' && *p != '}')
            p++;
        if (*p != '"')
            break;
        p = cd_json_str(p + 1, key, sizeof(key));
        while (*p && *p != ':')
            p++;
        if (*p)
            p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (*p == '"')
            p = cd_json_str(p + 1, val, sizeof(val));
        else if (*p == '{' || *p == '[')
        {
            int  depth = 0;
            char open  = *p, close = (*p == '{') ? '}' : ']';
            do
            {
                if (*p == '"')
                {
                    p = cd_json_str(p + 1, val, sizeof(val));
                    continue;
                }
                if (*p == open) depth++;
                else if (*p == close) depth--;
                p++;
            } while (*p && depth > 0);
            continue;
        }
        else
        {
            int n = 0;
            while (*p && *p != ',' && *p != '}' && n < (int)sizeof(val) - 1)
                val[n++] = *p++;
            val[n] = '\0';
            cd_trim(val);
        }
        fn(ctx, key, val);
    }
    return *p == '}' ? p + 1 : p;
}

static char* cd_read_all(const char* path, long long maxBytes)
{
    FILE*     f = fopen(path, "rb");
    long long sz;
    char*     buf;
    size_t    got;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > maxBytes || (buf = (char*)malloc((size_t)sz + 1)) == NULL)
    {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* ------------------------------------------------------------------------ */
/* catalog                                                                   */

static CdEntry s_cat[CD_MAX_ENTRIES];
static int     s_catCount;
static int     s_catState;

static void cd_cat_field(void* ctx, const char* k, const char* v)
{
    CdEntry* e = (CdEntry*)ctx;

    if (strcmp(k, "id") == 0)            snprintf(e->id, sizeof(e->id), "%s", v);
    else if (strcmp(k, "name") == 0)     snprintf(e->name, sizeof(e->name), "%s", v);
    else if (strcmp(k, "category") == 0) snprintf(e->category, sizeof(e->category), "%s", v);
    else if (strcmp(k, "author") == 0)   snprintf(e->author, sizeof(e->author), "%s", v);
    else if (strcmp(k, "version") == 0)  snprintf(e->version, sizeof(e->version), "%s", v);
    else if (strcmp(k, "file") == 0)     snprintf(e->file, sizeof(e->file), "%s", v);
    else if (strcmp(k, "hero") == 0)     snprintf(e->hero, sizeof(e->hero), "%s", v);
    else if (strcmp(k, "size") == 0)     e->size = atoll(v);
    else if (strcmp(k, "sha256") == 0)   snprintf(e->sha256, sizeof(e->sha256), "%s", v);
    else if (strcmp(k, "glb_size") == 0) e->glbSize = atoll(v);
    else if (strcmp(k, "glb_crc32") == 0) e->glbCrc = (unsigned)strtoul(v, NULL, 16);
}

/* Off the net: anything that could escape the folders is dropped. */
static int cd_safe_name(const char* s)
{
    return s[0] != '\0' && strchr(s, '/') == NULL && strchr(s, '\\') == NULL && strstr(s, "..") == NULL;
}

static int cd_parse_catalog(const char* json)
{
    const char* p = strstr(json, "\"characters\"");

    s_catCount = 0;
    if (p == NULL || (p = strchr(p, '[')) == NULL)
        return 0;
    p++;
    while (*p && s_catCount < CD_MAX_ENTRIES)
    {
        CdEntry e;

        while (*p && *p != '{' && *p != ']')
            p++;
        if (*p != '{')
            break;
        memset(&e, 0, sizeof(e));
        p = cd_json_object(p, cd_cat_field, &e);
        if (e.id[0] && cd_safe_name(e.file) && cd_safe_name(e.hero) && SDL_strncasecmp(e.hero, "HERO", 4) == 0)
        {
            if (e.name[0] == '\0')
                cd_name_from_file(e.hero, e.name, sizeof(e.name));
            if (e.category[0] == '\0')
                snprintf(e.category, sizeof(e.category), "Other");
            s_cat[s_catCount++] = e;
        }
    }
    return s_catCount;
}

int Cd_CatalogState(void) { return s_catState; }
int Cd_CatalogCount(void) { return s_catCount; }
const CdEntry* Cd_CatalogGet(int i) { return (i >= 0 && i < s_catCount) ? &s_cat[i] : NULL; }

/* ------------------------------------------------------------------------ */
/* download records                                                          */

typedef struct
{
    char id[CD_ID_MAX];
    char version[16];
    char sha256[65];
    char hero[CD_FILE_MAX];
} CdRecord;

static CdRecord s_rec[CD_MAX_ENTRIES];
static int      s_recCount;

static void cd_records_load(void)
{
    FILE* f = fopen(CD_RECORDS, "r");
    char  line[512];

    s_recCount = 0;
    if (f == NULL)
        return;
    while (fgets(line, sizeof(line), f) != NULL && s_recCount < CD_MAX_ENTRIES)
    {
        CdRecord r;
        char*    t[4];
        int      n = 0;
        char*    p = line;

        cd_trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        while (n < 4)
        {
            t[n++] = p;
            p = strchr(p, '\t');
            if (p == NULL)
                break;
            *p++ = '\0';
        }
        if (n < 4)
            continue;
        memset(&r, 0, sizeof(r));
        snprintf(r.id, sizeof(r.id), "%s", t[0]);
        snprintf(r.version, sizeof(r.version), "%s", t[1]);
        snprintf(r.sha256, sizeof(r.sha256), "%s", t[2]);
        snprintf(r.hero, sizeof(r.hero), "%s", t[3]);
        s_rec[s_recCount++] = r;
    }
    fclose(f);
}

static void cd_records_save(void)
{
    FILE* f = fopen(CD_RECORDS, "w");
    int   i;

    if (f == NULL)
        return;
    fprintf(f, "# id\tversion\tsha256\thero  (written by the game and the launcher)\n");
    for (i = 0; i < s_recCount; i++)
        fprintf(f, "%s\t%s\t%s\t%s\n", s_rec[i].id, s_rec[i].version, s_rec[i].sha256, s_rec[i].hero);
    fclose(f);
}

static CdRecord* cd_record_by_id(const char* id)
{
    int i;

    for (i = 0; i < s_recCount; i++)
        if (strcmp(s_rec[i].id, id) == 0)
            return &s_rec[i];
    return NULL;
}

static CdRecord* cd_record_by_hero(const char* hero)
{
    int i;

    for (i = 0; i < s_recCount; i++)
        if (cd_ieq(s_rec[i].hero, hero))
            return &s_rec[i];
    return NULL;
}

static void cd_record_drop(const char* id)
{
    int i;

    for (i = 0; i < s_recCount; i++)
        if (strcmp(s_rec[i].id, id) == 0)
        {
            memmove(&s_rec[i], &s_rec[i + 1], (size_t)(s_recCount - i - 1) * sizeof(s_rec[0]));
            s_recCount--;
            return;
        }
}

int Cd_IsDownloaded(const CdEntry* e)
{
    return e != NULL && cd_record_by_id(e->id) != NULL;
}

int Cd_NeedsUpdate(const CdEntry* e)
{
    CdRecord* r = e ? cd_record_by_id(e->id) : NULL;

    return r != NULL && (strcmp(r->version, e->version) != 0 ||
                         (e->sha256[0] && r->sha256[0] && strcmp(r->sha256, e->sha256) != 0));
}

/* ------------------------------------------------------------------------ */
/* local scan                                                                */

static CdLocal s_local[CD_MAX_LOCAL];
static int     s_localCount;
static CdGroup s_groups[CD_MAX_GROUPS];
static int     s_groupCount;

int            Cd_LocalCount(void) { return s_localCount; }
const CdLocal* Cd_LocalGet(int i) { return (i >= 0 && i < s_localCount) ? &s_local[i] : NULL; }
int            Cd_GroupCount(void) { return s_groupCount; }
const CdGroup* Cd_GroupGet(int i) { return (i >= 0 && i < s_groupCount) ? &s_groups[i] : NULL; }

int Cd_LocalFind(const char* hero)
{
    int i;

    for (i = 0; i < s_localCount; i++)
        if (cd_ieq(s_local[i].hero, hero))
            return i;
    return -1;
}

int Cd_CustomCopyWins(const CdEntry* e)
{
    int i = e != NULL ? Cd_LocalFind(e->hero) : -1;

    return i >= 0 && s_local[i].source != CD_SRC_DOWNLOAD && s_local[i].enabled && !s_local[i].oldJojo;
}

static int cd_group_add(const char* name, const char* key, const char* author, int source, int enabled)
{
    int i;

    for (i = 0; i < s_groupCount; i++)
        if (s_groups[i].source == source && strcmp(s_groups[i].key, key) == 0)
            return i;
    if (s_groupCount >= CD_MAX_GROUPS)
        return -1;
    memset(&s_groups[s_groupCount], 0, sizeof(s_groups[0]));
    snprintf(s_groups[s_groupCount].name, sizeof(s_groups[0].name), "%s", name);
    snprintf(s_groups[s_groupCount].key, sizeof(s_groups[0].key), "%s", key);
    snprintf(s_groups[s_groupCount].author, sizeof(s_groups[0].author), "%s", author ? author : "");
    s_groups[s_groupCount].source  = source;
    s_groups[s_groupCount].enabled = enabled;
    return s_groupCount++;
}

static CdLocal* cd_local_add(const char* hero, const char* dir, int source, int group, int enabled)
{
    CdLocal* l;
    char     path[320];

    if (s_localCount >= CD_MAX_LOCAL || Cd_LocalFind(hero) >= 0)
        return NULL;
    l = &s_local[s_localCount++];
    memset(l, 0, sizeof(*l));
    snprintf(l->hero, sizeof(l->hero), "%s", hero);
    snprintf(path, sizeof(path), "%s/%s.ini", dir, hero);
    cd_ini_meta_path(path, l->name, sizeof(l->name), l->category, sizeof(l->category), l->author,
                     sizeof(l->author), l->version, sizeof(l->version));
    if (l->name[0] == '\0')
        cd_name_from_file(hero, l->name, sizeof(l->name));
    if (l->author[0] == '\0' && group >= 0)
        snprintf(l->author, sizeof(l->author), "%s", s_groups[group].author);
    l->source  = source;
    l->group   = group;
    l->enabled = enabled;
    return l;
}

/* The heroes a folder holds, into names[] (bases). */
static int cd_list_heroes(const char* dir, char names[][CD_FILE_MAX], int max)
{
    DIR*           d = opendir(dir);
    struct dirent* e;
    int            n = 0;

    if (d == NULL)
        return 0;
    while ((e = readdir(d)) != NULL && n < max)
        if (cd_hero_base(e->d_name, names[n], CD_FILE_MAX))
            n++;
    closedir(d);
    return n;
}

/* Legacy QoL character packs (ports): ids in qol_installed.txt and the files
 * each put down, as the catalog listed them when they shipped. */
typedef struct
{
    const char* id;
    const char* name;
    const char* author;
    const char* heroes[6];
} CdLegacyPack;

static const CdLegacyPack s_legacy[] = {
    { "character-pack-silent-hill", "Silent Hill Pack", "Konami, Bloober Team, Agnaro, tykr",
      { "HERO_Harry-JamesReskin", "HERO_HarrySM", "HERO_Heather", "HERO_JamesClassic", "HERO_JamesRemake", NULL } },
    { "character-pack-resident-evil", "Resident Evil Pack", "CAPCOM",
      { "HERO_ChrisRedfield", "HERO_Hunk", "HERO_JillValentine", "HERO_Rebecca", "HERO_Wesker", NULL } },
    { "character-lynette", "Lynette", "Bandai Namco", { "HERO_Lynette", NULL } },
    { "character-cj", "CJ", "Rockstar Games", { "HERO_CJ", NULL } },
    { "character-master-chief", "Master Chief", "Microsoft, Vuthakral", { "HERO_MasterChief", NULL } },
};
#define CD_LEGACY_COUNT ((int)(sizeof(s_legacy) / sizeof(s_legacy[0])))

static int cd_qol_installed(const char* id)
{
    FILE* f = fopen("gamedata/qol_installed.txt", "r");
    char  line[256];
    int   found = 0;

    if (f == NULL)
        return 0;
    while (!found && fgets(line, sizeof(line), f) != NULL)
    {
        char* sp = strchr(line, ' ');

        if (sp)
            *sp = '\0';
        cd_trim(line);
        found = strcmp(line, id) == 0;
    }
    fclose(f);
    return found;
}

#if !defined(__ANDROID__) && !defined(SH_IOS) && !defined(__EMSCRIPTEN__)
#define CD_HAS_LAUNCHER_MODS 1

/* The launcher's mods/modmanager.json: Name (folder), Enabled, DisplayName,
 * Author. Written by DataContractJsonSerializer, so flat objects. */
typedef struct
{
    char name[CD_NAME_MAX];
    char display[CD_NAME_MAX];
    char author[96];
    char essential[64];
    int  enabled;
} CdMod;

static void cd_mod_field(void* ctx, const char* k, const char* v)
{
    CdMod* m = (CdMod*)ctx;

    if (strcmp(k, "Name") == 0)             snprintf(m->name, sizeof(m->name), "%s", v);
    else if (strcmp(k, "DisplayName") == 0) snprintf(m->display, sizeof(m->display), "%s", v);
    else if (strcmp(k, "Author") == 0)      snprintf(m->author, sizeof(m->author), "%s", v);
    else if (strcmp(k, "EssentialId") == 0) snprintf(m->essential, sizeof(m->essential), "%s", v);
    else if (strcmp(k, "Enabled") == 0)     m->enabled = strcmp(v, "true") == 0;
}

/* Where a mod keeps its load/CHARA: the root, or one folder down (as the
 * launcher looks for modinfo.txt). */
static int cd_mod_chara_dir(const char* mod, char* out, int outSz)
{
    DIR*           d;
    struct dirent* e;
    char           top[300];

    snprintf(out, (size_t)outSz, "mods/%s/load/CHARA", mod);
    if (cd_is_dir(out))
        return 1;
    snprintf(top, sizeof(top), "mods/%s", mod);
    if ((d = opendir(top)) == NULL)
        return 0;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        snprintf(out, (size_t)outSz, "mods/%s/%s/load/CHARA", mod, e->d_name);
        if (cd_is_dir(out))
        {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

static int cd_mod_modinfo(const char* mod, const char* charaDir, char* id, int idSz, char* author,
                          int authSz, int* hasVersion)
{
    char  path[400];
    FILE* f;
    char  line[256];
    char* cut;

    /* modinfo.txt sits beside the load/ folder. */
    snprintf(path, sizeof(path), "%s", charaDir);
    cut = strstr(path, "/load/CHARA");
    if (cut)
        snprintf(cut, sizeof(path) - (size_t)(cut - path), "/modinfo.txt");
    id[0] = author[0] = '\0';
    *hasVersion = 0;
    if ((f = fopen(path, "r")) == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char* eq = strchr(line, '=');
        char  key[32];

        if (eq == NULL || line[0] == '#')
            continue;
        *eq = '\0';
        snprintf(key, sizeof(key), "%s", line);
        cd_trim(key);
        cd_trim(eq + 1);
        if (cd_ieq(key, "id"))           snprintf(id, (size_t)idSz, "%s", eq + 1);
        else if (cd_ieq(key, "author"))  snprintf(author, (size_t)authSz, "%s", eq + 1);
        else if (cd_ieq(key, "version")) *hasVersion = (eq[1] != '\0');
    }
    fclose(f);
    (void)mod;
    return 1;
}

static void cd_scan_launcher_mods(void)
{
    char*       json = cd_read_all("mods/modmanager.json", 4 * 1024 * 1024);
    const char* p;

    if (json == NULL)
        return;
    p = strstr(json, "\"Mods\"");
    if (p && (p = strchr(p, '[')) != NULL)
    {
        p++;
        while (*p)
        {
            CdMod m;
            static char heroes[CD_MAX_LOCAL][CD_FILE_MAX];
            char  dir[300], id[64], author[96];
            int   n, i, g, hasVer;

            while (*p && *p != '{' && *p != ']')
                p++;
            if (*p != '{')
                break;
            memset(&m, 0, sizeof(m));
            p = cd_json_object(p, cd_mod_field, &m);
            if (!m.name[0] || !cd_mod_chara_dir(m.name, dir, sizeof(dir)))
                continue;
            n = cd_list_heroes(dir, heroes, CD_MAX_LOCAL);
            if (n == 0)
                continue;
            cd_mod_modinfo(m.name, dir, id, sizeof(id), author, sizeof(author), &hasVer);
            g = cd_group_add(m.display[0] ? m.display : m.name, m.name,
                             m.author[0] ? m.author : author, CD_SRC_PACK, m.enabled);
            if (g < 0)
                continue;
            s_groups[g].oldJojo = cd_ieq(id, "additionalcharacters") && SDL_strcasestr(author, "jojo670") && !hasVer;
            for (i = 0; i < n; i++)
            {
                char path[320];

                /* The old pack gives way to a download of the same name, as in
                 * the launcher; every other mod wins over one. */
                if (s_groups[g].oldJojo && cd_record_by_hero(heroes[i]))
                    continue;
                snprintf(path, sizeof(path), CD_CHARA_DIR "/%s.glb", heroes[i]);
                {
                    CdLocal* l = cd_local_add(heroes[i], m.enabled && cd_exists(path) ? CD_CHARA_DIR : dir,
                                              CD_SRC_PACK, g, m.enabled);

                    if (l != NULL)
                        l->oldJojo = s_groups[g].oldJojo;
                }
            }
        }
    }
    free(json);
}

/* Flip one mod's "Enabled" in mods/modmanager.json, so the launcher's next
 * Apply agrees with what the game did. */
static void cd_launcher_set_enabled(const char* modName, int enabled)
{
    char*  json = cd_read_all("mods/modmanager.json", 4 * 1024 * 1024);
    char   needle[160];
    char*  hit;
    char*  start;
    char*  end;
    FILE*  f;

    if (json == NULL)
        return;
    snprintf(needle, sizeof(needle), "\"Name\":\"%s\"", modName);
    hit = strstr(json, needle);
    if (hit != NULL)
    {
        start = hit;
        while (start > json && *start != '{')
            start--;
        end = strchr(hit, '}');
        if (end != NULL)
        {
            char  saved = *end;
            char* en;

            *end = '\0';
            en = strstr(start, enabled ? "\"Enabled\":false" : "\"Enabled\":true");
            *end = saved;
            if (en != NULL)
            {
                const char* rep = enabled ? "\"Enabled\":true" : "\"Enabled\":false";
                size_t      oldLen = strlen(enabled ? "\"Enabled\":false" : "\"Enabled\":true");
                size_t      repLen = strlen(rep);
                size_t      tail = strlen(en + oldLen);
                char*       out = (char*)malloc(strlen(json) + 8);

                if (out != NULL)
                {
                    size_t head = (size_t)(en - json);

                    memcpy(out, json, head);
                    memcpy(out + head, rep, repLen);
                    memcpy(out + head + repLen, en + oldLen, tail + 1);
                    if ((f = fopen("mods/modmanager.json", "wb")) != NULL)
                    {
                        fwrite(out, 1, strlen(out), f);
                        fclose(f);
                    }
                    free(out);
                }
            }
        }
    }
    free(json);
}
#endif

static void cd_migrate_downloads(void);

const char* Cd_ResolveBase(const char* base, char* out, int outSz)
{
    const char* at = base ? strstr(base, "/load/CHARA/") : NULL;
    char        p[400];

    if (at == NULL)
        return base;
    snprintf(p, sizeof(p), "%s.glb", base);
    if (cd_exists(p))
        return base;
    snprintf(p, sizeof(p), "%s.gltf", base);
    if (cd_exists(p))
        return base;
    snprintf(out, (size_t)outSz, "%.*s/load/CHARA_dl/%s", (int)(at - base), base, at + 12);
    return out;
}

void Cd_LocalScan(void)
{
    static char heroes[CD_MAX_LOCAL][CD_FILE_MAX];
    int  n, i, j;

    s_localCount = 0;
    s_groupCount = 0;
    cd_records_load();

    cd_migrate_downloads();

#if defined(CD_HAS_LAUNCHER_MODS)
    cd_scan_launcher_mods();
#endif

    /* QoL packs from before the catalog (the ports' list installed them). */
    for (i = 0; i < CD_LEGACY_COUNT; i++)
    {
        int g = -1;

        if (!cd_qol_installed(s_legacy[i].id))
            continue;
        for (j = 0; s_legacy[i].heroes[j] != NULL; j++)
        {
            char on[320], off[320];
            int  enabled;

            snprintf(on, sizeof(on), CD_CHARA_DIR "/%s.glb", s_legacy[i].heroes[j]);
            snprintf(off, sizeof(off), CD_OFF_DIR "/%s/%s.glb", s_legacy[i].id, s_legacy[i].heroes[j]);
            enabled = cd_exists(on);
            if (!enabled && !cd_exists(off))
                continue;
            if (Cd_LocalFind(s_legacy[i].heroes[j]) >= 0)
                continue;
            if (g < 0)
                g = cd_group_add(s_legacy[i].name, s_legacy[i].id, s_legacy[i].author, CD_SRC_PACK, enabled);
            if (g < 0)
                break;
            if (enabled)
                s_groups[g].enabled = 1;
            if (enabled)
                cd_local_add(s_legacy[i].heroes[j], CD_CHARA_DIR, CD_SRC_PACK, g, 1);
            else
            {
                char dir[200];
                snprintf(dir, sizeof(dir), CD_OFF_DIR "/%s", s_legacy[i].id);
                cd_local_add(s_legacy[i].heroes[j], dir, CD_SRC_PACK, g, 0);
            }
        }
    }

    /* Everything else in the folder: copied in by hand. */
    n = cd_list_heroes(CD_CHARA_DIR, heroes, CD_MAX_LOCAL);
    {
        int g = -1;

        for (i = 0; i < n; i++)
        {
            if (Cd_LocalFind(heroes[i]) >= 0)
                continue;
            if (g < 0)
                g = cd_group_add("Manually installed", "loose", "", CD_SRC_LOOSE, 1);
            cd_local_add(heroes[i], CD_CHARA_DIR, CD_SRC_LOOSE, g, 1);
        }
        n = cd_list_heroes(CD_OFF_DIR "/loose", heroes, CD_MAX_LOCAL);
        for (i = 0; i < n; i++)
        {
            if (Cd_LocalFind(heroes[i]) >= 0)
                continue;
            if (g < 0)
                g = cd_group_add("Manually installed", "loose", "", CD_SRC_LOOSE, 0);
            cd_local_add(heroes[i], CD_OFF_DIR "/loose", CD_SRC_LOOSE, g, 0);
        }
        if (g >= 0)
        {
            s_groups[g].enabled = 0;
            for (i = 0; i < s_localCount; i++)
                if (s_local[i].group == g && s_local[i].enabled)
                    s_groups[g].enabled = 1;
        }
    }

    /* Downloads after everything else: a custom copy of the same name is the
     * one that loads (cd_local_add keeps the first). Grouped by category. */
    for (i = 0; i < s_recCount; i++)
    {
        char path[320], cat[CD_NAME_MAX];
        int  g;

        snprintf(path, sizeof(path), CD_DL_DIR "/%s.ini", s_rec[i].hero);
        cd_ini_meta_path(path, NULL, 0, cat, sizeof(cat), NULL, 0, NULL, 0);
        if (cat[0] == '\0')
            snprintf(cat, sizeof(cat), "Other");
        snprintf(path, sizeof(path), CD_DL_DIR "/%s.glb", s_rec[i].hero);
        if (!cd_exists(path))
            continue;
        {
            int k = Cd_LocalFind(s_rec[i].hero);

            if (k >= 0 && s_local[k].enabled && !s_local[k].oldJojo)
                continue;
            if (k >= 0)
            {
                memmove(&s_local[k], &s_local[k + 1], (size_t)(s_localCount - k - 1) * sizeof(s_local[0]));
                s_localCount--;
            }
        }
        g = cd_group_add(cat, cat, "", CD_SRC_DOWNLOAD, 1);
        cd_local_add(s_rec[i].hero, CD_DL_DIR, CD_SRC_DOWNLOAD, g, 1);
    }

    /* Ports: jojo670's pack copied in by hand has no modinfo here; its files
     * match catalog entries but carry no name= in their ini. */
    for (i = 0; i < s_groupCount; i++)
        if (s_groups[i].source == CD_SRC_LOOSE)
            for (j = 0; j < s_localCount; j++)
            {
                int k;
                char name[8];

                if (s_local[j].group != i)
                    continue;
                {
                    char path[320];
                    snprintf(path, sizeof(path), CD_CHARA_DIR "/%s.ini", s_local[j].hero);
                    cd_ini_meta_path(path, name, sizeof(name), NULL, 0, NULL, 0, NULL, 0);
                }
                if (name[0] != '\0')
                    continue;
                for (k = 0; k < s_catCount; k++)
                    if (cd_ieq(s_cat[k].hero, s_local[j].hero) && SDL_strcasestr(s_cat[k].author, "jojo670"))
                    {
                        s_groups[i].oldJojo = 1;
                        s_local[j].oldJojo  = 1;
                        /* No ini metadata: the author is what it is known by. */
                        if (s_local[j].author[0] == '\0')
                            snprintf(s_local[j].author, sizeof(s_local[j].author), "jojo670");
                    }
            }

    SH_DBG("[CHARADL] local: %d characters in %d groups, %d downloads recorded", s_localCount, s_groupCount,
           s_recCount);
}

/* ------------------------------------------------------------------------ */
/* jobs                                                                      */

enum { CD_JOB_CATALOG, CD_JOB_INSTALL, CD_JOB_REMOVE, CD_JOB_PREVIEW, CD_JOB_GROUP_ON, CD_JOB_GROUP_OFF };

typedef struct
{
    int kind;
    int arg;
} CdJob;

static CdJob         s_jobs[CD_MAX_JOBS];
static int           s_jobHead, s_jobTail;
static int           s_jobRunning;   /* the job at head is in flight */
static volatile int  s_netState;     /* 0 idle, 1 running, 2 finished */
static volatile int  s_netStatus;
static volatile long long s_netGot;
static long long     s_netTotal;
static char          s_netUrl[512];
static char          s_netPath[320];
static char          s_status[128];
static int           s_restart;
static char          s_previewReady[CD_MAX_ENTRIES][CD_FILE_MAX + 24];

int Cd_Busy(void) { return s_jobHead != s_jobTail || s_jobRunning; }
int Cd_RestartAdvised(void) { return s_restart; }

const char* Cd_Status(float* progress)
{
    if (progress)
        *progress = (s_netState == 1 && s_netTotal > 0) ? (float)s_netGot / (float)s_netTotal : -1.0f;
    return s_status;
}

static void cd_push(int kind, int arg)
{
    int i, next = (s_jobTail + 1) % CD_MAX_JOBS;

    for (i = s_jobHead; i != s_jobTail; i = (i + 1) % CD_MAX_JOBS)
        if (s_jobs[i].kind == kind && s_jobs[i].arg == arg)
            return;
    if (next == s_jobHead)
        return;
    s_jobs[s_jobTail].kind = kind;
    s_jobs[s_jobTail].arg  = arg;
    s_jobTail = next;
}

#if defined(__EMSCRIPTEN__)
EM_JS(void, Cd_WebFetchJs, (const char* url, const char* path, int* got, int* done), {
    var u = UTF8ToString(url);
    var p = UTF8ToString(path);
    fetch(u).then(function (r) {
        if (!r.ok) { HEAP32[done >> 2] = r.status || -1; return; }
        return r.arrayBuffer().then(function (b) {
            var a = new Uint8Array(b);
            FS.writeFile(p, a);
            HEAP32[got >> 2] = a.length;
            HEAP32[done >> 2] = 200;
        });
    }).catch(function () { HEAP32[done >> 2] = -1; });
});
EM_JS(void, Cd_WebPersistJs, (void), {
    if (Module["shPersist"]) Module["shPersist"](function () {});
});
static int s_webGot, s_webDone;
#else
static int SDLCALL cd_net_worker(void* unused)
{
    (void)unused;
#if defined(CD_HTTP_FILE)
    s_netStatus = Pc_QolHttpDownload(s_netUrl, s_netPath, &s_netGot);
#else
    {
        char*  body = NULL;
        size_t len  = 0;
        int    st   = Pc_RaHttpRequest(s_netUrl, NULL, &body, &len);

        if (st >= 200 && st < 300 && body != NULL)
        {
            FILE* f = fopen(s_netPath, "wb");

            if (f == NULL || fwrite(body, 1, len, f) != len)
                st = 0;
            if (f)
                fclose(f);
            s_netGot = (long long)len;
        }
        free(body);
        s_netStatus = st;
    }
#endif
    s_netState = 2;
    return 0;
}
#endif

static void cd_net_start(const char* asset, long long size)
{
    cd_mkdir("gamedata");
    cd_mkdir(CD_TMP_DIR);
    snprintf(s_netPath, sizeof(s_netPath), CD_TMP_DIR "/%s", asset);
    remove(s_netPath);
    s_netGot    = 0;
    s_netTotal  = size;
    s_netStatus = 0;
    s_netState  = 1;
    /* Testing before anything is uploaded: a folder holding characters.json
     * and the zips stands in for the release. */
    if (cd_is_dir(CD_TEST_DIR))
    {
        char from[320];

        snprintf(from, sizeof(from), CD_TEST_DIR "/%s", asset);
        s_netStatus = cd_copy(from, s_netPath) ? 200 : 404;
        s_netGot    = size;
        s_netState  = 2;
        SH_DBG("[CHARADL] test source: %s -> %d", from, s_netStatus);
        return;
    }
#if defined(__EMSCRIPTEN__)
    snprintf(s_netUrl, sizeof(s_netUrl), CD_WEB_RELAY "%s", asset);
    s_webGot = s_webDone = 0;
    Cd_WebFetchJs(s_netUrl, s_netPath, &s_webGot, &s_webDone);
#else
    snprintf(s_netUrl, sizeof(s_netUrl), CD_RELEASE_BASE "%s", asset);
    {
        SDL_Thread* t = SDL_CreateThread(cd_net_worker, "SH_CHARADL", NULL);

        if (t)
            SDL_DetachThread(t);
        else
        {
            s_netStatus = 0;
            s_netState  = 2;
        }
    }
#endif
}

/* 1 on the frame the transfer finished (s_netStatus then holds the result). */
static int cd_net_poll(void)
{
#if defined(__EMSCRIPTEN__)
    if (s_netState == 1)
    {
        s_netGot = s_webGot;
        if (s_webDone != 0)
        {
            s_netStatus = s_webDone;
            s_netState  = 2;
        }
    }
#endif
    if (s_netState != 2)
        return 0;
    s_netState = 0;
    return 1;
}

static void cd_allow_loose(void)
{
    if (!g_PcConfig.allowLooseFiles)
    {
        g_PcConfig.allowLooseFiles = 1;
        PcConfig_SaveKeyValue("allow_loose_files", "1");
    }
}

/* Unpacks the zip's load/CHARA files into `dir`; the glb base into heroOut. */
static int cd_unzip_chara(const char* zipPath, const char* dir, int glbOnly, char* heroOut, int heroSz)
{
    mz_zip_archive zip;
    mz_uint        i, n;
    int            wrote = 0, ok = 1;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath, 0))
        return 0;
    n = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < n && ok; i++)
    {
        char        name[512], dest[400];
        const char* base;
        const char* dot;

        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (strstr(name, "..") || SDL_strcasestr(name, "load/CHARA/") == NULL)
            continue;
        base = strrchr(name, '/') + 1;
        dot  = strrchr(base, '.');
        if (!base[0] || dot == NULL)
            continue;
        if (glbOnly && !cd_ieq(dot, ".glb") && !cd_ieq(dot, ".ini"))
            continue;
        snprintf(dest, sizeof(dest), "%s/%s", dir, base);
        if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0))
            ok = 0;
        else
        {
            wrote++;
            if (heroOut && (cd_ieq(dot, ".glb") || cd_ieq(dot, ".gltf")))
                cd_hero_base(base, heroOut, heroSz);
        }
    }
    mz_zip_reader_end(&zip);
    return ok && wrote > 0;
}

static void cd_persist(void)
{
#if defined(__EMSCRIPTEN__)
    Cd_WebPersistJs();
#endif
}

/* Downloads went into load/CHARA before the load order: move them over. A
 * recorded file there was the download's (the launcher kept mods off it). */
static void cd_migrate_downloads(void)
{
    static const char* const ext[2] = { "glb", "ini" };
    int                      i, k;

    for (i = 0; i < s_recCount; i++)
    {
        char from[320], to[320];

        snprintf(to, sizeof(to), CD_DL_DIR "/%s.glb", s_rec[i].hero);
        snprintf(from, sizeof(from), CD_CHARA_DIR "/%s.glb", s_rec[i].hero);
        if (cd_exists(to) || !cd_exists(from))
            continue;
        cd_mkdir("gamedata/load");
        cd_mkdir(CD_DL_DIR);
        {
            int ok = 1;

            for (k = 0; k < 2; k++)
            {
                snprintf(from, sizeof(from), CD_CHARA_DIR "/%s.%s", s_rec[i].hero, ext[k]);
                snprintf(to, sizeof(to), CD_DL_DIR "/%s.%s", s_rec[i].hero, ext[k]);
                if (cd_exists(from) && !cd_move(from, to))
                    ok = 0;
            }
            SH_DBG("[CHARADL] %s download %s to " CD_DL_DIR, ok ? "moved" : "could not move", s_rec[i].hero);
        }
    }
}

/* The old pack's copy of a file just downloaded would load first from
 * load/CHARA. A launcher mod's copy is only deleted (its library keeps it and
 * the launcher stops deploying it); one copied in by hand is parked. */
static void cd_old_jojo_step_aside(const char* hero)
{
    static const char* const ext[2] = { "glb", "ini" };
    int                      i = Cd_LocalFind(hero), k;
    const CdGroup*           g;

    if (i < 0 || s_local[i].source == CD_SRC_DOWNLOAD || !s_local[i].enabled || !s_local[i].oldJojo)
        return;
    g = &s_groups[s_local[i].group];
    for (k = 0; k < 2; k++)
    {
        char from[320], to[320];

        snprintf(from, sizeof(from), CD_CHARA_DIR "/%s.%s", hero, ext[k]);
        if (!cd_exists(from))
            continue;
        s_restart = 1;
        if (g->source == CD_SRC_PACK)
            remove(from);
        else
        {
            snprintf(to, sizeof(to), CD_OFF_DIR "/%s", g->key);
            cd_mkdir(CD_OFF_DIR);
            cd_mkdir(to);
            snprintf(to, sizeof(to), CD_OFF_DIR "/%s/%s.%s", g->key, hero, ext[k]);
            cd_move(from, to);
        }
    }
}

static void cd_finish_install(const CdEntry* e)
{
    char      hero[CD_FILE_MAX] = "";
    CdRecord* r;
    char      glb[320];

    if (s_netStatus < 200 || s_netStatus >= 300 || (e->size > 0 && s_netGot > 0 && s_netGot != e->size))
    {
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("{name}: download failed", "{name}", e->name));
        remove(s_netPath);
        return;
    }
    cd_mkdir("gamedata/load");
    cd_mkdir(CD_DL_DIR);
    snprintf(glb, sizeof(glb), CD_DL_DIR "/%s.glb", e->hero);
    /* A file the running game may already have read: its copy in memory stays
     * until a restart. */
    if (cd_exists(glb))
        s_restart = 1;
    if (!cd_unzip_chara(s_netPath, CD_DL_DIR, 0, hero, sizeof(hero)))
    {
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("{name}: could not install", "{name}", e->name));
        remove(s_netPath);
        return;
    }
    remove(s_netPath);
    r = cd_record_by_id(e->id);
    if (r != NULL && hero[0] && !cd_ieq(r->hero, hero))
    {
        char p[320];

        snprintf(p, sizeof(p), CD_DL_DIR "/%s.glb", r->hero);
        remove(p);
        snprintf(p, sizeof(p), CD_DL_DIR "/%s.ini", r->hero);
        remove(p);
    }
    if (r == NULL && s_recCount < CD_MAX_ENTRIES)
        r = &s_rec[s_recCount++];
    if (r != NULL)
    {
        memset(r, 0, sizeof(*r));
        snprintf(r->id, sizeof(r->id), "%s", e->id);
        snprintf(r->version, sizeof(r->version), "%s", e->version[0] ? e->version : "1");
        snprintf(r->sha256, sizeof(r->sha256), "%s", e->sha256[0] ? e->sha256 : "-");
        snprintf(r->hero, sizeof(r->hero), "%s", hero[0] ? hero : e->hero);
    }
    cd_records_save();
    cd_old_jojo_step_aside(hero[0] ? hero : e->hero);
    cd_allow_loose();
    cd_persist();
    snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("Installed {name}", "{name}", e->name));
    SH_DBG("[CHARADL] installed %s (%s %s)", e->id, e->hero, e->version);
}

static void cd_do_remove(const CdEntry* e)
{
    CdRecord* r = cd_record_by_id(e->id);
    char      p[320];

    if (r == NULL)
        return;
    snprintf(p, sizeof(p), CD_DL_DIR "/%s.glb", r->hero);
    remove(p);
    snprintf(p, sizeof(p), CD_DL_DIR "/%s.ini", r->hero);
    remove(p);
    cd_record_drop(e->id);
    cd_records_save();
    cd_persist();
    snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("Removed {name}", "{name}", e->name));
    SH_DBG("[CHARADL] removed %s", e->id);
}

static void cd_finish_preview(int idx)
{
    const CdEntry* e = &s_cat[idx];
    char           hero[CD_FILE_MAX] = "";

    if (s_netStatus < 200 || s_netStatus >= 300)
    {
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("{name}: preview failed", "{name}", e->name));
        remove(s_netPath);
        return;
    }
    cd_mkdir(CD_PREVIEW_DIR);
    if (cd_unzip_chara(s_netPath, CD_PREVIEW_DIR, 1, hero, sizeof(hero)) && hero[0])
    {
        /* Relative to load/CHARA, where the skin loader builds its paths. */
        snprintf(s_previewReady[idx], sizeof(s_previewReady[idx]), "../../.chr_preview/%s", hero);
        s_status[0] = '\0';
    }
    else
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("{name}: preview failed", "{name}", e->name));
    remove(s_netPath);
}

const char* Cd_PreviewFile(int idx)
{
    if (idx < 0 || idx >= s_catCount)
        return NULL;
    /* Installed: the real file is the preview. */
    if (cd_record_by_id(s_cat[idx].id) && !Cd_CustomCopyWins(&s_cat[idx]))
    {
        static char installed[CD_FILE_MAX + 16];
        snprintf(installed, sizeof(installed), "../CHARA_dl/%s", cd_record_by_id(s_cat[idx].id)->hero);
        return installed;
    }
    /* Already here from a pack, the launcher or by hand: preview that copy,
     * no download needed. */
    {
        int li = Cd_LocalFind(s_cat[idx].hero);

        if (li >= 0)
        {
            static char local[CD_FILE_MAX + 64];
            const CdLocal* l = &s_local[li];

            if (l->enabled)
                snprintf(local, sizeof(local), "%s", l->hero);
            else
                snprintf(local, sizeof(local), "../CHARA_off/%s/%s", s_groups[l->group].key, l->hero);
            return local;
        }
    }
    return s_previewReady[idx][0] ? s_previewReady[idx] : NULL;
}

/* Local groups: parking a pack moves its files out of load/CHARA (the game
 * reads nothing else), and for a launcher mod flips its Enabled too. */
static void cd_group_toggle(int g, int enabled)
{
    int i;
    char offDir[200];

    if (g < 0 || g >= s_groupCount)
        return;
    snprintf(offDir, sizeof(offDir), CD_OFF_DIR "/%s", s_groups[g].key);
#if defined(CD_HAS_LAUNCHER_MODS)
    if (s_groups[g].source == CD_SRC_PACK && !cd_qol_installed(s_groups[g].key))
    {
        char dir[300];

        if (cd_mod_chara_dir(s_groups[g].key, dir, sizeof(dir)))
            for (i = 0; i < s_localCount; i++)
            {
                char from[400], to[400];
                const char* ext[2] = { "glb", "ini" };
                int k;

                if (s_local[i].group != g)
                    continue;
                for (k = 0; k < 2; k++)
                {
                    snprintf(from, sizeof(from), "%s/%s.%s", dir, s_local[i].hero, ext[k]);
                    snprintf(to, sizeof(to), CD_CHARA_DIR "/%s.%s", s_local[i].hero, ext[k]);
                    if (enabled)
                        cd_copy(from, to);
                    else
                        remove(to);
                }
            }
        cd_launcher_set_enabled(s_groups[g].key, enabled);
        snprintf(s_status, sizeof(s_status), "%s",
                 Pc_LangQuickFill(enabled ? "{name} enabled" : "{name} disabled", "{name}", s_groups[g].name));
        return;
    }
#endif
    cd_mkdir(CD_OFF_DIR);
    cd_mkdir(offDir);
    for (i = 0; i < s_localCount; i++)
    {
        const char* ext[2] = { "glb", "ini" };
        int         k;

        if (s_local[i].group != g || s_local[i].source == CD_SRC_DOWNLOAD)
            continue;
        for (k = 0; k < 2; k++)
        {
            char on[400], off[400];

            snprintf(on, sizeof(on), CD_CHARA_DIR "/%s.%s", s_local[i].hero, ext[k]);
            snprintf(off, sizeof(off), "%s/%s.%s", offDir, s_local[i].hero, ext[k]);
            if (enabled)
                cd_move(off, on);
            else if (cd_exists(on))
                cd_move(on, off);
        }
    }
    cd_persist();
    snprintf(s_status, sizeof(s_status), "%s",
                 Pc_LangQuickFill(enabled ? "{name} enabled" : "{name} disabled", "{name}", s_groups[g].name));
}

void Cd_CatalogFetch(void)
{
    if (s_catState == CD_CAT_LOADING || s_catState == CD_CAT_READY)
        return;
    s_catState = CD_CAT_LOADING;
    cd_push(CD_JOB_CATALOG, 0);
}

void Cd_CatalogRefresh(void)
{
    /* Queued jobs hold catalog indices, which a new list could reorder. */
    if (s_catState == CD_CAT_LOADING || Cd_Busy())
        return;
    s_catState = CD_CAT_LOADING;
    cd_push(CD_JOB_CATALOG, 0);
}

void Cd_QueueInstall(int idx)  { if (s_catState != CD_CAT_LOADING && idx >= 0 && idx < s_catCount) cd_push(CD_JOB_INSTALL, idx); }
void Cd_QueueRemove(int idx)   { if (s_catState != CD_CAT_LOADING && idx >= 0 && idx < s_catCount) cd_push(CD_JOB_REMOVE, idx); }
void Cd_QueuePreview(int idx)  { if (s_catState != CD_CAT_LOADING && idx >= 0 && idx < s_catCount && !Cd_PreviewFile(idx)) cd_push(CD_JOB_PREVIEW, idx); }
void Cd_SetGroupEnabled(int g, int enabled) { cd_push(enabled ? CD_JOB_GROUP_ON : CD_JOB_GROUP_OFF, g); }

int Cd_OldJojoPending(void)
{
    int i;

    if (s_catState != CD_CAT_READY)
        return 0;
    for (i = 0; i < s_groupCount; i++)
        if (s_groups[i].oldJojo && s_groups[i].enabled)
            return 1;
    return 0;
}

void Cd_UpdateOldJojo(void)
{
    int i, k;

    for (i = 0; i < s_groupCount; i++)
    {
        if (!s_groups[i].oldJojo)
            continue;
        for (k = 0; k < s_localCount; k++)
        {
            int c;

            if (s_local[k].group != i)
                continue;
            for (c = 0; c < s_catCount; c++)
                if (cd_ieq(s_cat[c].hero, s_local[k].hero))
                    cd_push(CD_JOB_INSTALL, c);
        }
        /* The launcher must not deploy the old copies over the new ones. */
        if (s_groups[i].source == CD_SRC_PACK)
        {
#if defined(CD_HAS_LAUNCHER_MODS)
            cd_launcher_set_enabled(s_groups[i].key, 0);
#endif
        }
        s_groups[i].oldJojo = 0;
    }
}

static void cd_job_begin(const CdJob* j)
{
    switch (j->kind)
    {
    case CD_JOB_CATALOG:
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuick("Fetching the character list..."));
        cd_net_start(CD_CATALOG_FILE, 0);
        break;
    case CD_JOB_INSTALL:
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("Downloading {name}...", "{name}", s_cat[j->arg].name));
        cd_net_start(s_cat[j->arg].file, s_cat[j->arg].size);
        break;
    case CD_JOB_PREVIEW:
        snprintf(s_status, sizeof(s_status), "%s", Pc_LangQuickFill("Downloading the preview of {name}...", "{name}", s_cat[j->arg].name));
        cd_net_start(s_cat[j->arg].file, s_cat[j->arg].size);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* the QoL character packs -> the catalog                                    */

#define CD_JOJO_NOASK "gamedata/chara_jojo_noask"
#define CD_MIGRATED   "gamedata/chara_migrated.txt"
#define CD_QOL_NOASK  "gamedata/chara_qol_noask"

static int s_legacyGo; /* the player said yes this session */
/* Worked out when the list arrives, not per frame: it reads files. */
static int s_legacyPend;

int Cd_LegacyNoAsk(void) { return cd_exists(CD_QOL_NOASK); }

void Cd_SetLegacyNoAsk(void)
{
    FILE* f = fopen(CD_QOL_NOASK, "w");

    if (f != NULL)
    {
        fputs("The QoL character pack offer was turned off (Don't ask again). Delete to be asked again.\n", f);
        fclose(f);
    }
}

/* Replaced once already: a pack turned back on after that (the old
 * launcher can) is the player's choice and is not offered again. */
static int cd_legacy_done(const char* id)
{
    char* t = cd_read_all(CD_MIGRATED, 1 << 20);
    char* l;
    int   done = 0;

    if (t == NULL)
        return 0;
    for (l = strtok(t, "\r\n"); l != NULL && !done; l = strtok(NULL, "\r\n"))
        done = strcmp(l, id) == 0;
    free(t);
    return done;
}

int Cd_OldJojoNoAsk(void) { return cd_exists(CD_JOJO_NOASK); }

void Cd_SetOldJojoNoAsk(void)
{
    FILE* f = fopen(CD_JOJO_NOASK, "w");

    if (f != NULL)
    {
        fputs("The old jojo670 pack offer was turned off (Don't ask again). Delete to be asked again.\n", f);
        fclose(f);
    }
}

static int cd_crc_file(const char* path, long long* size, unsigned* crc)
{
    FILE*         f = fopen(path, "rb");
    unsigned char buf[65536];
    size_t        n;
    mz_ulong      c = MZ_CRC32_INIT;

    if (f == NULL)
        return 0;
    *size = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        c = mz_crc32(c, buf, n);
        *size += (long long)n;
    }
    fclose(f);
    *crc = (unsigned)c;
    return 1;
}

static void cd_qol_unmark(const char* id)
{
    char* txt = cd_read_all("gamedata/qol_installed.txt", 1024 * 1024);
    FILE* f;
    char* line;

    if (txt == NULL)
        return;
    f = fopen("gamedata/qol_installed.txt", "w");
    if (f != NULL)
    {
        for (line = strtok(txt, "\n"); line != NULL; line = strtok(NULL, "\n"))
        {
            size_t n = strcspn(line, " \r");

            if (!(n == strlen(id) && strncmp(line, id, n) == 0) && line[0] != '\r')
                fprintf(f, "%s\n", line);
        }
        fclose(f);
    }
    free(txt);
}

#if defined(CD_HAS_LAUNCHER_MODS)
/* The launcher mod the Essential list installed as `essentialId`. */
static int cd_find_essential_mod(const char* essentialId, CdMod* out)
{
    char*       json = cd_read_all("mods/modmanager.json", 4 * 1024 * 1024);
    const char* p;
    int         found = 0;

    if (json == NULL)
        return 0;
    p = strstr(json, "\"Mods\"");
    if (p && (p = strchr(p, '[')) != NULL)
        for (p++; *p && !found;)
        {
            while (*p && *p != '{' && *p != ']')
                p++;
            if (*p != '{')
                break;
            memset(out, 0, sizeof(*out));
            p = cd_json_object(p, cd_mod_field, out);
            found = out->name[0] && cd_ieq(out->essential, essentialId);
        }
    free(json);
    return found;
}
#endif

/* Characters first shipped as QoL packs are catalog entries now. Each one a
 * pack still installed has is downloaded again from the catalog (matched by
 * its model's size and CRC-32, else its file name), and the pack's copy is
 * deleted once that download is in, so a failed download never loses a
 * character. When every one of a pack's characters is in, the pack itself is
 * uninstalled, so it neither shadows the downloads nor comes back. Runs when
 * the list arrives and again when the downloads it queued are done. */
static unsigned char s_legacyTried[CD_MAX_ENTRIES]; /* queued this session */

static int cd_legacy_entry(const char* glb, const char* hero)
{
    long long size = 0;
    unsigned  crc = 0;
    int       k;

    if (cd_crc_file(glb, &size, &crc) && size > 0)
        for (k = 0; k < s_catCount; k++)
            if (s_cat[k].glbSize == size && s_cat[k].glbCrc == crc)
                return k;
    for (k = 0; k < s_catCount; k++)
        if (cd_ieq(s_cat[k].hero, hero))
            return k;
    return -1;
}

/* `queue` 0 only reports whether a pack is waiting to be replaced. */
static int cd_migrate_legacy_pass(int queue)
{
    int i, j, pending = 0;

    for (i = 0; i < CD_LEGACY_COUNT; i++)
    {
        const CdLegacyPack* L = &s_legacy[i];
        char                modDir[300] = "", off[200];
        int                 ports = cd_qol_installed(L->id), waiting = 0;
#if defined(CD_HAS_LAUNCHER_MODS)
        CdMod               mod;
        int                 pc = !ports && cd_find_essential_mod(L->id, &mod) && cd_mod_chara_dir(mod.name, modDir, sizeof(modDir));
#else
        int                 pc = 0;
#endif

        if ((!ports && !pc) || cd_legacy_done(L->id))
            continue;
#if defined(CD_HAS_LAUNCHER_MODS)
        if (pc && !mod.enabled)
            continue;
#endif
        snprintf(off, sizeof(off), CD_OFF_DIR "/%s", L->id);
        for (j = 0; L->heroes[j] != NULL; j++)
        {
            const char* h = L->heroes[j];
            const char* dirs[3] = { CD_CHARA_DIR, off, modDir };
            char        src[400] = "", p[400];
            CdRecord*   r;
            int         k, idx;

            for (k = 0; k < 3 && !src[0]; k++)
            {
                if (!dirs[k][0])
                    continue;
                snprintf(src, sizeof(src), "%s/%s.glb", dirs[k], h);
                if (!cd_exists(src))
                    src[0] = '\0';
            }
            if (!src[0] || (idx = cd_legacy_entry(src, h)) < 0)
                continue;
            if (!queue)
            {
                pending = 1;
                break;
            }
            r = cd_record_by_id(s_cat[idx].id);
            snprintf(p, sizeof(p), CD_DL_DIR "/%s.glb", r ? r->hero : "");
            if (r == NULL || !cd_exists(p))
            {
                if (!s_legacyTried[idx])
                {
                    s_legacyTried[idx] = 1;
                    cd_push(CD_JOB_INSTALL, idx);
                    SH_DBG("[CHARADL] %s: %s -> downloading %s", L->id, h, s_cat[idx].id);
                }
                waiting++;
                continue;
            }
            for (k = 0; k < 2; k++)
            {
                const char* d = k == 0 ? CD_CHARA_DIR : off;

                snprintf(p, sizeof(p), "%s/%s.glb", d, h);
                if (cd_exists(p))
                    s_restart = 1;
                remove(p);
                snprintf(p, sizeof(p), "%s/%s.ini", d, h);
                remove(p);
            }
        }
        if (!queue || waiting > 0)
            continue;
        if (ports)
            cd_qol_unmark(L->id);
#if defined(CD_HAS_LAUNCHER_MODS)
        if (pc)
            cd_launcher_set_enabled(mod.name, 0);
#endif
        {
            /* The launcher deletes the pack from its library when it sees this. */
            FILE* f = fopen(CD_MIGRATED, "a");

            if (f != NULL)
            {
                fprintf(f, "%s\n", L->id);
                fclose(f);
            }
        }
        cd_persist();
        SH_DBG("[CHARADL] QoL pack %s replaced by its Additional Characters downloads", L->id);
    }
    return pending;
}

static void cd_migrate_legacy(void)
{
    if (s_legacyGo)
        cd_migrate_legacy_pass(1);
    else
        s_legacyPend = cd_migrate_legacy_pass(0);
}

int Cd_LegacyPending(void)
{
    return s_catState == CD_CAT_READY && !s_legacyGo && s_legacyPend;
}

void Cd_UpdateLegacy(void)
{
    if (s_catState != CD_CAT_READY)
        return;
    s_legacyGo = 1;
    cd_records_load();
    cd_migrate_legacy_pass(1);
}

void Cd_Tick(void)
{
    CdJob* j;

    if (!s_jobRunning)
    {
        if (s_jobHead == s_jobTail)
            return;
        j = &s_jobs[s_jobHead];
        if (j->kind == CD_JOB_REMOVE)
        {
            cd_do_remove(&s_cat[j->arg]);
            s_jobHead = (s_jobHead + 1) % CD_MAX_JOBS;
            Cd_LocalScan();
            return;
        }
        if (j->kind == CD_JOB_GROUP_ON || j->kind == CD_JOB_GROUP_OFF)
        {
            cd_group_toggle(j->arg, j->kind == CD_JOB_GROUP_ON);
            s_jobHead = (s_jobHead + 1) % CD_MAX_JOBS;
            Cd_LocalScan();
            return;
        }
        cd_job_begin(j);
        s_jobRunning = 1;
        return;
    }
    if (!cd_net_poll())
        return;
    j = &s_jobs[s_jobHead];
    switch (j->kind)
    {
    case CD_JOB_CATALOG:
    {
        char* json = (s_netStatus >= 200 && s_netStatus < 300) ? cd_read_all(s_netPath, 2 * 1024 * 1024) : NULL;

        if (json != NULL && cd_parse_catalog(json) > 0)
        {
            s_catState = CD_CAT_READY;
            memset(s_previewReady, 0, sizeof(s_previewReady));
            cd_records_load();
            cd_migrate_legacy();
        }
        else
            s_catState = s_catCount > 0 ? CD_CAT_READY : CD_CAT_FAILED;
        free(json);
        remove(s_netPath);
        snprintf(s_status, sizeof(s_status), "%s",
                 s_catState == CD_CAT_FAILED ? Pc_LangQuick("The character list could not be downloaded") : "");
        SH_DBG("[CHARADL] catalog: status %d, %d characters", s_netStatus, s_catCount);
        Cd_LocalScan();
        break;
    }
    case CD_JOB_INSTALL:
        cd_finish_install(&s_cat[j->arg]);
        /* The last of a QoL pack's downloads: the pack can go now. */
        if ((s_jobHead + 1) % CD_MAX_JOBS == s_jobTail)
            cd_migrate_legacy();
        Cd_LocalScan();
        break;
    case CD_JOB_PREVIEW:
        cd_finish_preview(j->arg);
        break;
    default:
        break;
    }
    s_jobRunning = 0;
    s_jobHead    = (s_jobHead + 1) % CD_MAX_JOBS;
}
