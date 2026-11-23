/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_qol_update.c - offers updates for the Essential QoL mods the launcher's
 * Mod Manager installed. See pc_qol_update.h.
 *
 * Why the game does this. The launcher brings a mod up to date only after it
 * has installed a game build, and a realtime update never replaces the
 * launcher, so a fixed mod could sit unseen behind an old launcher for good.
 * The game is the one part every player updates, so the first time a new build
 * reaches the title it reads the catalog (essential-mods.json on the qol-mods
 * release) and asks about anything out of date.
 *
 * What counts as out of date. A catalog entry may carry "verify": one
 * "crc32 size path" line per file, checked against the copy in mods/<Name>/,
 * so the answer comes from the files themselves. The launcher's own record
 * (EssentialSha in mods/modmanager.json) cannot be trusted for this: it is
 * missing for anything installed before it was kept, and a launcher that finds
 * it missing writes down the catalog's current value without looking at the
 * files. Without "verify" that record is all there is, and a mod is out of
 * date when its recorded sha256 differs from the catalog's.
 *
 * An update has to leave the folders the way the Mod Manager would. Its next
 * Apply compares every deployed file with the library copy by size and
 * last-write time; one that differs while mods/deployed.txt says otherwise is
 * taken for the player's own edit, backed up, and put back when the mod is
 * removed. So: the library folder is replaced whole, a deployed file is only
 * replaced when it still is the old library copy, the copy keeps the library
 * file's time (pc_file_ops.c), and deployed.txt and EssentialSha are brought
 * along. Texture packs and mods with code or video are left to the launcher,
 * whose unpack and deploy rules for those are not repeated here.
 *
 * Testing before anything is uploaded: a folder gamedata/qol_catalog_test
 * holding essential-mods.json and the zips stands in for the release.
 */

#include "pc_qol_update.h"

#if defined(__ANDROID__) || defined(SH_IOS) || defined(__EMSCRIPTEN__)

int Pc_QolUpdate_TitleFrame(int left, int right, int confirm, int cancel)
{
    (void)left; (void)right; (void)confirm; (void)cancel;
    return 0;
}

int Pc_QolUpdate_IsOpen(void)
{
    return 0;
}

#else

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

#include <SDL.h>

#include "miniz.h"
#include "pc_config.h"
#include "pc_confirm_dialog.h"
#include "pc_file_ops.h"
#include "pc_mod_registry.h"
#include "pc_ra_http.h"
#include "sh_build_info.h"
#include "sh_log.h"

#define QU_RELEASE_BASE "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/"
#define QU_CATALOG_FILE "essential-mods.json"
#define QU_STATE_FILE   "mods/modmanager.json"
#define QU_MANIFEST     "mods/deployed.txt"
/* The Mod Manager skips folders whose name starts with a dot. */
#define QU_WORK_DIR     "mods/.qolupdate"
#define QU_TEST_DIR     "gamedata/qol_catalog_test"
#define QU_LOAD_REL     "gamedata\\load\\"
#define QU_CHECKED_KEY  "qol_mods_checked_build"
#define QU_ASK_KEY      "qol_mods_update_ask"
#define QU_TITLE        "Essential QoL Mods"

#define QU_MAX_MODS     32
#define QU_PATH         600
/* Two seconds in .NET ticks: the Mod Manager's own tolerance (SameFile). */
#define QU_SAME_TICKS   20000000LL

/* ------------------------------------------------------------------ */
/* Strings and files                                                   */
/* ------------------------------------------------------------------ */

static int qu_ieq(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
    }
}

static int qu_iprefix(const char* s, const char* pre)
{
    for (; *pre; s++, pre++)
        if (tolower((unsigned char)*s) != tolower((unsigned char)*pre))
            return 0;
    return 1;
}

static int qu_iends(const char* s, const char* suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && qu_ieq(s + n - m, suf);
}

/* Equal as paths: case-blind, and either slash. */
static int qu_path_eq(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca == '\\') ca = '/';
        if (cb == '\\') cb = '/';
        if (ca != cb)
            return 0;
        if (ca == 0)
            return 1;
    }
}

static void qu_mkdir(const char* p)
{
#if defined(_WIN32)
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

static void qu_rmdir(const char* p)
{
#if defined(_WIN32)
    _rmdir(p);
#else
    rmdir(p);
#endif
}

static void qu_mkdirs_for(const char* filePath)
{
    char   buf[QU_PATH];
    size_t i, n = strlen(filePath);

    if (n >= sizeof(buf))
        return;
    memcpy(buf, filePath, n + 1);
    for (i = 1; i < n; i++)
    {
        if (buf[i] == '/' || buf[i] == '\\')
        {
            char c = buf[i];
            buf[i] = '\0';
            qu_mkdir(buf);
            buf[i] = c;
        }
    }
}

static int qu_is_dir(const char* p)
{
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR);
}

static char* qu_read_all(const char* path, long long maxBytes, size_t* outLen)
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
    if (outLen)
        *outLen = got;
    return buf;
}

/* Written beside the file and moved over it, so a write cut short leaves the
 * old file whole. */
static int qu_write_all(const char* path, const char* data, size_t len)
{
    char  tmp[QU_PATH];
    FILE* f;
    int   ok;

    snprintf(tmp, sizeof(tmp), "%s.new", path);
    if ((f = fopen(tmp, "wb")) == NULL)
        return 0;
    ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0)
        ok = 0;
    if (ok && !PcFile_MoveReplace(tmp, path))
        ok = PcFile_CopyKeepTime(tmp, path);
    remove(tmp);
    return ok;
}

/* A path taken from the catalog or a zip: it has to stay under the folder it
 * is joined to. */
static int qu_safe_rel(const char* p)
{
    const char* s = p;

    if (p[0] == '\0' || p[0] == '/' || strchr(p, '\\') || strchr(p, ':'))
        return 0;
    while (*s)
    {
        const char* e = strchr(s, '/');
        size_t      n = e ? (size_t)(e - s) : strlen(s);

        if (n == 0 || (n == 2 && s[0] == '.' && s[1] == '.') || (n == 1 && s[0] == '.'))
            return 0;
        s += n + (e ? 1 : 0);
    }
    return 1;
}

static int qu_safe_name(const char* p)
{
    return p[0] != '\0' && !strchr(p, '/') && !strchr(p, '\\') && !strchr(p, ':') &&
           strcmp(p, ".") != 0 && strcmp(p, "..") != 0;
}

typedef struct
{
    char** v;
    int    n, cap;
} QuList;

static int qu_list_add(QuList* l, const char* s)
{
    if (l->n == l->cap)
    {
        int    cap = l->cap ? l->cap * 2 : 64;
        char** v   = (char**)realloc(l->v, (size_t)cap * sizeof(char*));
        if (v == NULL)
            return 0;
        l->v   = v;
        l->cap = cap;
    }
    if ((l->v[l->n] = SDL_strdup(s)) == NULL)
        return 0;
    l->n++;
    return 1;
}

