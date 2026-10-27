/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_qol_mods.c - Essential QoL Mods installer (Android, iOS, web).
 *
 * Builds without the desktop launcher have no Mod Manager, so players had to
 * find mods on Discord and copy files by hand. This lists the qol-mods GitHub
 * release (essential-mods.json, shared with the launcher), downloads the mods
 * ticked and installs them where the game reads mods:
 *   - texture packs (texupload-/texpage- PNGs) go to gamedata/texturemods/ as
 *     the zip itself, which tex_pack.c reads in place;
 *   - everything else is a load/ overlay, extracted into gamedata/load/
 *     (fsqueue reads loose files only, never from a zip), and loose-file
 *     support is switched on, which the web build defaults off.
 * Installed ids are kept in gamedata/qol_installed.txt. Unticking an
 * installed mod removes it: the catalog lists, per mod, the files it puts
 * under gamedata/, so removal needs neither the zip nor a record made at
 * install time (mods installed before removal existed have none).
 *
 * GL constraints are pc_rando_settings.c's: own program/VAO/textures with full
 * state save+restore, legacy GLSL, all GL work in Draw.
 *
 * Downloads: native builds run Pc_QolHttpDownload (pc_ra_http.c on Android,
 * ios_ra_http.m on iOS) and the install on a worker thread. The browser has no
 * threads, so there a fetch() streams the file into the virtual FS and the
 * install runs on the main thread when it completes. GitHub release downloads
 * send no CORS headers, so the browser fetches through the site's qolMods
 * function (web_port/firebase/functions), which relays this release only.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "stb_truetype.h"
#include "miniz.h"

#include "pc_qol_mods.h"
#include "pc_config.h"
#include "pc_mod_registry.h"
#include "pc_mouse_cursor.h"
#include "sh_log.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#define QM_RELEASE_BASE "https://github.com/SlickAmogus/silent-hill-decomp/releases/download/qol-mods/"
#define QM_CATALOG_FILE "essential-mods.json"
#if defined(__EMSCRIPTEN__)
#define QM_WEB_RELAY    "https://us-central1-sh1pc-8d53f.cloudfunctions.net/qolMods/"
#endif

#define QM_TMP_DIR      "gamedata/.qol"
#define QM_INSTALLED    "gamedata/qol_installed.txt"
#define QM_OFFERED_KEY  "qol_mods_offered"

#if defined(__ANDROID__)
#define QM_PLATFORM_OPTIONS "Android Options"
#elif defined(SH_IOS)
#define QM_PLATFORM_OPTIONS "iOS Options"
#elif defined(__EMSCRIPTEN__)
#define QM_PLATFORM_OPTIONS "Web Options"
#else
#define QM_PLATFORM_OPTIONS "PC Options"
#endif

#if defined(__EMSCRIPTEN__)
#define QM_RESTART_TEXT "Reload the page to use them."
#else
#define QM_RESTART_TEXT "Restart the game to use them."
#endif

/* ------------------------------------------------------------------ */
/* Catalog                                                             */
/* ------------------------------------------------------------------ */

#define QM_MAX_MODS 12

typedef struct
{
    char      id[48];
    char      name[96];
    char      author[160];
    char      desc[600];
    char      file[96];
    char      url[256];
    long long size;
    char*     files;     /* '\n'-separated paths under gamedata/, from the catalog */
    int       installed;
    int       want;      /* the ticked state: differs from installed = a pending change */
} s_QmMod;

static s_QmMod s_mods[QM_MAX_MODS];
static int     s_modCount;

/* ------------------------------------------------------------------ */
/* UI state                                                            */
/* ------------------------------------------------------------------ */

enum { QM_CLOSED = 0, QM_PROMPT, QM_NOTICE, QM_LOADING, QM_ERROR, QM_LIST, QM_INFO, QM_BUSY, QM_DONE };
static int    s_mode;
static Uint32 s_openMs;
static int    s_sel;          /* LIST: row index, then the buttons; elsewhere unused */
static int    s_btnSel;       /* selected button on the button bar */
static int    s_infoIdx;      /* QM_INFO: which mod */
static int    s_offeredThisRun;
static int    s_titleFrames;
static char   s_message[512]; /* QM_ERROR / QM_DONE body */

/* Busy sequence: the ticked mods, installed one after another. */
static int    s_queue[QM_MAX_MODS];
static int    s_queueLen, s_queuePos, s_installedNow, s_failedNow, s_removedNow;
static char   s_failNames[256];

/* ------------------------------------------------------------------ */
/* Files                                                               */
/* ------------------------------------------------------------------ */

static void qm_mkdir(const char* path)
{
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* mkdir -p for the directory part of a file path. */
static void qm_mkdirs_for(const char* filePath)
{
    char buf[512];
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
            qm_mkdir(buf);
            buf[i] = c;
        }
    }
}