static void qu_list_free(QuList* l)
{
    int i;
    for (i = 0; i < l->n; i++)
        SDL_free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

/* Every file under root, as paths relative to it with '/' between parts. */
static int qu_collect(const char* root, const char* rel, QuList* out, int depth)
{
    char           dir[QU_PATH];
    DIR*           d;
    struct dirent* e;
    int            ok = 1;

    if (depth > 24)
        return 0;
    if (rel[0])
        snprintf(dir, sizeof(dir), "%s/%s", root, rel);
    else
        snprintf(dir, sizeof(dir), "%s", root);
    if ((d = opendir(dir)) == NULL)
        return 0;
    while (ok && (e = readdir(d)) != NULL)
    {
        char sub[QU_PATH], full[QU_PATH];

        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (rel[0])
            snprintf(sub, sizeof(sub), "%s/%s", rel, e->d_name);
        else
            snprintf(sub, sizeof(sub), "%s", e->d_name);
        snprintf(full, sizeof(full), "%s/%s", root, sub);
        if (qu_is_dir(full))
            ok = qu_collect(root, sub, out, depth + 1);
        else
            ok = qu_list_add(out, sub);
    }
    closedir(d);
    return ok;
}

static void qu_rmtree(const char* dir)
{
    QuList files = { NULL, 0, 0 };
    QuList dirs  = { NULL, 0, 0 };
    int    i, j;

    if (!qu_is_dir(dir))
        return;
    qu_collect(dir, "", &files, 0);
    for (i = 0; i < files.n; i++)
    {
        char  full[QU_PATH];
        char* cut;

        snprintf(full, sizeof(full), "%s/%s", dir, files.v[i]);
        remove(full);
        /* Its folders, each once, to be removed deepest first. */
        while ((cut = strrchr(files.v[i], '/')) != NULL)
        {
            *cut = '\0';
            for (j = 0; j < dirs.n; j++)
                if (strcmp(dirs.v[j], files.v[i]) == 0)
                    break;
            if (j == dirs.n)
                qu_list_add(&dirs, files.v[i]);
        }
    }
    for (;;)
    {
        size_t longest = 0;
        int    at      = -1;

        for (j = 0; j < dirs.n; j++)
            if (dirs.v[j][0] && strlen(dirs.v[j]) > longest)
            {
                longest = strlen(dirs.v[j]);
                at      = j;
            }
        if (at < 0)
            break;
        {
            char full[QU_PATH];
            snprintf(full, sizeof(full), "%s/%s", dir, dirs.v[at]);
            qu_rmdir(full);
            dirs.v[at][0] = '\0';
        }
    }
    qu_rmdir(dir);
    qu_list_free(&files);
    qu_list_free(&dirs);
}

static int qu_copy_tree(const char* from, const char* to)
{
    QuList files = { NULL, 0, 0 };
    int    i, ok;

    ok = qu_collect(from, "", &files, 0);
    for (i = 0; ok && i < files.n; i++)
    {
        char a[QU_PATH], b[QU_PATH];

        snprintf(a, sizeof(a), "%s/%s", from, files.v[i]);
        snprintf(b, sizeof(b), "%s/%s", to, files.v[i]);
        qu_mkdirs_for(b);
        ok = PcFile_CopyKeepTime(a, b);
    }
    qu_list_free(&files);
    return ok;
}

static int qu_crc_file(const char* path, long long* size, unsigned* crc)
{
    unsigned char buf[65536];
    FILE*         f = fopen(path, "rb");
    mz_ulong      c = MZ_CRC32_INIT;
    long long     total = 0;
    size_t        n;

    if (f == NULL)
        return 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        c = mz_crc32(c, buf, n);
        total += (long long)n;
    }
    fclose(f);
    *size = total;
    *crc  = (unsigned)c;
    return 1;
}

/* ------------------------------------------------------------------ */
/* JSON, as far as these two files need it                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    const char* key;
    int         keyLen;
    const char* val;    /* first char of the value */
    const char* valEnd; /* the char after it */
} QuMember;

/* p at the opening quote; returns the char after the closing one. */
static const char* qu_js_skip_string(const char* p)
{
    p++;
    while (*p && *p != '"')
    {
        if (*p == '\\' && p[1])
            p++;
        p++;
    }
    return *p == '"' ? p + 1 : p;
}

static const char* qu_js_skip_value(const char* p)
{
    if (*p == '"')
        return qu_js_skip_string(p);
    if (*p == '{' || *p == '[')
    {
        int depth = 0;
        do
        {
            if (*p == '"')
            {
                p = qu_js_skip_string(p);
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']')
                depth--;
            p++;
        } while (*p && depth > 0);
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* p inside an object, at or before a member. Returns the char after the
 * member's value, or NULL at the end of the object. */
static const char* qu_js_member(const char* p, QuMember* m)
{
    while (*p && *p != '"' && *p != '}')
        p++;
    if (*p != '"')
        return NULL;
    m->key = p + 1;
    p = qu_js_skip_string(p);
    m->keyLen = (int)(p - 1 - m->key);
    if (m->keyLen < 0)
        return NULL;
    while (*p && *p != ':')
        p++;
    if (*p)
        p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    m->val    = p;
    m->valEnd = qu_js_skip_value(p);
    return m->valEnd;
}

static int qu_js_key_is(const QuMember* m, const char* key)
{
    return (int)strlen(key) == m->keyLen && strncmp(m->key, key, (size_t)m->keyLen) == 0;
}

/* The value as text: a string without its quotes and escapes, a number as
 * written, "" for null. Returns the length it needs, which may exceed outSz. */
static size_t qu_js_text(const QuMember* m, char* out, size_t outSz)
{
    const char* p = m->val;
    size_t      n = 0;

#define QU_PUT(ch) do { if (n + 1 < outSz) out[n] = (char)(ch); n++; } while (0)
    if (*p != '"')
    {
        const char* e = m->valEnd;

        while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
            e--;
        if (!(e - p == 4 && strncmp(p, "null", 4) == 0))
            for (; p < e; p++)
                QU_PUT(*p);
    }
    else
    {
        for (p++; *p && *p != '"'; p++)
        {
            unsigned v = 0;
            int      k;

            if (*p != '\\' || !p[1])
            {
                QU_PUT(*p);
                continue;
            }
            p++;
            switch (*p)
            {
                case 'n': QU_PUT('\n'); break;
                case 't': QU_PUT('\t'); break;
                case 'r': QU_PUT('\r'); break;
                case 'b': case 'f': break;
                case 'u':
                    for (k = 0; k < 4 && isxdigit((unsigned char)p[1]); k++, p++)
                        v = v * 16 + (unsigned)(isdigit((unsigned char)p[1]) ? p[1] - '0'
                                                : tolower((unsigned char)p[1]) - 'a' + 10);
                    if (v < 0x80)
                        QU_PUT(v);
                    else if (v < 0x800)
                    {
                        QU_PUT(0xC0 | (v >> 6));
                        QU_PUT(0x80 | (v & 0x3F));
                    }
                    else
                    {
                        QU_PUT(0xE0 | (v >> 12));
                        QU_PUT(0x80 | ((v >> 6) & 0x3F));
                        QU_PUT(0x80 | (v & 0x3F));
                    }
                    break;
                default: QU_PUT(*p); break;
            }
        }
    }
#undef QU_PUT
    if (outSz)
        out[n < outSz ? n : outSz - 1] = '\0';
    return n;
}

static char* qu_js_dup(const QuMember* m)
{
    size_t need = qu_js_text(m, NULL, 0);
    char*  s    = (char*)malloc(need + 1);

    if (s)
        qu_js_text(m, s, need + 1);
    return s;
}

/* The array value of a top-level member: the char after its '[', or NULL. */
static const char* qu_js_top_array(const char* json, const char* key)
{
    const char* p = strchr(json, '{');
    QuMember    m;

    if (p == NULL)
        return NULL;
    p++;
    while ((p = qu_js_member(p, &m)) != NULL)
        if (qu_js_key_is(&m, key))
            return *m.val == '[' ? m.val + 1 : NULL;
    return NULL;
}

/* The next object of an array: its '{', or NULL at the array's end. */
static const char* qu_js_next_object(const char* p)
{
    for (;;)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')
            p++;
        if (*p == '{')
            return p;
        if (*p == '\0' || *p == ']')
            return NULL;
        p = qu_js_skip_value(p);
    }
}

/* ------------------------------------------------------------------ */
/* The catalog and what the launcher installed                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    char      id[64];
    char      name[96];
    char      file[96];
    char      url[320];
    char      sha256[72];
    long long size;
    char*     verify; /* "crc32 size path" lines, or NULL */
} QuCat;