static long long qm_file_size(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static void qm_tmp_path(char* out, size_t sz, const char* file)
{
    snprintf(out, sz, "%s/%s", QM_TMP_DIR, file);
}

static void qm_load_installed(void)
{
    char  line[128];
    FILE* f = fopen(QM_INSTALLED, "rb");
    int   i;
    for (i = 0; i < s_modCount; i++)
        s_mods[i].installed = 0;
    if (f)
    {
        while (fgets(line, sizeof(line), f))
        {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
                line[--n] = '\0';
            for (i = 0; i < s_modCount; i++)
                if (n && strcmp(line, s_mods[i].id) == 0)
                    s_mods[i].installed = 1;
        }
        fclose(f);
    }
    for (i = 0; i < s_modCount; i++)
        s_mods[i].want = s_mods[i].installed;
}

static void qm_mark_installed(const s_QmMod* m)
{
    FILE* f = fopen(QM_INSTALLED, "ab");
    if (f)
    {
        fprintf(f, "%s\n", m->id);
        fclose(f);
    }
}

static void qm_unmark_installed(const s_QmMod* m)
{
    char  line[128], keep[2048];
    size_t used = 0;
    FILE* f = fopen(QM_INSTALLED, "rb");
    keep[0] = '\0';
    if (!f)
        return;
    while (fgets(line, sizeof(line), f))
    {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = '\0';
        if (n && strcmp(line, m->id) != 0 && used + n + 2 < sizeof(keep))
        {
            memcpy(keep + used, line, n);
            used += n;
            keep[used++] = '\n';
            keep[used] = '\0';
        }
    }
    fclose(f);
    f = fopen(QM_INSTALLED, "wb");
    if (f)
    {
        fwrite(keep, 1, used, f);
        fclose(f);
    }
}

/* ------------------------------------------------------------------ */
/* JSON (the catalog is a flat list of string/number fields)           */
/* ------------------------------------------------------------------ */

static const char* js_ws(const char* p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/* p at the opening quote; returns past the closing one, or NULL. Non-ASCII
 * \u escapes become '?': the overlay font path is ASCII-only anyway. */
static const char* js_str(const char* p, char* out, size_t sz)
{
    size_t n = 0;
    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"')
    {
        char c = *p++;
        if (c == '\\' && *p)
        {
            char e = *p++;
            switch (e)
            {
                case 'n': c = '\n'; break;
                case 't': c = ' ';  break;
                case 'r': c = ' ';  break;
                case 'u':
                {
                    unsigned v = 0;
                    int k;
                    for (k = 0; k < 4 && isxdigit((unsigned char)*p); k++, p++)
                        v = v * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
                    c = (v < 0x80) ? (char)v : '?';
                    break;
                }
                default: c = e; break;
            }
        }
        if (n + 1 < sz)
            out[n++] = c;
    }
    if (sz)
        out[n] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

static int qm_parse_catalog(const char* json)
{
    const char* p = strstr(json, "\"mods\"");
    if (!p)
        return 0;
    p = strchr(p, '[');
    if (!p)
        return 0;
    p++;
    {
        int k;
        for (k = 0; k < s_modCount; k++)
        {
            free(s_mods[k].files);
            s_mods[k].files = NULL;
        }
    }
    s_modCount = 0;
    for (;;)
    {
        s_QmMod m;
        p = js_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p != '{')
            break;
        p++;
        memset(&m, 0, sizeof(m));
        for (;;)
        {
            char key[32], val[600];
            p = js_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            if (!(p = js_str(p, key, sizeof(key))))
                return s_modCount;
            p = js_ws(p);
            if (*p != ':')
                return s_modCount;
            p = js_ws(p + 1);
            val[0] = '\0';
            /* One '\n'-separated string, too long for val. */
            if (*p == '"' && !strcmp(key, "files"))
            {
                size_t cap = strlen(p) + 1;
                free(m.files);
                m.files = (char*)malloc(cap);
                if (!m.files || !(p = js_str(p, m.files, cap)))
                    return s_modCount;
                continue;
            }
            if (*p == '"')
            {
                if (!(p = js_str(p, val, sizeof(val))))
                    return s_modCount;
            }
            else
            {
                size_t n = 0;
                while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
                {
                    if (n + 1 < sizeof(val)) val[n++] = *p;
                    p++;
                }
                val[n] = '\0';
            }
            if      (!strcmp(key, "id"))          snprintf(m.id,     sizeof(m.id),     "%s", val);
            else if (!strcmp(key, "name"))        snprintf(m.name,   sizeof(m.name),   "%s", val);
            else if (!strcmp(key, "author"))      snprintf(m.author, sizeof(m.author), "%s", val);
            else if (!strcmp(key, "description")) snprintf(m.desc,   sizeof(m.desc),   "%s", val);
            else if (!strcmp(key, "file"))        snprintf(m.file,   sizeof(m.file),   "%s", val);
            else if (!strcmp(key, "url"))         snprintf(m.url,    sizeof(m.url),    "%s", val);
            else if (!strcmp(key, "size"))        m.size = atoll(val);
        }
        /* The file name becomes a path, so nothing that could leave the tmp dir. */
        if (m.id[0] && m.file[0] && m.url[0] && !strchr(m.file, '/') && !strchr(m.file, '\\') &&
            !strstr(m.file, "..") && s_modCount < QM_MAX_MODS)
            s_mods[s_modCount++] = m;
        else
            free(m.files);
    }
    return s_modCount;
}

/* ------------------------------------------------------------------ */
/* Install                                                             */
/* ------------------------------------------------------------------ */

static int qm_has_prefix_ci(const char* s, const char* pre)
{
    while (*pre)
        if (tolower((unsigned char)*s++) != tolower((unsigned char)*pre++))
            return 0;
    return 1;
}

/* Where an entry's path continues after its "load/" folder, or NULL. Mods
 * wrap that folder differently (load/... or ModName/load/...). */
static const char* qm_after_load(const char* name)
{
    const char* p = name;
    while (*p)
    {
        if ((p == name || p[-1] == '/') && qm_has_prefix_ci(p, "load/"))
            return p + 5;
        p++;
    }
    return NULL;
}

/* Returns 1 on success; err gets a short reason otherwise. */
static int qm_install_zip(const char* zipPath, const s_QmMod* m, char* err, size_t errsz)
{
    mz_zip_archive zip;
    mz_uint i, n;
    int texture = 0, written = 0, ok = 1;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath, 0))
    {
        snprintf(err, errsz, "not a valid zip");
        return 0;
    }
    n = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < n; i++)
    {
        char name[512];
        const char* base;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if ((qm_has_prefix_ci(base, "texupload-") || qm_has_prefix_ci(base, "texpage-")))
            texture = 1;
    }

    if (texture)
    {
        char dest[512];
        mz_zip_reader_end(&zip);
        qm_mkdir("gamedata");
        qm_mkdir("gamedata/texturemods");
        snprintf(dest, sizeof(dest), "gamedata/texturemods/%s", m->file);
        remove(dest);
        if (rename(zipPath, dest) != 0)
        {
            snprintf(err, errsz, "could not move it into texturemods");
            return 0;
        }
        return 1;
    }

    for (i = 0; i < n && ok; i++)
    {
        char name[512], dest[600];
        const char* rest;
        if (mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
        rest = qm_after_load(name);
        if (!rest || !rest[0] || strstr(rest, ".."))
            continue;
        snprintf(dest, sizeof(dest), "gamedata/load/%s", rest);
        qm_mkdirs_for(dest);
        if (!mz_zip_reader_extract_to_file(&zip, i, dest, 0))
        {
            snprintf(err, errsz, "could not write %s", rest);
            ok = 0;
        }
        else
            written++;
    }
    mz_zip_reader_end(&zip);
    remove(zipPath);
    if (ok && written == 0)
    {
        snprintf(err, errsz, "nothing in it the game can use");
        return 0;
    }
    if (ok && !g_PcConfig.allowLooseFiles)
    {
        g_PcConfig.allowLooseFiles = 1;
        PcConfig_SaveKeyValue("allow_loose_files", "1");
    }
    return ok;
}

/* Delete what the catalog says the mod put under gamedata/. Each path is
 * checked to stay inside load/ or texturemods/, since it comes off the net. */
static void qm_remove_mod(const s_QmMod* m)
{
    const char* p = m->files;
    while (p && *p)
    {
        const char* e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char rel[512], path[600];
        if (n > 0 && n < sizeof(rel))
        {
            memcpy(rel, p, n);
            rel[n] = '\0';
            if (!strstr(rel, "..") && rel[0] != '/' && rel[0] != '\\' &&
                (qm_has_prefix_ci(rel, "load/") || qm_has_prefix_ci(rel, "texturemods/")))
            {
                snprintf(path, sizeof(path), "gamedata/%s", rel);
                remove(path);
                if (qm_has_prefix_ci(rel, "texturemods/"))
                {
                    snprintf(path, sizeof(path), "gamedata/%s.disabled", rel);
                    remove(path);
                }
            }
        }
        p = e ? e + 1 : p + n;
    }
    qm_unmark_installed(m);
}