typedef struct
{
    char name[128]; /* the folder under mods/ */
    char essentialId[64];
    char essentialSha[72];
    int  enabled;
} QuInst;

typedef struct
{
    QuCat  cat;
    QuInst inst;
    int    ok;
    int    deployed;    /* files under gamedata/ changed */
    int    needRestart; /* some of them are not text the game can read again */
    int    needApply;   /* switched on, yet nothing of it could be replaced under gamedata/ */
    char   err[200];
} QuJob;

static QuJob s_jobs[QU_MAX_MODS];
static int   s_jobCount;

static int qu_parse_catalog(const char* json, QuCat* out, int max)
{
    const char* p = qu_js_top_array(json, "mods");
    int         n = 0;

    while (p != NULL && n < max && (p = qu_js_next_object(p)) != NULL)
    {
        const char* end = qu_js_skip_value(p);
        const char* q   = p + 1;
        QuMember    m;
        QuCat       c;
        char        num[32];

        memset(&c, 0, sizeof(c));
        while ((q = qu_js_member(q, &m)) != NULL && q <= end)
        {
            if (qu_js_key_is(&m, "id"))          qu_js_text(&m, c.id, sizeof(c.id));
            else if (qu_js_key_is(&m, "name"))   qu_js_text(&m, c.name, sizeof(c.name));
            else if (qu_js_key_is(&m, "file"))   qu_js_text(&m, c.file, sizeof(c.file));
            else if (qu_js_key_is(&m, "url"))    qu_js_text(&m, c.url, sizeof(c.url));
            else if (qu_js_key_is(&m, "sha256")) qu_js_text(&m, c.sha256, sizeof(c.sha256));
            else if (qu_js_key_is(&m, "size"))
            {
                qu_js_text(&m, num, sizeof(num));
                c.size = atoll(num);
            }
            else if (qu_js_key_is(&m, "verify"))
            {
                free(c.verify);
                c.verify = qu_js_dup(&m);
            }
        }
        /* The file name becomes a path. */
        if (c.id[0] && c.url[0] && qu_safe_name(c.file))
            out[n++] = c;
        else
            free(c.verify);
        p = end;
    }
    return n;
}

static int qu_parse_installed(const char* json, QuInst* out, int max)
{
    const char* p = qu_js_top_array(json, "Mods");
    int         n = 0;

    while (p != NULL && n < max && (p = qu_js_next_object(p)) != NULL)
    {
        const char* end = qu_js_skip_value(p);
        const char* q   = p + 1;
        QuMember    m;
        QuInst      in;
        char        lit[16];

        memset(&in, 0, sizeof(in));
        while ((q = qu_js_member(q, &m)) != NULL && q <= end)
        {
            if (qu_js_key_is(&m, "Name"))              qu_js_text(&m, in.name, sizeof(in.name));
            else if (qu_js_key_is(&m, "EssentialId"))  qu_js_text(&m, in.essentialId, sizeof(in.essentialId));
            else if (qu_js_key_is(&m, "EssentialSha")) qu_js_text(&m, in.essentialSha, sizeof(in.essentialSha));
            else if (qu_js_key_is(&m, "Enabled"))
            {
                qu_js_text(&m, lit, sizeof(lit));
                in.enabled = strcmp(lit, "true") == 0;
            }
        }
        if (in.essentialId[0] && qu_safe_name(in.name))
            out[n++] = in;
        p = end;
    }
    return n;
}

/* Where a mod folder keeps what goes into gamedata/load: its load/ folder, at
 * the root or one folder down (a zip that wraps everything in one folder). */
static int qu_load_dir(const char* modDir, char* out, size_t outSz)
{
    DIR*           d;
    struct dirent* e;

    snprintf(out, outSz, "%s/load", modDir);
    if (qu_is_dir(out))
        return 1;
    if ((d = opendir(modDir)) == NULL)
        return 0;
    while ((e = readdir(d)) != NULL)
    {
        if (e->d_name[0] == '.')
            continue;
        snprintf(out, outSz, "%s/%s/load", modDir, e->d_name);
        if (qu_is_dir(out))
        {
            closedir(d);
            return 1;
        }
    }
    closedir(d);
    return 0;
}

/* The launcher deploys these by other rules (DLLs from maps/, videos flat into
 * gamedata/FMV), so a mod holding any is the launcher's to update. */
static int qu_launcher_only_file(const char* rel)
{
    static const char* const ext[] = { ".dll", ".avi", ".mp4", ".mkv", ".webm", ".mov", ".m4v" };
    const char*              s     = rel;
    size_t                   i;

    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (qu_iends(rel, ext[i]))
            return 1;
    while (*s)
    {
        const char* e = strchr(s, '/');
        size_t      n = e ? (size_t)(e - s) : 0;

        if (e == NULL)
            break;
        if ((n == 4 && qu_iprefix(s, "maps")) || (n == 7 && qu_iprefix(s, "plugins")))
            return 1;
        s = e + 1;
    }
    return 0;
}

/* A library mod this code may update: a folder under mods/ with a load/
 * folder and nothing the launcher deploys by other rules. */
static int qu_updatable(const char* modDir)
{
    QuList files = { NULL, 0, 0 };
    char   load[QU_PATH];
    int    i, ok;

    if (!qu_is_dir(modDir) || !qu_load_dir(modDir, load, sizeof(load)))
        return 0;
    ok = qu_collect(modDir, "", &files, 0);
    for (i = 0; ok && i < files.n; i++)
        if (qu_launcher_only_file(files.v[i]))
            ok = 0;
    qu_list_free(&files);
    return ok;
}

/* The catalog's "verify" lines against the files under modDir.
 * 1 = every file is the catalog's, 0 = one differs or is missing,
 * -1 = no usable line (the caller falls back on the recorded sha256). */
static int qu_verify(const char* modDir, const char* verify)
{
    const char* p     = verify;
    int         lines = 0;

    while (p && *p)
    {
        const char* e = strchr(p, '\n');
        size_t      n = e ? (size_t)(e - p) : strlen(p);
        char        line[QU_PATH], rel[QU_PATH], path[QU_PATH];
        unsigned    wantCrc = 0, crc = 0;
        long long   wantSize = 0, size = 0;
        int         used = 0;

        if (n > 0 && n < sizeof(line))
        {
            memcpy(line, p, n);
            line[n] = '\0';
            if (n && line[n - 1] == '\r')
                line[n - 1] = '\0';
            if (sscanf(line, "%8x %lld %n", &wantCrc, &wantSize, &used) >= 2 && used > 0 && line[used])
            {
                snprintf(rel, sizeof(rel), "%s", line + used);
                if (!qu_safe_rel(rel))
                    return -1;
                lines++;
                snprintf(path, sizeof(path), "%s/%s", modDir, rel);
                if (!qu_crc_file(path, &size, &crc))
                {
                    /* One folder down, as qu_load_dir looks. */
                    DIR*           d = opendir(modDir);
                    struct dirent* de;
                    int            found = 0;

                    while (d && !found && (de = readdir(d)) != NULL)
                    {
                        if (de->d_name[0] == '.')
                            continue;
                        snprintf(path, sizeof(path), "%s/%s/%s", modDir, de->d_name, rel);
                        found = qu_crc_file(path, &size, &crc);
                    }
                    if (d)
                        closedir(d);
                    if (!found)
                        return 0;
                }
                if (size != wantSize || crc != wantCrc)
                    return 0;
            }
        }
        p = e ? e + 1 : p + n;
    }
    return lines > 0 ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* mods/deployed.txt                                                   */
/* ------------------------------------------------------------------ */

typedef struct
{
    char*       raw;   /* the line, without its line end */
    int         owned; /* raw was rebuilt here */
    int         drop;
    char        op;    /* 'F' created by the Mod Manager, 'B' over a backed-up file */
    const char* rel;   /* inside raw; NULL for a line this code does not read */
} QuManLine;

typedef struct
{
    char*      text;
    QuManLine* lines;
    int        count, cap, dirty;
} QuManifest;

static void qu_man_free(QuManifest* m)
{
    int i;
    for (i = 0; i < m->count; i++)
        if (m->lines[i].owned)
            free(m->lines[i].raw);
    free(m->lines);
    free(m->text);
    memset(m, 0, sizeof(*m));
}

static QuManLine* qu_man_push(QuManifest* m)
{
    if (m->count == m->cap)
    {
        int        cap = m->cap ? m->cap * 2 : 256;
        QuManLine* l   = (QuManLine*)realloc(m->lines, (size_t)cap * sizeof(QuManLine));
        if (l == NULL)
            return NULL;
        m->lines = l;
        m->cap   = cap;
    }
    memset(&m->lines[m->count], 0, sizeof(QuManLine));
    return &m->lines[m->count++];
}

/* "F|size|ticks|rel", or the older "F|rel". */
static void qu_man_parse(QuManLine* l)
{
    const char* rest;
    const char* a;
    const char* b;

    l->op  = 0;
    l->rel = NULL;
    if (strlen(l->raw) < 3 || l->raw[1] != '|')
        return;
    rest = l->raw + 2;
    a    = strchr(rest, '|');
    b    = a ? strchr(a + 1, '|') : NULL;
    if (a && b && a > rest && strspn(rest, "0123456789-") == (size_t)(a - rest) &&
        strspn(a + 1, "0123456789-") == (size_t)(b - a - 1))
        l->rel = b + 1;
    else
        l->rel = rest;
    if (l->rel[0] == '\0')
        l->rel = NULL;
    else
        l->op = l->raw[0];
}

static int qu_man_load(QuManifest* m)
{
    char* p;

    memset(m, 0, sizeof(*m));
    m->text = qu_read_all(QU_MANIFEST, 64LL * 1024 * 1024, NULL);
    if (m->text == NULL)
        return 0;
    p = m->text;
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (*p)
    {
        char*      e = strchr(p, '\n');
        char*      next = e ? e + 1 : p + strlen(p);
        QuManLine* l;

        if (e)
            *e = '\0';
        if (p[0] && p[strlen(p) - 1] == '\r')
            p[strlen(p) - 1] = '\0';
        if (p[0])
        {
            if ((l = qu_man_push(m)) == NULL)
                return 0;
            l->raw = p;
            qu_man_parse(l);
        }
        p = next;
    }
    return 1;
}

static QuManLine* qu_man_find(QuManifest* m, const char* rel)
{
    int i;
    for (i = 0; i < m->count; i++)
        if (!m->lines[i].drop && m->lines[i].rel && qu_path_eq(m->lines[i].rel, rel))
            return &m->lines[i];
    return NULL;
}

/* Record what now sits at `full` under `rel` (an existing line, or a new one). */
static int qu_man_set(QuManifest* m, QuManLine* l, char op, const char* rel, const char* full)
{
    long long size = 0, ticks = 0;
    char*     raw;
    size_t    cap = strlen(rel) + 64;

    if (!PcFile_Stat(full, &size, &ticks) || (raw = (char*)malloc(cap)) == NULL)
        return 0;
    snprintf(raw, cap, "%c|%lld|%lld|%s", op, size, ticks, rel);
    if (l == NULL && (l = qu_man_push(m)) == NULL)
    {
        free(raw);
        return 0;
    }
    if (l->owned)
        free(l->raw);
    l->raw   = raw;
    l->owned = 1;
    l->drop  = 0;
    qu_man_parse(l);
    m->dirty = 1;
    return 1;
}

static int qu_man_save(QuManifest* m)
{
    size_t len = 0, at = 0;
    char*  out;
    int    i, ok;

    for (i = 0; i < m->count; i++)
        if (!m->lines[i].drop)
            len += strlen(m->lines[i].raw) + 2;
    if ((out = (char*)malloc(len + 1)) == NULL)
        return 0;
    for (i = 0; i < m->count; i++)
    {
        size_t n;
        if (m->lines[i].drop)
            continue;
        n = strlen(m->lines[i].raw);
        memcpy(out + at, m->lines[i].raw, n);
        at += n;
        out[at++] = '\r';
        out[at++] = '\n';
    }
    ok = qu_write_all(QU_MANIFEST, out, at);
    free(out);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Where the Mod Manager deploys a mod's files                         */
/* ------------------------------------------------------------------ */

/* TextOverrideTag's name part: the mod's folder name, lowered, anything but a
 * letter or digit turned into '_', trimmed, at most 40 long. */
static void qu_slug(const char* name, char* out, size_t outSz)
{
    char   buf[160];
    size_t n = 0, s = 0, e;

    for (; *name && n < sizeof(buf) - 1; name++)
        buf[n++] = isalnum((unsigned char)*name) ? (char)tolower((unsigned char)*name) : '_';
    buf[n] = '\0';
    while (buf[s] == '_')
        s++;
    e = n;
    while (e > s && buf[e - 1] == '_')
        e--;
    buf[e] = '\0';
    if (e - s > 40)
        buf[s + 40] = '\0';
    snprintf(out, outSz, "%s", e > s ? buf + s : "mod");
}

static int qu_is_text_override(const char* rel)
{
    return qu_ieq(rel, "text_overrides.txt") ||
           (qu_iprefix(rel, "text_overrides/") && qu_iends(rel, ".txt"));
}

/* "_name" for text_overrides/name.txt, "" for text_overrides.txt: what follows
 * the tag in the deployed file's name (RemapTextOverride). */
static void qu_override_leaf(const char* rel, char* out, size_t outSz)
{
    const char* base = strrchr(rel, '/');

    out[0] = '\0';
    if (base == NULL)
        return;
    base++;
    snprintf(out, outSz, "_%.*s", (int)(strlen(base) - 4), base);
}

/* A mod's text files are deployed as text_overrides/<NNN>_<slug><leaf>.txt,
 * NNN being its place in the Mod Manager's list. That place is not known
 * here, so it is read off a file already deployed. */
static int qu_find_tag(QuManifest* man, const char* slug, const QuList* oldFiles, char* tag, size_t tagSz)
{
    const size_t pre = strlen(QU_LOAD_REL "text_overrides\\");
    int          i, k;

    for (i = 0; i < oldFiles->n; i++)
    {
        char leaf[200], want[300];

        if (!qu_is_text_override(oldFiles->v[i]))
            continue;
        qu_override_leaf(oldFiles->v[i], leaf, sizeof(leaf));
        snprintf(want, sizeof(want), "_%s%s.txt", slug, leaf);
        for (k = 0; k < man->count; k++)
        {
            const char* rel = man->lines[k].rel;

            if (rel == NULL || man->lines[k].drop || strlen(rel) <= pre + 3)
                continue;
            if (!qu_iprefix(rel, QU_LOAD_REL "text_overrides\\") &&
                !qu_iprefix(rel, "gamedata/load/text_overrides/"))
                continue;
            rel += pre;
            if (isdigit((unsigned char)rel[0]) && isdigit((unsigned char)rel[1]) &&
                isdigit((unsigned char)rel[2]) && qu_ieq(rel + 3, want))
            {
                snprintf(tag, tagSz, "%.3s_%s", rel, slug);
                return 1;
            }
        }
    }
    return 0;
}

/* The path under gamedata/load a library file is deployed to, '/' between
 * parts. 0 for a text file when the mod's tag is not known. */
static int qu_deploy_rel(const char* rel, const char* tag, char* out, size_t outSz)
{
    char leaf[200];

    if (!qu_is_text_override(rel))
    {
        snprintf(out, outSz, "%s", rel);
        return 1;
    }
    if (tag == NULL || tag[0] == '\0')
        return 0;
    qu_override_leaf(rel, leaf, sizeof(leaf));
    snprintf(out, outSz, "text_overrides/%s%s.txt", tag, leaf);
    return 1;
}

/* deployed.txt names files from the game folder, with backslashes. */
static void qu_manifest_rel(const char* deployRel, char* out, size_t outSz)
{
    char* c;

    snprintf(out, outSz, QU_LOAD_REL "%s", deployRel);
    for (c = out; *c; c++)
        if (*c == '/')
            *c = '\\';
}

/* The Mod Manager's SameFile: equal size, last-write times within 2 s. */
static int qu_same_file(const char* a, const char* b)
{
    long long sa, sb, ta, tb, dt;

    if (!PcFile_Stat(a, &sa, &ta) || !PcFile_Stat(b, &sb, &tb) || sa != sb)
        return 0;
    dt = ta > tb ? ta - tb : tb - ta;
    return dt < QU_SAME_TICKS;
}

/* ------------------------------------------------------------------ */
/* mods/modmanager.json: the sha256 the mod was installed from         */
/* ------------------------------------------------------------------ */

static int qu_state_set_sha(const char* name, const char* sha)
{
    size_t      len = 0;
    char*       json = qu_read_all(QU_STATE_FILE, 8LL * 1024 * 1024, &len);
    const char* p;
    int         ok = 0;

    if (json == NULL)
        return 0;
    p = qu_js_top_array(json, "Mods");
    while (p != NULL && (p = qu_js_next_object(p)) != NULL)
    {
        const char* end = qu_js_skip_value(p);
        const char* q   = p + 1;
        const char* nameKey = NULL;
        const char* shaVal = NULL;
        const char* shaEnd = NULL;
        QuMember    m;
        char        got[128];
        int         mine = 0;

        while ((q = qu_js_member(q, &m)) != NULL && q <= end)
        {
            if (qu_js_key_is(&m, "Name"))
            {
                qu_js_text(&m, got, sizeof(got));
                mine    = strcmp(got, name) == 0;
                nameKey = m.key - 1;
            }
            else if (qu_js_key_is(&m, "EssentialSha"))
            {
                shaVal = m.val;
                shaEnd = m.valEnd;
            }
        }
        if (mine)
        {
            /* Replace the value, or add the member ahead of "Name" in a file
             * from a launcher that did not write one. */
            const char* cutA = shaVal ? shaVal : nameKey;
            const char* cutB = shaVal ? shaEnd : nameKey;
            char        ins[160];
            size_t      insLen, head = (size_t)(cutA - json), tail = len - (size_t)(cutB - json);
            char*       out;

            if (shaVal)
            {
                /* A number or null runs up to the comma; keep any blank after it. */
                while (cutB > cutA && (cutB[-1] == ' ' || cutB[-1] == '\r' || cutB[-1] == '\n' || cutB[-1] == '\t'))
                    cutB--;
                tail = len - (size_t)(cutB - json);
                snprintf(ins, sizeof(ins), "\"%s\"", sha);
            }
            else
                snprintf(ins, sizeof(ins), "\"EssentialSha\":\"%s\",", sha);
            insLen = strlen(ins);
            if ((out = (char*)malloc(head + insLen + tail + 1)) != NULL)
            {
                memcpy(out, json, head);
                memcpy(out + head, ins, insLen);
                memcpy(out + head + insLen, cutB, tail);
                ok = qu_write_all(QU_STATE_FILE, out, head + insLen + tail);
                free(out);
            }
            break;
        }
        p = end;
    }
    free(json);
    return ok;
}

/* ------------------------------------------------------------------ */
/* The work, on a thread of its own                                    */
/* ------------------------------------------------------------------ */

enum { QW_IDLE = 0, QW_RUNNING, QW_FINISHED };
enum { QK_CHECK = 0, QK_APPLY };
static volatile int       s_workState;
static int                s_workKind;
static int                s_checkOk;     /* the catalog was read and compared */
static volatile int       s_jobPos;      /* QK_APPLY: the mod being worked on */
static volatile int       s_jobUnpack;   /* its download is done */
static volatile long long s_dlGot;

/* 200 with the file at `path`, else an HTTP status or 0. */
static int qu_fetch(const char* file, const char* url, const char* path)
{
    remove(path);
    if (qu_is_dir(QU_TEST_DIR))
    {
        char from[QU_PATH];

        snprintf(from, sizeof(from), QU_TEST_DIR "/%s", file);
        return PcFile_CopyKeepTime(from, path) ? 200 : 404;
    }
    /* The catalog names its own downloads; only this release is followed. */
    if (strncmp(url, QU_RELEASE_BASE, strlen(QU_RELEASE_BASE)) != 0)
        return 0;
    return Pc_RaHttpDownloadFile(url, path, &s_dlGot);
}

/* A character pack the game has replaced with its Additional Characters
 * downloads (pc_chara_dl.c lists it in this file). The launcher drops such a
 * pack from its library the next time it runs; until then it must not be
 * offered as an update. */
static int qu_migrated(const char* essentialId)
{
    char* txt = qu_read_all("gamedata/chara_migrated.txt", 1 << 20, NULL);
    char* line;
    int   found = 0;

    if (txt == NULL)
        return 0;
    for (line = strtok(txt, "\r\n"); line != NULL && !found; line = strtok(NULL, "\r\n"))
        found = qu_ieq(line, essentialId);
    free(txt);
    return found;
}

/* Fills s_jobs with the installed mods that are out of date. 0 when the
 * catalog could not be read, so the build is not marked as checked. */
static int qu_check(void)
{
    static QuCat  cat[QU_MAX_MODS];
    static QuInst inst[QU_MAX_MODS];
    char*         json;
    int           catCount, instCount, i, k;

    s_jobCount = 0;
    json = qu_read_all(QU_STATE_FILE, 8LL * 1024 * 1024, NULL);
    if (json == NULL)
        return 1;
    instCount = qu_parse_installed(json, inst, QU_MAX_MODS);
    free(json);
    if (instCount == 0)
        return 1;

    qu_mkdir("mods");
    qu_mkdir(QU_WORK_DIR);
    if (qu_fetch(QU_CATALOG_FILE, QU_RELEASE_BASE QU_CATALOG_FILE, QU_WORK_DIR "/" QU_CATALOG_FILE) != 200)
    {
        qu_rmtree(QU_WORK_DIR);
        return 0;
    }
    json = qu_read_all(QU_WORK_DIR "/" QU_CATALOG_FILE, 4LL * 1024 * 1024, NULL);
    qu_rmtree(QU_WORK_DIR);
    if (json == NULL)
        return 0;
    catCount = qu_parse_catalog(json, cat, QU_MAX_MODS);
    free(json);
    if (catCount == 0)
        return 0;

    for (i = 0; i < instCount; i++)
    {
        char dir[QU_PATH];
        int  v, stale;

        for (k = 0; k < catCount; k++)
            if (qu_ieq(cat[k].id, inst[i].essentialId))
                break;
        if (k == catCount || qu_migrated(inst[i].essentialId))
            continue;
        snprintf(dir, sizeof(dir), "mods/%s", inst[i].name);
        if (!qu_updatable(dir))
            continue;
        v = cat[k].verify ? qu_verify(dir, cat[k].verify) : -1;
        if (v >= 0)
            stale = !v;
        else
            stale = inst[i].essentialSha[0] && cat[k].sha256[0] && !qu_ieq(inst[i].essentialSha, cat[k].sha256);
        if (!stale)
            continue;
        memset(&s_jobs[s_jobCount], 0, sizeof(QuJob));
        s_jobs[s_jobCount].cat  = cat[k];
        s_jobs[s_jobCount].inst = inst[i];
        cat[k].verify           = NULL; /* the job owns it now */
        s_jobCount++;
    }
    for (k = 0; k < catCount; k++)
        free(cat[k].verify);
    return 1;
}

typedef struct
{
    char rel[QU_PATH]; /* as deployed.txt names it */
    char op;
    int  kept;
} QuOwned;

static int qu_extract(const char* zipPath, const char* destDir, char* err, size_t errSz)
{
    mz_zip_archive zip;
    mz_uint        i, n;
    int            ok = 1, files = 0;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath, 0))
    {
        snprintf(err, errSz, "the download is not a valid zip");
        return 0;
    }
    n = mz_zip_reader_get_num_files(&zip);
    for (i = 0; ok && i < n; i++)
    {
        char name[QU_PATH], dest[QU_PATH];

        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        if (!qu_safe_rel(name))
        {
            snprintf(err, errSz, "the zip holds a path that leaves its folder");
            ok = 0;
        }
        else if (qu_launcher_only_file(name))
        {
            snprintf(err, errSz, "this version has to be installed from the launcher's Mod Manager");
            ok = 0;
        }
        else
        {
            snprintf(dest, sizeof(dest), "%s/%s", destDir, name);
            qu_mkdirs_for(dest);
            if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0))
            {
                snprintf(err, errSz, "could not unpack %s", name);
                ok = 0;
            }
            files++;
        }
    }
    mz_zip_reader_end(&zip);
    if (ok && files == 0)
    {
        snprintf(err, errSz, "the zip is empty");
        ok = 0;
    }
    return ok;
}