/* ------------------------------------------------------------------ */
/* Download task                                                       */
/* ------------------------------------------------------------------ */

enum { QT_IDLE = 0, QT_RUNNING, QT_FINISHED };
static volatile int       s_taskState;
static volatile long long s_taskGot;
static char               s_taskUrl[512], s_taskPath[512];
static int                s_taskMod;    /* -1 = catalog */
static volatile int       s_taskOk;
static char               s_taskErr[160];

/* Runs where the download finished: the worker thread natively, the main
 * thread in the browser. Touches nothing the UI draws from except the result. */
static void qm_task_after(int status)
{
    if (status != 200)
    {
        snprintf(s_taskErr, sizeof(s_taskErr), status ? "the server answered %d" : "no connection", status);
        remove(s_taskPath);
        s_taskOk = 0;
        return;
    }
    if (s_taskMod < 0)
    {
        long long sz = qm_file_size(s_taskPath);
        char* buf = (sz > 0 && sz < 1024 * 1024) ? (char*)malloc((size_t)sz + 1) : NULL;
        FILE* f = buf ? fopen(s_taskPath, "rb") : NULL;
        s_taskOk = 0;
        if (f)
        {
            size_t got = fread(buf, 1, (size_t)sz, f);
            fclose(f);
            buf[got] = '\0';
            s_taskOk = qm_parse_catalog(buf) > 0;
        }
        free(buf);
        if (!s_taskOk)
            snprintf(s_taskErr, sizeof(s_taskErr), "the mod list could not be read");
        return;
    }
    {
        const s_QmMod* m = &s_mods[s_taskMod];
        long long sz = qm_file_size(s_taskPath);
        if (m->size > 0 && sz != m->size)
        {
            snprintf(s_taskErr, sizeof(s_taskErr), "the download was incomplete");
            remove(s_taskPath);
            s_taskOk = 0;
            return;
        }
        s_taskOk = qm_install_zip(s_taskPath, m, s_taskErr, sizeof(s_taskErr));
        if (s_taskOk)
            qm_mark_installed(m);
        else
            remove(s_taskPath);
    }
}

#if defined(__EMSCRIPTEN__)
static volatile int s_webGot, s_webDone;

/* Streams into the virtual FS chunk by chunk rather than buffering the whole
 * body, so a large pack is not held twice. String ops only: a slash pair in
 * EM_JS reads as a C comment. */
EM_JS(void, Qm_WebFetchJs, (const char* url, const char* path, int* got, int* done), {
    var u = UTF8ToString(url);
    var p = UTF8ToString(path);
    fetch(u).then(function (r) {
        if (!r.ok || !r.body) { HEAP32[done >> 2] = r.status || -1; return; }
        var reader = r.body.getReader();
        var stream = FS.open(p, "w");
        var n = 0;
        function pump() {
            return reader.read().then(function (res) {
                if (res.done) { FS.close(stream); HEAP32[done >> 2] = 200; return; }
                FS.write(stream, res.value, 0, res.value.length);
                n += res.value.length;
                HEAP32[got >> 2] = n;
                return pump();
            });
        }
        return pump().catch(function () { try { FS.close(stream); } catch (e) {} HEAP32[done >> 2] = -1; });
    }).catch(function () { HEAP32[done >> 2] = -1; });
});

static void qm_web_url(char* out, size_t sz, const char* url)
{
    size_t baseLen = strlen(QM_RELEASE_BASE);
    if (strncmp(url, QM_RELEASE_BASE, baseLen) == 0)
        snprintf(out, sz, "%s%s", QM_WEB_RELAY, url + baseLen);
    else
        snprintf(out, sz, "%s", url);
}
#else
static int SDLCALL qm_worker(void* unused)
{
    int status;
    (void)unused;
    status = Pc_QolHttpDownload(s_taskUrl, s_taskPath, &s_taskGot);
    qm_task_after(status);
    s_taskState = QT_FINISHED;
    return 0;
}
#endif

static void qm_task_start(int modIdx)
{
    const char* file = modIdx < 0 ? QM_CATALOG_FILE : s_mods[modIdx].file;
    s_taskMod = modIdx;
    s_taskOk  = 0;
    s_taskErr[0] = '\0';
    s_taskGot = 0;
    if (modIdx < 0)
        snprintf(s_taskUrl, sizeof(s_taskUrl), "%s%s", QM_RELEASE_BASE, QM_CATALOG_FILE);
    else
        snprintf(s_taskUrl, sizeof(s_taskUrl), "%s", s_mods[modIdx].url);
    qm_mkdir("gamedata");
    qm_mkdir(QM_TMP_DIR);
    qm_tmp_path(s_taskPath, sizeof(s_taskPath), file);
    remove(s_taskPath);
    s_taskState = QT_RUNNING;
#if defined(__EMSCRIPTEN__)
    {
        char url[512];
        qm_web_url(url, sizeof(url), s_taskUrl);
        s_webGot = 0;
        s_webDone = 0;
        Qm_WebFetchJs(url, s_taskPath, (int*)&s_webGot, (int*)&s_webDone);
    }
#else
    {
        SDL_Thread* t = SDL_CreateThread(qm_worker, "SH_QOL", NULL);
        if (t)
            SDL_DetachThread(t);
        else
        {
            snprintf(s_taskErr, sizeof(s_taskErr), "could not start the download");
            s_taskState = QT_FINISHED;
        }
    }
#endif
}

/* Bytes so far for the progress line. */
static long long qm_task_progress(void)
{
#if defined(__EMSCRIPTEN__)
    return s_webGot;
#else
    long long got = s_taskGot;
    if (got <= 0)
        got = qm_file_size(s_taskPath);
    return got > 0 ? got : 0;
#endif
}