static void qu_apply(QuJob* j)
{
    char       zipPath[QU_PATH], newDir[QU_PATH], libDir[QU_PATH], oldLoad[QU_PATH], newLoad[QU_PATH];
    char       slug[64], tag[80];
    QuList     oldFiles = { NULL, 0, 0 }, newFiles = { NULL, 0, 0 };
    QuManifest man;
    QuOwned*   owned = NULL;
    int        ownedCount = 0, deploy, status, i, k;
    long long  size = 0;

    memset(&man, 0, sizeof(man));
    tag[0] = '\0';
    snprintf(libDir, sizeof(libDir), "mods/%s", j->inst.name);
    snprintf(zipPath, sizeof(zipPath), QU_WORK_DIR "/%s", j->cat.file);
    snprintf(newDir, sizeof(newDir), QU_WORK_DIR "/%s.new", j->inst.name);
    qu_mkdir("mods");
    qu_mkdir(QU_WORK_DIR);

    s_dlGot     = 0;
    s_jobUnpack = 0;
    status = qu_fetch(j->cat.file, j->cat.url, zipPath);
    if (status != 200)
    {
        if (status)
            snprintf(j->err, sizeof(j->err), "the server answered %d", status);
        else
            snprintf(j->err, sizeof(j->err), "no connection");
        goto done;
    }
    s_jobUnpack = 1;
    if (j->cat.size > 0 && (!PcFile_Stat(zipPath, &size, NULL) || size != j->cat.size))
    {
        snprintf(j->err, sizeof(j->err), "the download was incomplete");
        goto done;
    }

    qu_rmtree(newDir);
    if (!qu_extract(zipPath, newDir, j->err, sizeof(j->err)))
        goto done;
    if (!qu_load_dir(newDir, newLoad, sizeof(newLoad)))
    {
        snprintf(j->err, sizeof(j->err), "there is nothing in it for gamedata/load");
        goto done;
    }
    /* The files just unpacked are the ones the catalog describes, or nothing
     * is touched. */
    if (j->cat.verify && qu_verify(newDir, j->cat.verify) == 0)
    {
        snprintf(j->err, sizeof(j->err), "the download does not match the mod list");
        goto done;
    }

    /* Which deployed files are this mod's own, while the copy they came from
     * is still there to compare with. */
    deploy = j->inst.enabled && qu_load_dir(libDir, oldLoad, sizeof(oldLoad)) &&
             qu_collect(oldLoad, "", &oldFiles, 0) && qu_man_load(&man);
    if (deploy)
    {
        qu_slug(j->inst.name, slug, sizeof(slug));
        qu_find_tag(&man, slug, &oldFiles, tag, sizeof(tag));
        owned = (QuOwned*)calloc((size_t)oldFiles.n + 1, sizeof(QuOwned));
        for (i = 0; owned && i < oldFiles.n; i++)
        {
            char       rel[QU_PATH], mrel[QU_PATH], lib[QU_PATH], dst[QU_PATH];
            QuManLine* l;

            if (!qu_deploy_rel(oldFiles.v[i], tag, rel, sizeof(rel)))
                continue;
            qu_manifest_rel(rel, mrel, sizeof(mrel));
            snprintf(lib, sizeof(lib), "%s/%s", oldLoad, oldFiles.v[i]);
            snprintf(dst, sizeof(dst), "gamedata/load/%s", rel);
            l = qu_man_find(&man, mrel);
            if (l == NULL || (l->op != 'F' && l->op != 'B') || !qu_same_file(lib, dst))
                continue;
            snprintf(owned[ownedCount].rel, sizeof(owned[ownedCount].rel), "%s", mrel);
            owned[ownedCount].op = l->op;
            ownedCount++;
        }
        /* Nothing of it is deployed (never applied, or every file outranked
         * by another mod): the library copy is all there is to update. */
        if (owned == NULL || ownedCount == 0)
            deploy = 0;
    }

    qu_rmtree(libDir);
    if (!PcFile_MoveReplace(newDir, libDir))
    {
        if (!qu_copy_tree(newDir, libDir))
        {
            snprintf(j->err, sizeof(j->err), "could not write mods/%s", j->inst.name);
            goto done;
        }
        qu_rmtree(newDir);
    }

    if (deploy && qu_load_dir(libDir, newLoad, sizeof(newLoad)) && qu_collect(newLoad, "", &newFiles, 0))
    {
        for (i = 0; i < newFiles.n; i++)
        {
            char       rel[QU_PATH], mrel[QU_PATH], lib[QU_PATH], dst[QU_PATH];
            QuManLine* l;
            char       op = 0;

            if (!qu_deploy_rel(newFiles.v[i], tag, rel, sizeof(rel)))
                continue;
            qu_manifest_rel(rel, mrel, sizeof(mrel));
            snprintf(lib, sizeof(lib), "%s/%s", newLoad, newFiles.v[i]);
            snprintf(dst, sizeof(dst), "gamedata/load/%s", rel);
            for (k = 0; k < ownedCount; k++)
                if (qu_path_eq(owned[k].rel, mrel))
                {
                    owned[k].kept = 1;
                    op            = owned[k].op;
                }
            l = qu_man_find(&man, mrel);
            if (op == 0)
            {
                /* A file this version adds. One already there is another
                 * mod's or the player's, and the Mod Manager's to settle. */
                if (l != NULL || PcFile_Stat(dst, NULL, NULL))
                    continue;
                op = 'F';
            }
            qu_mkdirs_for(dst);
            if (!PcFile_CopyKeepTime(lib, dst) || !qu_man_set(&man, l, op, mrel, dst))
            {
                snprintf(j->err, sizeof(j->err), "could not write %s", rel);
                continue;
            }
            j->deployed = 1;
            if (!qu_is_text_override(newFiles.v[i]))
                j->needRestart = 1;
        }
        /* A file this version no longer has. One the Mod Manager created is
         * removed, as its own Apply would; one over a backup is left for it. */
        for (k = 0; k < ownedCount; k++)
        {
            char       dst[QU_PATH];
            char*      c;
            QuManLine* l;

            if (owned[k].kept || owned[k].op != 'F')
                continue;
            snprintf(dst, sizeof(dst), "%s", owned[k].rel);
            for (c = dst; *c; c++)
                if (*c == '\\')
                    *c = '/';
            remove(dst);
            if ((l = qu_man_find(&man, owned[k].rel)) != NULL)
            {
                l->drop   = 1;
                man.dirty = 1;
            }
            j->deployed    = 1;
            j->needRestart = 1;
        }
        if (man.dirty && !qu_man_save(&man) && !j->err[0])
            snprintf(j->err, sizeof(j->err), "could not write " QU_MANIFEST);
    }

    if (!j->err[0])
    {
        if (j->cat.sha256[0])
            qu_state_set_sha(j->inst.name, j->cat.sha256);
        j->needApply = j->inst.enabled && !j->deployed;
        j->ok        = 1;
    }

done:
    remove(zipPath);
    qu_rmtree(newDir);
    qu_list_free(&oldFiles);
    qu_list_free(&newFiles);
    qu_man_free(&man);
    free(owned);
}