/* Poll the task; returns 1 on the frame it has finished. */
static int qm_task_poll(void)
{
#if defined(__EMSCRIPTEN__)
    if (s_taskState == QT_RUNNING && s_webDone != 0)
    {
        qm_task_after(s_webDone);
        s_taskState = QT_FINISHED;
    }
#endif
    if (s_taskState != QT_FINISHED)
        return 0;
    s_taskState = QT_IDLE;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Modes                                                               */
/* ------------------------------------------------------------------ */

static void qm_set_mode(int mode)
{
    if (s_mode == QM_CLOSED)
        s_openMs = SDL_GetTicks();
    s_mode   = mode;
    s_btnSel = 0;
}

static void qm_close(void)
{
    s_mode = QM_CLOSED;
}

static void qm_open_list_now(void)
{
    if (s_taskState != QT_IDLE)
    {
        qm_set_mode(s_taskMod < 0 ? QM_LOADING : QM_BUSY);
        return;
    }
    if (s_modCount > 0)
    {
        qm_load_installed();
        s_sel = 0;
        qm_set_mode(QM_LIST);
        return;
    }
    qm_set_mode(QM_LOADING);
    qm_task_start(-1);
}

int Pc_QolMods_IsOpen(void)
{
    return s_mode != QM_CLOSED;
}

void Pc_QolMods_OpenList(void)
{
    qm_open_list_now();
}

static int qm_change_count(void)
{
    int i, n = 0;
    for (i = 0; i < s_modCount; i++)
        if (s_mods[i].want != s_mods[i].installed)
            n++;
    return n;
}

static void qm_finish_message(void)
{
    char done[96];
    if (s_installedNow && s_removedNow)
        snprintf(done, sizeof(done), "Installed %d and removed %d mod%s.", s_installedNow, s_removedNow,
                 s_removedNow == 1 ? "" : "s");
    else if (s_removedNow)
        snprintf(done, sizeof(done), "Removed %d mod%s.", s_removedNow, s_removedNow == 1 ? "" : "s");
    else
        snprintf(done, sizeof(done), "Installed %d mod%s.", s_installedNow, s_installedNow == 1 ? "" : "s");

    if (s_failedNow == 0)
        snprintf(s_message, sizeof(s_message), "%s %s", done, QM_RESTART_TEXT);
    else if (s_installedNow + s_removedNow > 0)
        snprintf(s_message, sizeof(s_message), "%s %s Could not install: %s", done, QM_RESTART_TEXT, s_failNames);
    else
        snprintf(s_message, sizeof(s_message), "Could not install: %s", s_failNames);
}

/* Removals are a few deletes, done at once; installs download one by one. */
static void qm_begin_apply(void)
{
    int i;
    if (qm_change_count() == 0)
        return;
    s_queueLen = 0;
    s_queuePos = 0;
    s_installedNow = 0;
    s_removedNow = 0;
    s_failedNow = 0;
    s_failNames[0] = '\0';
    for (i = 0; i < s_modCount; i++)
    {
        if (s_mods[i].installed && !s_mods[i].want)
        {
            qm_remove_mod(&s_mods[i]);
            s_mods[i].installed = 0;
            s_removedNow++;
        }
        else if (!s_mods[i].installed && s_mods[i].want)
            s_queue[s_queueLen++] = i;
    }
    if (s_queueLen == 0)
    {
        qm_finish_message();
        qm_set_mode(QM_DONE);
        return;
    }
    qm_set_mode(QM_BUSY);
    qm_task_start(s_queue[0]);
}

/* Button bar per mode. LIST adds "Install (n)" and "Close" after its rows. */
static int qm_buttons(const char* labels[3])
{
    static char installLbl[32];
    switch (s_mode)
    {
        case QM_PROMPT: labels[0] = "More Info"; labels[1] = "No"; return 2;
        case QM_NOTICE: labels[0] = "OK"; return 1;
        case QM_LOADING: labels[0] = "Close"; return 1;
        case QM_ERROR:  labels[0] = "Retry"; labels[1] = "Close"; return 2;
        case QM_LIST:
            snprintf(installLbl, sizeof(installLbl), "Apply (%d)", qm_change_count());
            labels[0] = installLbl; labels[1] = "Close"; return 2;
        case QM_INFO:
        {
            const s_QmMod* m = &s_mods[s_infoIdx];
            if (m->installed) labels[0] = m->want ? "Remove" : "Keep";
            else              labels[0] = m->want ? "Unselect" : "Select";
            labels[1] = "Back";
            return 2;
        }
        case QM_DONE:   labels[0] = "OK"; return 1;
        default: return 0;
    }
}

static void qm_press_button(int b)
{
    switch (s_mode)
    {
        case QM_PROMPT:
            if (b == 0) qm_open_list_now();
            else        qm_set_mode(QM_NOTICE);
            break;
        case QM_NOTICE:
        case QM_LOADING:
            qm_close();
            break;
        case QM_ERROR:
            if (b == 0) { qm_set_mode(QM_LOADING); qm_task_start(-1); }
            else        qm_close();
            break;
        case QM_LIST:
            if (b == 0) qm_begin_apply();
            else        qm_close();
            break;
        case QM_INFO:
            if (b == 0)
                s_mods[s_infoIdx].want = !s_mods[s_infoIdx].want;
            else
            {
                s_sel = s_infoIdx;
                qm_set_mode(QM_LIST);
            }
            break;
        case QM_DONE:
            qm_load_installed();
            s_sel = 0;
            qm_set_mode(QM_LIST);
            break;
        default:
            break;
    }
}

static void qm_cancel(void)
{
    switch (s_mode)
    {
        case QM_PROMPT: qm_set_mode(QM_NOTICE); break;
        case QM_INFO:   s_sel = s_infoIdx; qm_set_mode(QM_LIST); break;
        case QM_DONE:   qm_press_button(0); break;
        case QM_BUSY:   break; /* finishes on its own; nothing half-installed is left */
        default:        qm_close(); break;
    }
}

/* Advance the background work: catalog fetched, or one mod done. */
static void qm_step(void)
{
    if (!qm_task_poll())
        return;

    if (s_taskMod < 0)
    {
        if (s_mode != QM_LOADING)
            return;
        if (s_taskOk)
        {
            qm_load_installed();
            s_sel = 0;
            qm_set_mode(QM_LIST);
        }
        else
        {
            snprintf(s_message, sizeof(s_message),
                     "Could not get the list of mods (%s). Check your connection and try again.", s_taskErr);
            qm_set_mode(QM_ERROR);
        }
        return;
    }

    if (s_taskOk)
    {
        s_mods[s_taskMod].installed = 1;
        s_installedNow++;
    }
    else
    {
        size_t n = strlen(s_failNames);
        s_failedNow++;
        snprintf(s_failNames + n, sizeof(s_failNames) - n, "%s%s (%s)",
                 n ? ", " : "", s_mods[s_taskMod].name, s_taskErr);
        SH_DBG("[QOL] %s failed: %s", s_mods[s_taskMod].name, s_taskErr);
    }

    if (++s_queuePos < s_queueLen)
    {
        qm_task_start(s_queue[s_queuePos]);
        return;
    }

    qm_finish_message();
    if (s_mode == QM_BUSY)
        qm_set_mode(QM_DONE);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Geometry published by Draw for the pointer hit-test (viewport px, y up). */
static float s_vpW = 1920.0f, s_vpH = 1080.0f;
static int   s_geoValid;
static float s_geoRowT, s_geoRowPitch, s_geoRowL, s_geoRowR, s_geoCheckR;
static int   s_geoRows;
static float s_geoBtnL[3], s_geoBtnR[3], s_geoBtnT, s_geoBtnB;
static int   s_geoBtns;

#define QM_OPEN_GUARD_MS 250u

int Pc_QolMods_Frame(int atTitle, int up, int down, int left, int right, int confirm, int cancel)
{
    const char* labels[3];
    int   nBtn, mMoved, mClick;
    float mx, my;

    qm_step();

    if (s_mode == QM_CLOSED)
    {
        if (!atTitle)
        {
            s_titleFrames = 0;
            return 0;
        }
        /* A second on the title before asking, so it does not land on the
         * first frame of the menu fading in. */
        if (!s_offeredThisRun && ++s_titleFrames > 60)
        {
            const char* v = Pc_ModConfig_Value(QM_OFFERED_KEY);
            s_offeredThisRun = 1;
            if (!(v && v[0] == '1'))
            {
                PcConfig_SaveKeyValue(QM_OFFERED_KEY, "1");
                qm_set_mode(QM_PROMPT);
                Pc_MouseCursor_Show();
                return 1;
            }
        }
        return 0;
    }

    /* The pointer is drawn only on frames that ask for it. */
    Pc_MouseCursor_Show();

    /* The press that opened the window must not also act inside it. */
    if (SDL_GetTicks() - s_openMs < QM_OPEN_GUARD_MS)
        return 1;

    nBtn = qm_buttons(labels);

    if (cancel) { qm_cancel(); return 1; }

    if (s_mode == QM_LIST)
    {
        int total = s_modCount + 1; /* the button bar counts as one stop */
        if (up)   s_sel = (s_sel + total - 1) % total;
        if (down) s_sel = (s_sel + 1) % total;
        if (s_sel < s_modCount)
        {
            if (right) { s_infoIdx = s_sel; qm_set_mode(QM_INFO); return 1; }
            if (confirm) s_mods[s_sel].want = !s_mods[s_sel].want;
            confirm = 0;
        }
        else
        {
            if (left)  s_btnSel = (s_btnSel + nBtn - 1) % nBtn;
            if (right) s_btnSel = (s_btnSel + 1) % nBtn;
        }
    }
    else if (nBtn > 0)
    {
        if (left)  s_btnSel = (s_btnSel + nBtn - 1) % nBtn;
        if (right) s_btnSel = (s_btnSel + 1) % nBtn;
    }

    /* Pointer and touch: tap a checkbox to tick it, the rest of a row for its
     * details, or a button. */
    mMoved = Pc_MouseCursor_Moved();
    mClick = Pc_MouseCursor_LeftClicked();
    if (s_geoValid && (mMoved || mClick) && Pc_MouseCursor_ViewportPos(&mx, &my))
    {
        float py  = (1.0f - my) * s_vpH;
        float mpx = mx * s_vpW;
        int   b;
        for (b = 0; b < s_geoBtns && b < nBtn; b++)
        {
            if (mpx >= s_geoBtnL[b] && mpx <= s_geoBtnR[b] && py <= s_geoBtnT && py >= s_geoBtnB)
            {
                if (mMoved) { s_btnSel = b; if (s_mode == QM_LIST) s_sel = s_modCount; }
                if (mClick) { qm_press_button(b); return 1; }
            }
        }
        if (s_mode == QM_LIST && s_geoRowPitch > 0.0f && mpx >= s_geoRowL && mpx <= s_geoRowR)
        {
            int row = (int)((s_geoRowT - py) / s_geoRowPitch);
            if (py <= s_geoRowT && row >= 0 && row < s_geoRows && row < s_modCount)
            {
                if (mMoved) s_sel = row;
                if (mClick)
                {
                    s_sel = row;
                    if (mpx <= s_geoCheckR)
                        s_mods[row].want = !s_mods[row].want;
                    else
                    {
                        s_infoIdx = row;
                        qm_set_mode(QM_INFO);
                    }
                    return 1;
                }
            }
        }
    }

    if (confirm && nBtn > 0)
        qm_press_button(s_btnSel);
    return 1;
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo, s_texWhite;
static GLint  s_locColor;
static int    s_glReady;

static stbtt_fontinfo s_font;
static unsigned char* s_fontData;
static int            s_fontOk, s_fontsTried;

#define QM_GARBAGE 160
static GLuint s_garbage[QM_GARBAGE];
static int    s_garbageCount;

static GLuint qm_make_shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[QOL] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void qm_gl_init(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv) * u_color; }\n";
    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0, prevProg = 0;

    s_glReady = -1;
    vs = qm_make_shader(GL_VERTEX_SHADER, vs_src);
    fs = qm_make_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_uv");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    /* PsyCross caches the bound program, so the previous one must be put back. */
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glUseProgram(s_prog);
    glUniform1i(glGetUniformLocation(s_prog, "u_tex"), 0);
    s_locColor = glGetUniformLocation(s_prog, "u_color");
    glUseProgram((GLuint)prevProg);

    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    s_glReady = 1;
}

static GLuint qm_upload_rgba(const unsigned char* rgba, int w, int h)
{
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return t;
}

static void qm_retire(GLuint tex)
{
    if (tex && s_garbageCount < QM_GARBAGE)
        s_garbage[s_garbageCount++] = tex;
    else if (tex)
        glDeleteTextures(1, &tex);
}

static void qm_quad(GLuint tex, float x0, float yTop, float x1, float yBot, float r, float g, float b, float a)
{
    float v[6][4];
    if (!tex)
        return;
    v[0][0] = x0; v[0][1] = yTop; v[0][2] = 0; v[0][3] = 0;
    v[1][0] = x0; v[1][1] = yBot; v[1][2] = 0; v[1][3] = 1;
    v[2][0] = x1; v[2][1] = yTop; v[2][2] = 1; v[2][3] = 0;
    v[3][0] = x1; v[3][1] = yTop; v[3][2] = 1; v[3][3] = 0;
    v[4][0] = x0; v[4][1] = yBot; v[4][2] = 0; v[4][3] = 1;
    v[5][0] = x1; v[5][1] = yBot; v[5][2] = 1; v[5][3] = 1;
    glUniform4f(s_locColor, r, g, b, a);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static unsigned char* qm_read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    long  n;
    unsigned char* buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static void qm_fonts_init(void)
{
    static const char* paths[] = {
        "gamedata/font/Oswald-Regular.ttf",
        "gamedata/font/BarlowSemiCondensed-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    int i;
    if (s_fontsTried)
        return;
    s_fontsTried = 1;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++)
    {
        unsigned char* data = qm_read_file(paths[i]);
        if (!data)
            continue;
        if (stbtt_InitFont(&s_font, data, stbtt_GetFontOffsetForIndex(data, 0)))
        {
            s_fontData = data; /* stbtt keeps pointers into this */
            s_fontOk   = 1;
            return;
        }
        free(data);
    }
    SH_DBG("[QOL] no usable font - text disabled");
}

static float qm_text_width(const char* text, int len, float px)
{
    float scale = stbtt_ScaleForPixelHeight(&s_font, px), w = 0.0f;
    int   i, prev = 0;
    for (i = 0; i < len && text[i]; i++)
    {
        int cp = (unsigned char)text[i], adv, lsb;
        if (prev)
            w += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        w += adv * scale;
        prev = cp;
    }
    return w;
}

static GLuint qm_bake(const char* text, float px, int* outW, int* outH)
{
    const char* p;
    float scale, penX, baseY;
    int   asc, desc, gap, W, H, i, pad = 1, prev = 0;
    unsigned char *cov, *rgba, *scratch;
    GLuint tex;

    if (!s_fontOk || !text || !text[0] || px < 4.0f)
        return 0;
    scale = stbtt_ScaleForPixelHeight(&s_font, px);
    stbtt_GetFontVMetrics(&s_font, &asc, &desc, &gap);
    W = (int)ceilf(qm_text_width(text, (int)strlen(text), px)) + 4 * pad + 2;
    H = (int)ceilf(px * 1.4f) + 2 * pad;
    if (W <= 0 || H <= 0 || W > 4096 || H > 512)
        return 0;
    cov     = (unsigned char*)calloc((size_t)W * H, 1);
    rgba    = (unsigned char*)calloc((size_t)W * H, 4);
    scratch = (unsigned char*)malloc((size_t)W * H);
    if (!cov || !rgba || !scratch) { free(cov); free(rgba); free(scratch); return 0; }

    penX  = (float)pad;
    baseY = (float)pad + asc * scale;
    p     = text;
    while (*p)
    {
        int cp = (unsigned char)*p++;
        int gx0, gy0, gx1, gy1, gw, gh, adv, lsb, sx, sy;
        float shiftX;
        if (prev)
            penX += stbtt_GetCodepointKernAdvance(&s_font, prev, cp) * scale;
        shiftX = penX - floorf(penX);
        stbtt_GetCodepointBitmapBoxSubpixel(&s_font, cp, scale, scale, shiftX, 0.0f, &gx0, &gy0, &gx1, &gy1);
        gw = gx1 - gx0;
        gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= W && gh <= H)
        {
            memset(scratch, 0, (size_t)gw * gh);
            stbtt_MakeCodepointBitmapSubpixel(&s_font, scratch, gw, gh, gw, scale, scale, shiftX, 0.0f, cp);
            for (sy = 0; sy < gh; sy++)
            {
                int dy = (int)baseY + gy0 + sy;
                if (dy < 0 || dy >= H) continue;
                for (sx = 0; sx < gw; sx++)
                {
                    int dx = (int)penX + gx0 + sx;
                    unsigned char v;
                    if (dx < 0 || dx >= W) continue;
                    v = scratch[sy * gw + sx];
                    if (v > cov[dy * W + dx]) cov[dy * W + dx] = v;
                }
            }
        }
        stbtt_GetCodepointHMetrics(&s_font, cp, &adv, &lsb);
        penX += adv * scale;
        prev = cp;
        if (penX > (float)(W - pad)) break;
    }
    for (i = 0; i < W * H; i++)
    {
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = cov[i];
    }
    tex = qm_upload_rgba(rgba, W, H);
    if (outW) *outW = W;
    if (outH) *outH = H;
    free(cov); free(rgba); free(scratch);
    return tex;
}

/* Baked text cache: the window redraws the same strings every frame, and the
 * progress line changes a few times a second. Least recently used goes. */
#define QM_TEXT_CACHE 96
typedef struct { char key[200]; int px; GLuint tex; int w, h; Uint32 used; } s_QmText;
static s_QmText s_text[QM_TEXT_CACHE];
static Uint32   s_textClock;

static GLuint qm_text(const char* s, int px, int* w, int* h)
{
    int i, victim = 0;
    s_textClock++;
    for (i = 0; i < QM_TEXT_CACHE; i++)
    {
        if (s_text[i].tex && s_text[i].px == px && strcmp(s_text[i].key, s) == 0)
        {
            s_text[i].used = s_textClock;
            *w = s_text[i].w;
            *h = s_text[i].h;
            return s_text[i].tex;
        }
        if (s_text[i].used < s_text[victim].used)
            victim = i;
    }
    qm_retire(s_text[victim].tex);
    snprintf(s_text[victim].key, sizeof(s_text[victim].key), "%s", s);
    s_text[victim].px   = px;
    s_text[victim].used = s_textClock;
    s_text[victim].tex  = qm_bake(s, (float)px, &s_text[victim].w, &s_text[victim].h);
    *w = s_text[victim].w;
    *h = s_text[victim].h;
    return s_text[victim].tex;
}

static void qm_drop_text_cache(void)
{
    int i;
    for (i = 0; i < QM_TEXT_CACHE; i++)
    {
        qm_retire(s_text[i].tex);
        s_text[i].tex = 0;
        s_text[i].used = 0;
    }
}

/* Split text into lines no wider than maxW. Returns the line count. */
#define QM_MAX_LINES 16
static int qm_wrap(const char* text, float px, float maxW, char lines[QM_MAX_LINES][200])
{
    int n = 0;
    const char* p = text;
    while (*p && n < QM_MAX_LINES)
    {
        int len = 0, lastSpace = -1;
        while (p[len] && p[len] != '\n' && len < 199)
        {
            if (p[len] == ' ')
                lastSpace = len;
            if (qm_text_width(p, len + 1, px) > maxW && len > 0)
            {
                if (lastSpace > 0)
                    len = lastSpace;
                break;
            }
            len++;
        }
        memcpy(lines[n], p, (size_t)len);
        lines[n][len] = '\0';
        n++;
        p += len;
        while (*p == ' ')
            p++;
        if (*p == '\n')
            p++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Draw                                                                */
/* ------------------------------------------------------------------ */

static float s_dvpW, s_dvpH, s_dim;
#define NX(px_) (-1.0f + 2.0f * (px_) / s_dvpW)
#define NY(py_) (-1.0f + 2.0f * (py_) / s_dvpH)

static void qm_rect(float l, float t, float r, float b, float cr, float cg, float cb, float ca)
{
    qm_quad(s_texWhite, NX(l), NY(t), NX(r), NY(b), cr, cg, cb, ca * s_dim);
}

/* Text with its top-left at (x, top). Returns the width drawn. */
static float qm_draw_text(const char* s, int px, float x, float top, float r, float g, float b)
{
    int w = 0, h = 0;
    GLuint tex;
    if (!s || !s[0])
        return 0.0f;
    tex = qm_text(s, px, &w, &h);
    if (tex)
        qm_quad(tex, NX(x), NY(top), NX(x + w), NY(top - h), r, g, b, s_dim);
    return (float)w;
}

static void qm_draw_paragraph(const char* text, int px, float x, float top, float maxW, float lineH,
                              float r, float g, float b)
{
    char lines[QM_MAX_LINES][200];
    int  i, n = qm_wrap(text, (float)px, maxW, lines);
    for (i = 0; i < n; i++)
        qm_draw_text(lines[i], px, x, top - (float)i * lineH, r, g, b);
}

static void qm_format_mb(char* out, size_t sz, long long bytes)
{
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb < 1.0)
        snprintf(out, sz, "%.1f MB", mb < 0.1 ? 0.1 : mb);
    else
        snprintf(out, sz, "%.0f MB", mb);
}

void Pc_QolMods_Draw(void)
{
    GLint vp[4];
    float panelW, panelH, panelL, panelR, panelT, panelB, pad, titleH, btnH, bodyT, bodyB, lineH;
    int   px, pxTitle, nBtn, i;
    const char* labels[3];
    static int s_bakedPx;

    GLint  prevProg = 0, prevVao = 0, prevBuf = 0, prevTex = 0, prevUnit = GL_TEXTURE0, prevAlign = 4;
    GLint  prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint  prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;

    if (s_mode == QM_CLOSED)
    {
        s_geoValid = 0;
        return;
    }

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    s_dvpW = (float)vp[2];
    s_dvpH = (float)vp[3];

    if (!s_glReady) qm_gl_init();
    if (s_glReady != 1) return;
    if (!s_fontsTried) qm_fonts_init();

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevUnit);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    for (i = 0; i < s_garbageCount; i++)
        glDeleteTextures(1, &s_garbage[i]);
    s_garbageCount = 0;

    s_dim = (float)(SDL_GetTicks() - s_openMs) / 180.0f;
    if (s_dim > 1.0f) s_dim = 1.0f;

    /* Layout (viewport px, origin bottom-left). Phones are wide and short, so
     * the panel is sized off the height and capped by the width. */
    panelH = 0.86f * s_dvpH;
    panelW = 1.25f * panelH;
    if (panelW > 0.92f * s_dvpW) panelW = 0.92f * s_dvpW;
    if (s_mode == QM_PROMPT || s_mode == QM_NOTICE || s_mode == QM_ERROR ||
        s_mode == QM_LOADING || s_mode == QM_DONE || s_mode == QM_BUSY)
        panelH *= 0.62f;
    panelL = (s_dvpW - panelW) * 0.5f;
    panelR = panelL + panelW;
    panelB = (s_dvpH - panelH) * 0.5f;
    panelT = panelB + panelH;
    pad     = panelW * 0.04f;
    px      = (int)(s_dvpH * 0.034f);
    if (px < 10) px = 10;
    pxTitle = (int)(px * 1.25f);
    lineH   = (float)px * 1.32f;
    titleH  = (float)pxTitle * 2.0f;
    btnH    = (float)px * 2.3f;
    bodyT   = panelT - titleH - pad * 0.6f;
    bodyB   = panelB + btnH + pad;

    if (s_bakedPx != px)
    {
        qm_drop_text_cache();
        s_bakedPx = px;
    }

    if (!s_texWhite)
    {
        unsigned char one[4] = { 255, 255, 255, 255 };
        s_texWhite = qm_upload_rgba(one, 1, 1);
    }

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    /* Re-assert the layout every draw: other GL code repoints attribute 1 and
     * every text quad then samples a single texel. See pc_rando_settings.c. */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    s_vpW = s_dvpW;
    s_vpH = s_dvpH;

    qm_rect(0.0f, s_dvpH, s_dvpW, 0.0f, 0.0f, 0.0f, 0.0f, 0.62f);
    qm_rect(panelL, panelT, panelR, panelB, 0.055f, 0.05f, 0.065f, 0.96f);
    qm_rect(panelL + 2.0f, panelT - 2.0f, panelR - 2.0f, panelT - titleH, 0.10f, 0.05f, 0.05f, 0.95f);
    qm_rect(panelL + 2.0f, panelT - titleH, panelR - 2.0f, panelT - titleH - 2.0f, 0.47f, 0.11f, 0.08f, 1.0f);
    {
        const char* title = (s_mode == QM_INFO) ? s_mods[s_infoIdx].name : "Essential QoL Mods";
        int w = 0, h = 0;
        qm_text(title, pxTitle, &w, &h);
        qm_draw_text(title, pxTitle, panelL + (panelW - (float)w) * 0.5f,
                     panelT - (titleH - (float)h) * 0.5f, 1.0f, 0.93f, 0.86f);
    }

    s_geoRows = 0;
    switch (s_mode)
    {
        case QM_PROMPT:
            qm_draw_paragraph("Would you like to install the Essential QoL Mods? These free community mods add "
                              "HD fonts, HD maps, HD examination images and more.",
                              px, panelL + pad, bodyT, panelW - 2.0f * pad, lineH, 0.92f, 0.92f, 0.95f);
            break;
        case QM_NOTICE:
            qm_draw_paragraph("You can install or remove them any time from " QM_PLATFORM_OPTIONS " > Set Up QoL Mods.",
                              px, panelL + pad, bodyT, panelW - 2.0f * pad, lineH, 0.92f, 0.92f, 0.95f);
            break;
        case QM_LOADING:
            qm_draw_paragraph("Getting the list of mods...", px, panelL + pad, bodyT, panelW - 2.0f * pad, lineH,
                              0.92f, 0.92f, 0.95f);
            break;
        case QM_ERROR:
        case QM_DONE:
            qm_draw_paragraph(s_message, px, panelL + pad, bodyT, panelW - 2.0f * pad, lineH, 0.92f, 0.92f, 0.95f);
            break;
        case QM_BUSY:
            if (s_queueLen > 0)
        {
            char line[200], got[24], tot[24];
            const s_QmMod* m = &s_mods[s_queue[s_queuePos < s_queueLen ? s_queuePos : s_queueLen - 1]];
            long long have = qm_task_progress();
            float frac = (m->size > 0) ? (float)((double)have / (double)m->size) : 0.0f;
            float barT = bodyT - lineH * 2.2f, barB = barT - (float)px * 0.8f;
            if (frac > 1.0f) frac = 1.0f;
            snprintf(line, sizeof(line), "Installing %s (%d of %d)", m->name, s_queuePos + 1, s_queueLen);
            qm_draw_text(line, px, panelL + pad, bodyT, 0.92f, 0.92f, 0.95f);
            qm_format_mb(got, sizeof(got), have);
            qm_format_mb(tot, sizeof(tot), m->size);
            snprintf(line, sizeof(line), (frac >= 1.0f) ? "Unpacking..." : "%s of %s", got, tot);
            qm_draw_text(line, px, panelL + pad, bodyT - lineH, 0.75f, 0.75f, 0.80f);
            qm_rect(panelL + pad, barT, panelR - pad, barB, 0.18f, 0.16f, 0.20f, 1.0f);
            qm_rect(panelL + pad, barT, panelL + pad + (panelW - 2.0f * pad) * frac, barB, 0.62f, 0.16f, 0.12f, 1.0f);
            qm_draw_paragraph("Keep the game open until this finishes.", px, panelL + pad, barB - lineH * 0.6f,
                              panelW - 2.0f * pad, lineH, 0.65f, 0.65f, 0.70f);
            break;
        }
        case QM_INFO:
        {
            const s_QmMod* m = &s_mods[s_infoIdx];
            char line[300], sz[24];
            float y = bodyT;
            snprintf(line, sizeof(line), "By %s", m->author);
            qm_draw_paragraph(line, px, panelL + pad, y, panelW - 2.0f * pad, lineH, 0.95f, 0.80f, 0.55f);
            {
                char tmp[QM_MAX_LINES][200];
                y -= lineH * (float)qm_wrap(line, (float)px, panelW - 2.0f * pad, tmp) + lineH * 0.4f;
            }
            qm_draw_paragraph(m->desc, px, panelL + pad, y, panelW - 2.0f * pad, lineH, 0.92f, 0.92f, 0.95f);
            qm_format_mb(sz, sizeof(sz), m->size);
            snprintf(line, sizeof(line), "%s   %s", sz,
                     m->installed ? (m->want ? "Installed" : "Will be removed on Apply")
                                  : (m->want ? "Will be installed on Apply" : "Not installed"));
            qm_draw_text(line, px, panelL + pad, bodyB + lineH * 1.2f, 0.70f, 0.70f, 0.75f);
            break;
        }
        case QM_LIST:
        {
            float rowPitch = lineH * 2.05f, rowH = rowPitch * 0.92f, box = (float)px * 1.05f;
            float hintY = bodyB + lineH * 0.9f;
            float listT = bodyT + lineH * 0.3f;
            int   maxRows = (int)((listT - hintY - lineH * 0.4f) / rowPitch);
            if (maxRows < 1) maxRows = 1;
            s_geoRowT = listT; s_geoRowPitch = rowPitch; s_geoRowL = panelL + pad * 0.5f; s_geoRowR = panelR - pad * 0.5f;
            s_geoCheckR = panelL + pad + box * 1.6f;
            s_geoRows = s_modCount < maxRows ? s_modCount : maxRows;
            for (i = 0; i < s_geoRows; i++)
            {
                const s_QmMod* m = &s_mods[i];
                float top = listT - (float)i * rowPitch, mid = top - rowH * 0.5f;
                float bx = panelL + pad, by = mid + box * 0.5f, tx = bx + box * 1.6f;
                char  right[64], sz[24];
                int   w = 0, h = 0;
                if (i == s_sel)
                    qm_rect(panelL + 4.0f, top, panelR - 4.0f, top - rowH, 0.42f, 0.16f, 0.12f, 0.55f);
                qm_rect(bx, by, bx + box, by - box, 0.70f, 0.70f, 0.75f, 1.0f);
                qm_rect(bx + 2.0f, by - 2.0f, bx + box - 2.0f, by - box + 2.0f, 0.08f, 0.07f, 0.09f, 1.0f);
                if (m->want)
                    qm_rect(bx + box * 0.25f, by - box * 0.25f, bx + box * 0.75f, by - box * 0.75f,
                            0.85f, 0.25f, 0.18f, 1.0f);
                qm_draw_text(m->name, px, tx, mid + lineH * 0.95f, 0.95f, 0.95f, 0.97f);
                {
                    char by_[200];
                    snprintf(by_, sizeof(by_), "by %s", m->author);
                    /* One line in the list; the details page shows it all. */
                    if (strlen(by_) > 44 && qm_text_width(by_, (int)strlen(by_), (float)px * 0.8f) > panelW * 0.6f)
                        snprintf(by_ + 40, sizeof(by_) - 40, "...");
                    qm_draw_text(by_, (int)(px * 0.8f), tx, mid - lineH * 0.05f, 0.62f, 0.62f, 0.68f);
                }
                qm_format_mb(sz, sizeof(sz), m->size);
                snprintf(right, sizeof(right), "%s",
                         m->installed ? (m->want ? "Installed" : "Remove") : (m->want ? "Install" : sz));
                qm_text(right, px, &w, &h);
                qm_draw_text(right, px, panelR - pad - (float)w, mid + (float)h * 0.5f, 0.75f, 0.75f, 0.80f);
            }
            qm_draw_paragraph("Tick to install, untick to remove, then Apply. Tap a name for details.",
                              (int)(px * 0.85f), panelL + pad, hintY, panelW - 2.0f * pad, lineH, 0.6f, 0.6f, 0.65f);
            break;
        }
        default:
            break;
    }

    /* Button bar. */
    nBtn = qm_buttons(labels);
    s_geoBtns = nBtn;
    if (nBtn > 0)
    {
        float bw = (panelW - 2.0f * pad - (float)(nBtn - 1) * pad) / (float)nBtn;
        float bT = panelB + pad * 0.5f + btnH * 0.85f, bB = panelB + pad * 0.5f;
        int   barFocused = (s_mode != QM_LIST) || s_sel >= s_modCount;
        if (bw > panelW * 0.32f) bw = panelW * 0.32f;
        {
            float total = bw * (float)nBtn + pad * (float)(nBtn - 1);
            float x = panelL + (panelW - total) * 0.5f;
            for (i = 0; i < nBtn; i++)
            {
                int w = 0, h = 0, on = barFocused && i == s_btnSel;
                s_geoBtnL[i] = x; s_geoBtnR[i] = x + bw;
                qm_rect(x, bT, x + bw, bB, on ? 0.47f : 0.16f, on ? 0.13f : 0.14f, on ? 0.10f : 0.18f, 1.0f);
                qm_text(labels[i], px, &w, &h);
                qm_draw_text(labels[i], px, x + (bw - (float)w) * 0.5f, bT - (bT - bB - (float)h) * 0.5f,
                             1.0f, 0.95f, 0.90f);
                x += bw + pad;
            }
        }
        s_geoBtnT = bT;
        s_geoBtnB = bB;
    }
    s_geoValid = 1;

    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glActiveTexture((GLenum)prevUnit);
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
    glUseProgram((GLuint)prevProg);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
}

#undef NX
#undef NY