static int SDLCALL qu_worker(void* unused)
{
    int i;

    (void)unused;
    if (s_workKind == QK_CHECK)
        s_checkOk = qu_check();
    else
    {
        for (i = 0; i < s_jobCount; i++)
        {
            s_jobPos = i;
            qu_apply(&s_jobs[i]);
        }
        qu_rmtree(QU_WORK_DIR);
    }
    s_workState = QW_FINISHED;
    return 0;
}

static int qu_start(int kind)
{
    SDL_Thread* t;

    s_workKind  = kind;
    s_jobPos    = 0;
    s_workState = QW_RUNNING;
    t = SDL_CreateThread(qu_worker, "SH_QOLUPD", NULL);
    if (t == NULL)
    {
        s_workState = QW_IDLE;
        return 0;
    }
    SDL_DetachThread(t);
    return 1;
}

/* ------------------------------------------------------------------ */
/* The title screen                                                    */
/* ------------------------------------------------------------------ */

enum { QS_WAIT = 0, QS_CHECKING, QS_PROMPT, QS_APPLYING, QS_NOTICE, QS_OFF };
static int    s_state;
static int    s_titleFrames;
static Uint32 s_lastProgressMs;

static void qu_format_size(char* out, size_t sz, long long bytes)
{
    if (bytes < 1024 * 1024)
        snprintf(out, sz, "%lld KB", bytes < 1024 ? 1 : bytes / 1024);
    else
        snprintf(out, sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

static void qu_free_jobs(void)
{
    int i;
    for (i = 0; i < s_jobCount; i++)
    {
        free(s_jobs[i].cat.verify);
        s_jobs[i].cat.verify = NULL;
    }
}

static void qu_open_prompt(void)
{
    char      msg[480], names[300], size[32];
    long long total = 0;
    int       i;

    names[0] = '\0';
    for (i = 0; i < s_jobCount; i++)
    {
        size_t n = strlen(names);

        total += s_jobs[i].cat.size;
        if (i < 3)
            snprintf(names + n, sizeof(names) - n, "%s%s", n ? ", " : "", s_jobs[i].cat.name);
    }
    if (s_jobCount > 3)
    {
        size_t n = strlen(names);
        snprintf(names + n, sizeof(names) - n, " and %d more", s_jobCount - 3);
    }
    qu_format_size(size, sizeof(size), total);
    if (s_jobCount == 1)
        snprintf(msg, sizeof(msg), "An update is available for %s (%s). Download it now?", names, size);
    else
        snprintf(msg, sizeof(msg), "Updates are available for %d of your mods: %s (%s). Download them now?",
                 s_jobCount, names, size);
    Pc_ConfirmDialog_Open3(QU_TITLE, msg, "Don't ask again");
}

static void qu_progress_text(char* msg, size_t sz)
{
    const QuJob* j = &s_jobs[s_jobPos < s_jobCount ? s_jobPos : s_jobCount - 1];
    char         got[32], tot[32], count[32];

    count[0] = '\0';
    if (s_jobCount > 1)
        snprintf(count, sizeof(count), " (%d of %d)", s_jobPos + 1, s_jobCount);
    if (s_jobUnpack)
        snprintf(msg, sz, "Installing %s%s...\nKeep the game open until this finishes.", j->cat.name, count);
    else
    {
        qu_format_size(got, sizeof(got), s_dlGot);
        qu_format_size(tot, sizeof(tot), j->cat.size);
        snprintf(msg, sz, "Downloading %s%s: %s of %s\nKeep the game open until this finishes.",
                 j->cat.name, count, got, tot);
    }
}

static void qu_open_result(void)
{
    char   msg[480];
    size_t n;
    int    i, done = 0, failed = 0, restart = 0, deployed = 0, apply = 0;

    for (i = 0; i < s_jobCount; i++)
    {
        const QuJob* j = &s_jobs[i];

        if (j->ok)
        {
            done++;
            restart  |= j->needRestart;
            deployed |= j->deployed;
            apply    |= j->needApply;
            SH_DBG("[QOLUPD] %s updated%s", j->cat.name,
                   j->deployed ? "" : j->needApply ? " (mods/ copy only: its deployed files were not its own)"
                                                   : " (mods/ copy only: it is switched off)");
        }
        else
        {
            failed++;
            SH_DBG("[QOLUPD] %s failed: %s", j->cat.name, j->err);
        }
    }
    if (deployed)
    {
        extern void Pc_TextOverridesReload(void);
        Pc_TextOverridesReload();
    }
    msg[0] = '\0';
    if (done == 1 && s_jobCount == 1)
        snprintf(msg, sizeof(msg), "%s was updated.", s_jobs[0].cat.name);
    else if (done > 0)
        snprintf(msg, sizeof(msg), "%d mod%s updated.", done, done == 1 ? " was" : "s were");
    if (done > 0 && apply)
    {
        /* The files in gamedata/ were not the ones the Mod Manager put there
         * (or it never did), so only its Apply can put the new ones in. */
        n = strlen(msg);
        snprintf(msg + n, sizeof(msg) - n, " Open the launcher's Mod Manager and press Apply to finish.");
    }
    else if (done > 0 && restart)
    {
        n = strlen(msg);
        snprintf(msg + n, sizeof(msg) - n, " Restart the game to use %s.", done == 1 ? "it" : "them");
    }
    for (i = 0; i < s_jobCount; i++)
    {
        if (s_jobs[i].ok)
            continue;
        n = strlen(msg);
        snprintf(msg + n, sizeof(msg) - n, "%sCould not update %s: %s.", n ? "\n" : "", s_jobs[i].cat.name,
                 s_jobs[i].err);
    }
    if (failed)
    {
        n = strlen(msg);
        snprintf(msg + n, sizeof(msg) - n, "\nIt will be offered again the next time the game starts.");
    }
    else
        PcConfig_SaveKeyValue(QU_CHECKED_KEY, SH_BUILD_GIT_HASH);
    Pc_ConfirmDialog_OpenOk(QU_TITLE, msg);
}

int Pc_QolUpdate_IsOpen(void)
{
    return s_state == QS_PROMPT || s_state == QS_APPLYING || s_state == QS_NOTICE;
}

int Pc_QolUpdate_TitleFrame(int left, int right, int confirm, int cancel)
{
    switch (s_state)
    {
        case QS_WAIT:
        {
            const char* v;
            struct stat st;

            /* A second on the title first, so nothing opens over its fade-in. */
            if (++s_titleFrames <= 60 || Pc_ConfirmDialog_IsOpen())
                return 0;
            s_state = QS_OFF;
            if ((v = Pc_ModConfig_Value(QU_ASK_KEY)) != NULL && v[0] == '0')
                return 0;
            if ((v = Pc_ModConfig_Value(QU_CHECKED_KEY)) != NULL && strcmp(v, SH_BUILD_GIT_HASH) == 0)
                return 0;
            if (stat(QU_STATE_FILE, &st) != 0)
                return 0;
            if (qu_start(QK_CHECK))
                s_state = QS_CHECKING;
            return 0;
        }

        case QS_CHECKING:
            if (s_workState != QW_FINISHED || Pc_ConfirmDialog_IsOpen())
                return 0;
            s_workState = QW_IDLE;
            if (!s_checkOk)
            {
                SH_DBG("[QOLUPD] the mod list could not be read; trying again next start");
                s_state = QS_OFF;
                return 0;
            }
            if (s_jobCount == 0)
            {
                SH_DBG("[QOLUPD] Essential QoL mods are up to date for build " SH_BUILD_GIT_HASH);
                PcConfig_SaveKeyValue(QU_CHECKED_KEY, SH_BUILD_GIT_HASH);
                s_state = QS_OFF;
                return 0;
            }
            SH_DBG("[QOLUPD] %d mod(s) out of date, first: %s", s_jobCount, s_jobs[0].cat.name);
            qu_open_prompt();
            s_state = QS_PROMPT;
            return 1;

        case QS_PROMPT:
            switch (Pc_ConfirmDialog_Update(left, right, confirm, cancel))
            {
                case PC_CONFIRM_YES:
                {
                    char msg[320];

                    s_dlGot     = 0;
                    s_jobUnpack = 0;
                    if (!qu_start(QK_APPLY))
                    {
                        qu_free_jobs();
                        s_state = QS_OFF;
                        break;
                    }
                    qu_progress_text(msg, sizeof(msg));
                    Pc_ConfirmDialog_OpenBusy(QU_TITLE, msg);
                    s_lastProgressMs = SDL_GetTicks();
                    s_state          = QS_APPLYING;
                    break;
                }
                case PC_CONFIRM_NO:
                    SH_DBG("[QOLUPD] not now; asking again next start");
                    qu_free_jobs();
                    s_state = QS_OFF;
                    break;
                case PC_CONFIRM_THIRD:
                    SH_DBG("[QOLUPD] don't ask again (" QU_ASK_KEY " = 0)");
                    PcConfig_SaveKeyValue(QU_ASK_KEY, "0");
                    qu_free_jobs();
                    s_state = QS_OFF;
                    break;
                default:
                    break;
            }
            return 1;

        case QS_APPLYING:
            if (s_workState == QW_FINISHED)
            {
                s_workState = QW_IDLE;
                Pc_ConfirmDialog_Cancel();
                qu_open_result();
                qu_free_jobs();
                s_state = QS_NOTICE;
            }
            else if (SDL_GetTicks() - s_lastProgressMs >= 250u)
            {
                char msg[320];

                s_lastProgressMs = SDL_GetTicks();
                qu_progress_text(msg, sizeof(msg));
                Pc_ConfirmDialog_SetMessage(msg);
            }
            return 1;

        case QS_NOTICE:
            if (Pc_ConfirmDialog_Update(left, right, confirm, cancel) != PC_CONFIRM_NONE)
                s_state = QS_OFF;
            return 1;

        default:
            return 0;
    }
}

#endif
