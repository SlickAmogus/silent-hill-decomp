#include "tex_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* _strdup / _stricmp / _strnicmp are MSVC spellings of POSIX string helpers;
 * map them to the POSIX names off Windows. No <direct.h>: it is MSVC-only and
 * none of its directory calls (_mkdir etc.) are used here. */
#ifndef _WIN32
#include <strings.h> /* strcasecmp / strncasecmp */
#define _strdup(s)         strdup(s)
#define _strnicmp(a, b, n) strncasecmp((a), (b), (n))
#define _stricmp(a, b)     strcasecmp((a), (b))
#endif

#define XXH_INLINE_ALL
#include "xxhash.h"

#include "miniz.h"
#include "stb_image.h"

#include "pc_config.h"
#include "sh_log.h"

#ifndef TEXPACK_DIR
#define TEXPACK_DIR "gamedata/texturemods"
#endif

typedef struct {
    unsigned long long srcHash;
    unsigned long long palHash;
    unsigned short srcW, srcH;   /* upload size, VRAM halfwords */
    unsigned short offX, offY;   /* replaced sub-rect, native texels */
    unsigned short subW, subH;
    unsigned char  mode;         /* index into DuckStation's mode table:
                                  * 0=P4 1=P8 2/3=C16, +4 = STP variants */
    unsigned char  palMin;
    unsigned short palMax;
    short          zipIdx;       /* -1 = loose file */
    unsigned int   zipEntry;
    char*          loosePath;    /* malloc'd, NULL for zip entries */
    int            priority;     /* from loadorder.txt; higher wins same-subrect conflicts */
    unsigned int   seq;          /* insertion index — final tiebreak so the sort is total/stable */
} PackEntry;

static PackEntry* g_entries    = NULL;
static int        g_entryCount = 0;
static int        g_entryCap   = 0;

static char** g_zipPaths   = NULL;
static int    g_zipCount   = 0;
static int    g_zipCap     = 0;

static int g_scanned = 0;
static int g_texpageSkipped = 0; /* DuckStation "texpage-" (page-mode) entries — unsupported */

/* Optional gamedata/texturemods/loadorder.txt (written by the launcher's mod
 * manager): one top-level pack folder per line, HIGHEST priority first. When two
 * packs replace the same sub-rect of the same source texture, the higher-priority
 * one must blit last so it wins — this drives the entry sort below. Absent file =
 * every pack priority 0 = deterministic insertion order (no behavior loss). */
#define TEXPACK_MAX_PACKS 512
static char* g_loadOrder[TEXPACK_MAX_PACKS];
static int   g_loadOrderCount = 0;

static void LoadOrder_Read(void)
{
    char  path[512];
    char  line[256];
    FILE* f;

    snprintf(path, sizeof(path), "%s/loadorder.txt", TEXPACK_DIR);
    f = fopen(path, "rb");
    if (f == NULL) return;

    while (fgets(line, sizeof(line), f) != NULL && g_loadOrderCount < TEXPACK_MAX_PACKS)
    {
        char*  s = line;
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' '  || line[n - 1] == '\t'))
            line[--n] = '\0';
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '#') continue;
        g_loadOrder[g_loadOrderCount++] = _strdup(s);
    }
    fclose(f);
}

/* Top-level pack folder name for an entry's on-disk path (first component under
 * TEXPACK_DIR). Paths are always built from TEXPACK_DIR with '/' separators. */
static void Pack_ModName(const char* fullPath, char* out, size_t outSize)
{
    const char* p      = fullPath;
    size_t      pfxLen = strlen(TEXPACK_DIR);
    size_t      i      = 0;

    out[0] = '\0';
    if (fullPath == NULL) return;
    if (_strnicmp(p, TEXPACK_DIR, (int)pfxLen) == 0 && (p[pfxLen] == '/' || p[pfxLen] == '\\'))
        p += pfxLen + 1;
    while (p[i] != '\0' && p[i] != '/' && p[i] != '\\' && i + 1 < outSize)
    {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static int LoadOrder_Priority(const char* modName)
{
    int i;
    if (modName[0] == '\0') return 0;
    for (i = 0; i < g_loadOrderCount; i++)
        if (_stricmp(g_loadOrder[i], modName) == 0)
            return g_loadOrderCount - i; /* first line = highest priority */
    return 0;
}

static int Mode_Bpp(unsigned char mode)
{
    switch (mode & 0x3) {
        case 0:  return 4;
        case 1:  return 8;
        default: return 16;
    }
}

static int ParseHex64(const char** p, unsigned long long* out)
{
    char* end;
    *out = strtoull(*p, &end, 16);
    if (end != *p + 16) return 0; /* DuckStation always writes 16 hex digits */
    *p = end;
    return 1;
}

static int ParseDec16(const char** p, unsigned short* out)
{
    char*         end;
    unsigned long v = strtoul(*p, &end, 10);
    if (end == *p || v > 0xFFFF) return 0;
    *out = (unsigned short)v;
    *p   = end;
    return 1;
}

static int Expect(const char** p, char c)
{
    if (**p != c) return 0;
    (*p)++;
    return 1;
}

/* Parse a texupload file title (basename without extension). Returns 1 on
 * success. Format documented in tex_pack.h; 16bpp names omit the palette
 * hash and P-range fields. Anchored: trailing junk (e.g. upscaler-tool
 * suffixes) rejects the name. */
static int ParseName(const char* title, PackEntry* e)
{
    static const char* modeNames[8] = { "P4", "P8", "C16", "C16",
                                        "STP4", "STP8", "STC16", "STC16" };
    const char* p;
    char modeTok[8];
    size_t modeLen;
    int m;

    if (strncmp(title, "texpage-", 8) == 0)
    {
        g_texpageSkipped++;
        return 0;
    }
    if (strncmp(title, "texupload-", 10) != 0) return 0;
    p = title + 10;

    {
        const char* dash = strchr(p, '-');
        if (dash == NULL) return 0;
        modeLen = (size_t)(dash - p);
        if (modeLen == 0 || modeLen >= sizeof(modeTok)) return 0;
        memcpy(modeTok, p, modeLen);
        modeTok[modeLen] = '\0';
        p = dash + 1;
    }

    e->mode = 0xFF;
    for (m = 0; m < 8; m++)
    {
        if (strcmp(modeTok, modeNames[m]) == 0)
        {
            e->mode = (unsigned char)m;
            break;
        }
    }
    if (e->mode == 0xFF) return 0;

    if (!ParseHex64(&p, &e->srcHash)) return 0;

    if (Mode_Bpp(e->mode) != 16)
    {
        unsigned short palMin, palMax;
        if (!Expect(&p, '-') || !ParseHex64(&p, &e->palHash)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->srcW)) return 0;
        if (!Expect(&p, 'x') || !ParseDec16(&p, &e->srcH)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->offX)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->offY)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->subW)) return 0;
        if (!Expect(&p, 'x') || !ParseDec16(&p, &e->subH)) return 0;
        if (!Expect(&p, '-') || !Expect(&p, 'P') || !ParseDec16(&p, &palMin)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &palMax)) return 0;
        if (palMin > 255) return 0;
        e->palMin = (unsigned char)palMin;
        e->palMax = palMax;
    }
    else
    {
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->srcW)) return 0;
        if (!Expect(&p, 'x') || !ParseDec16(&p, &e->srcH)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->offX)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->offY)) return 0;
        if (!Expect(&p, '-') || !ParseDec16(&p, &e->subW)) return 0;
        if (!Expect(&p, 'x') || !ParseDec16(&p, &e->subH)) return 0;
        e->palHash = 0;
        e->palMin  = 0;
        e->palMax  = 0;
    }

    if (*p != '\0') return 0;

    return e->srcW != 0 && e->srcH != 0 && e->subW != 0 && e->subH != 0;
}

static void Entry_Add(const PackEntry* e)
{
    if (g_entryCount == g_entryCap)
    {
        g_entryCap = g_entryCap ? g_entryCap * 2 : 1024;
        g_entries  = (PackEntry*)realloc(g_entries, (size_t)g_entryCap * sizeof(PackEntry));
    }
    g_entries[g_entryCount]     = *e;
    g_entries[g_entryCount].seq = (unsigned int)g_entryCount;
    g_entryCount++;
}

/* File title = basename minus a final .png/.PNG. Returns 0 when the name is
 * not a png. `out` must hold >= 160 chars. */
static int FileTitle(const char* name, char* out, size_t outSize)
{
    size_t len = strlen(name);
    if (len < 5 || len >= outSize) return 0;
    if (_stricmp(name + len - 4, ".png") != 0) return 0;
    memcpy(out, name, len - 4);
    out[len - 4] = '\0';
    return 1;
}

static void Scan_LoosePng(const char* path, const char* name)
{
    char      title[160];
    PackEntry e;

    if (!FileTitle(name, title, sizeof(title))) return;
    memset(&e, 0, sizeof(e));
    if (!ParseName(title, &e)) return;

    e.zipIdx    = -1;
    e.loosePath = _strdup(path);
    Entry_Add(&e);
}

static void Scan_Zip(const char* path)
{
    mz_zip_archive zip;
    mz_uint        i, n;
    short          zipIdx;
    int            added = 0;

    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0))
    {
        SH_DBG("[TEXPACK] %s: not a readable zip", path);
        return;
    }

    if (g_zipCount == g_zipCap)
    {
        g_zipCap  = g_zipCap ? g_zipCap * 2 : 8;
        g_zipPaths = (char**)realloc(g_zipPaths, (size_t)g_zipCap * sizeof(char*));
    }
    zipIdx = (short)g_zipCount;
    g_zipPaths[g_zipCount++] = _strdup(path);

    n = mz_zip_reader_get_num_files(&zip);
    for (i = 0; i < n; i++)
    {
        char      entryName[512];
        char      title[160];
        PackEntry e;
        const char* base;

        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (mz_zip_reader_get_filename(&zip, i, entryName, sizeof(entryName)) == 0) continue;

        base = strrchr(entryName, '/');
        base = base ? base + 1 : entryName;
        if (!FileTitle(base, title, sizeof(title))) continue;
        memset(&e, 0, sizeof(e));
        if (!ParseName(title, &e)) continue;

        e.zipIdx    = zipIdx;
        e.zipEntry  = (unsigned int)i;
        e.loosePath = NULL;
        Entry_Add(&e);
        added++;
    }
    mz_zip_reader_end(&zip);

    SH_DBG("[TEXPACK] %s: %d entries", path, added);
}

/* Mod-manager "disabled" marker suffix: a texturemods entry (folder or .zip)
 * renamed "<name>.disabled" is inactive and must be skipped entirely. */
static int Name_IsDisabled(const char* name)
{
    size_t n = strlen(name);
    return n >= 9 && _stricmp(name + n - 9, ".disabled") == 0;
}

/* Index loose PNGs, subfolders, and .zip packs in place. .zip archives are read
 * directly (miniz) — nothing is extracted to disk. .rar is unsupported; convert
 * packs to .zip or a loose folder. */
static void Scan_Dir(const char* dirPath, int depth)
{
    DIR*           dir;
    struct dirent* de;

    if (depth > 8) return;

    dir = opendir(dirPath);
    if (dir == NULL) return;

    while ((de = readdir(dir)) != NULL)
    {
        char        path[512];
        struct stat st;
        size_t      nameLen = strlen(de->d_name);

        if (de->d_name[0] == '.') continue;
        if (Name_IsDisabled(de->d_name)) continue; /* mod-manager disabled */
        snprintf(path, sizeof(path), "%s/%s", dirPath, de->d_name);
        if (stat(path, &st) != 0) continue;

        if (st.st_mode & S_IFDIR)
        {
            Scan_Dir(path, depth + 1);
        }
        else if (nameLen > 4 && _stricmp(de->d_name + nameLen - 4, ".zip") == 0)
        {
            Scan_Zip(path);
        }
        else
        {
            Scan_LoosePng(path, de->d_name);
        }
    }
    closedir(dir);
}

static int Entry_CompareSrcHash(const void* a, const void* b)
{
    const PackEntry* ea = (const PackEntry*)a;
    const PackEntry* eb = (const PackEntry*)b;
    if (ea->srcHash < eb->srcHash) return -1;
    if (ea->srcHash > eb->srcHash) return 1;
    /* Same source texture: lower priority first so the higher-priority pack's
     * sub-rect blits LAST (wins). seq is the final tiebreak so the order is
     * total — qsort is not guaranteed stable. */
    if (ea->priority < eb->priority) return -1;
    if (ea->priority > eb->priority) return 1;
    if (ea->seq < eb->seq) return -1;
    if (ea->seq > eb->seq) return 1;
    return 0;
}

static void Scan_Once(void)
{
    if (g_scanned) return;
    g_scanned = 1;

    if (!g_PcConfig.texturePacks) return;

    Scan_Dir(TEXPACK_DIR, 0);

    LoadOrder_Read();
    if (g_loadOrderCount > 0)
    {
        int  i;
        char modName[160];
        for (i = 0; i < g_entryCount; i++)
        {
            const char* path = (g_entries[i].zipIdx < 0)
                                   ? g_entries[i].loosePath
                                   : g_zipPaths[g_entries[i].zipIdx];
            Pack_ModName(path, modName, sizeof(modName));
            g_entries[i].priority = LoadOrder_Priority(modName);
        }
        SH_DBG("[TEXPACK] loadorder.txt: %d packs ranked", g_loadOrderCount);
    }

    if (g_entryCount > 0)
    {
        qsort(g_entries, (size_t)g_entryCount, sizeof(PackEntry), Entry_CompareSrcHash);
        SH_DBG("[TEXPACK] indexed %d replacement entries (%d zip packs)",
               g_entryCount, g_zipCount);
    }
    if (g_texpageSkipped > 0)
    {
        SH_DBG("[TEXPACK] skipped %d texpage-* entries (page-mode replacements unsupported; texupload-* only)",
               g_texpageSkipped);
    }
}

int TexPack_HasEntries(void)
{
    Scan_Once();
    return g_entryCount > 0;
}

/* First index in g_entries with srcHash >= hash. */
static int Entry_LowerBound(unsigned long long hash)
{
    int lo = 0, hi = g_entryCount;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (g_entries[mid].srcHash < hash) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static unsigned char* Entry_LoadPng(const PackEntry* e, int* outW, int* outH)
{
    unsigned char* data = NULL;
    size_t         size = 0;
    unsigned char* rgba = NULL;

    if (e->zipIdx < 0)
    {
        FILE* f = fopen(e->loosePath, "rb");
        long  sz;
        if (f == NULL) return NULL;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 256 * 1024 * 1024)
        {
            data = (unsigned char*)malloc((size_t)sz);
            if (data != NULL && fread(data, 1, (size_t)sz, f) == (size_t)sz)
            {
                size = (size_t)sz;
            }
            else
            {
                free(data);
                data = NULL;
            }
        }
        fclose(f);
    }
    else
    {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        if (mz_zip_reader_init_file(&zip, g_zipPaths[e->zipIdx], 0))
        {
            data = (unsigned char*)mz_zip_reader_extract_to_heap(&zip, e->zipEntry, &size, 0);
            mz_zip_reader_end(&zip);
        }
    }

    if (data == NULL) return NULL;

    {
        int comp = 0;
        rgba = stbi_load_from_memory(data, (int)size, outW, outH, &comp, 4);
    }
    if (e->zipIdx < 0) free(data);
    else mz_free(data);
    return rgba;
}

static void bgr555_px(unsigned short cx, unsigned char* o)
{
    int r5 = cx & 0x1F, g5 = (cx >> 5) & 0x1F, b5 = (cx >> 10) & 0x1F;
    o[0] = (unsigned char)((r5 << 3) | (r5 >> 2));
    o[1] = (unsigned char)((g5 << 3) | (g5 >> 2));
    o[2] = (unsigned char)((b5 << 3) | (b5 >> 2));
    o[3] = (cx == 0) ? 0 : 255;
}

/* Decode the native upload to RGBA (nativeW x h). */
static unsigned char* DecodeNative(const unsigned char* pixels, int w16, int h,
                                   const unsigned short* clut, int clutCount,
                                   int bpp, int nativeW)
{
    unsigned char* rgba = (unsigned char*)malloc((size_t)nativeW * (size_t)h * 4);
    int x, y;

    if (rgba == NULL) return NULL;

    for (y = 0; y < h; y++)
    {
        const unsigned char* row = pixels + (size_t)y * (size_t)w16 * 2;
        for (x = 0; x < nativeW; x++)
        {
            unsigned char* o = &rgba[((size_t)y * (size_t)nativeW + (size_t)x) * 4];
            unsigned short cx;
            unsigned int   idx;

            switch (bpp)
            {
                case 4:
                    idx = (row[x >> 1] >> ((x & 1) * 4)) & 0xF;
                    cx  = (idx < (unsigned int)clutCount) ? clut[idx] : 0;
                    break;
                case 8:
                    idx = row[x];
                    cx  = (idx < (unsigned int)clutCount) ? clut[idx] : 0;
                    break;
                default:
                    cx = (unsigned short)(row[x * 2] | (row[x * 2 + 1] << 8));
                    break;
            }
            bgr555_px(cx, o);
        }
    }
    return rgba;
}

/* Nearest blit of src (srcW x srcH) into dst at (dx, dy, dw, dh). */
static void BlitNearest(unsigned char* dst, int dstW, int dstH,
                        int dx, int dy, int dw, int dh,
                        const unsigned char* src, int srcW, int srcH)
{
    int x, y;
    for (y = 0; y < dh; y++)
    {
        int ty = dy + y;
        int sy = (int)(((long long)y * srcH) / dh);
        if (ty < 0 || ty >= dstH) continue;
        for (x = 0; x < dw; x++)
        {
            int tx = dx + x;
            int sx = (int)(((long long)x * srcW) / dw);
            if (tx < 0 || tx >= dstW) continue;
            memcpy(&dst[((size_t)ty * (size_t)dstW + (size_t)tx) * 4],
                   &src[((size_t)sy * (size_t)srcW + (size_t)sx) * 4], 4);
        }
    }
}

/* ---- Composed-canvas cache -------------------------------------------------
 * TexPack_Compose runs on EVERY TIM upload that matches a pack entry, and
 * exterior streaming re-uploads the same chunk TIMs endlessly as they churn
 * through pool slots — re-doing the zip-open + PNG decode + composite each
 * time was the reported pack stutter. A composite is a pure function of
 * (pixel hash, palette hash, bpp), so results are cached content-keyed with
 * LRU eviction, capped by `texpack_cache_mb` (0 disables). The cache OWNS
 * every returned canvas — callers must not free it. */
#define TP_CACHE_MAX 512

typedef struct {
    unsigned long long srcHash;
    unsigned long long palHash;
    int                bpp;
    unsigned char*     rgba;
    int                w, h;
    size_t             bytes;
    unsigned           tick; /* LRU stamp */
} TpCacheEnt;

static TpCacheEnt g_tpCache[TP_CACHE_MAX];
static int        g_tpCacheCount = 0;
static size_t     g_tpCacheBytes = 0;
static unsigned   g_tpCacheTick  = 0;
static unsigned   g_tpCacheHits = 0, g_tpCacheMisses = 0;

/* Canvas kept alive until the next compose when caching is off/overflowing. */
static unsigned char* g_tpTransient = NULL;

static void tp_cache_evict_lru(void)
{
    int i, oldest = 0;
    for (i = 1; i < g_tpCacheCount; i++)
        if (g_tpCache[i].tick < g_tpCache[oldest].tick) oldest = i;
    free(g_tpCache[oldest].rgba);
    g_tpCacheBytes -= g_tpCache[oldest].bytes;
    g_tpCache[oldest] = g_tpCache[--g_tpCacheCount];
}

const unsigned char* TexPack_Compose(const unsigned char* pixels, int w16, int h,
                                     const unsigned short* clut, int clutCount,
                                     int bpp, int* outW, int* outH)
{
    unsigned long long srcHash;
    unsigned long long fullPalHash = 0;
    int                fullPalMax;
    int                i, first, matchCount = 0;
    int                matches[64];
    float              scaleX = 1.0f, scaleY = 1.0f;
    int                nativeW, canvasW, canvasH;
    unsigned char*     canvas;
    size_t             cacheCap;

    Scan_Once();
    if (g_entryCount == 0 || pixels == NULL || w16 <= 0 || h <= 0) return NULL;

    if (g_tpTransient != NULL)
    {
        free(g_tpTransient);
        g_tpTransient = NULL;
    }

    srcHash = XXH3_64bits(pixels, (size_t)w16 * (size_t)h * 2);

    fullPalMax = (bpp == 4) ? 15 : 255;
    if (bpp != 16 && clut != NULL && clutCount > 0)
    {
        int n = (bpp == 4) ? 16 : 256;
        if (n > clutCount) n = clutCount;
        fullPalHash = XXH3_64bits(clut, (size_t)n * 2);
    }

    cacheCap = (size_t)g_PcConfig.texpackCacheMb << 20;
    if (cacheCap > 0)
    {
        for (i = 0; i < g_tpCacheCount; i++)
        {
            TpCacheEnt* c = &g_tpCache[i];
            if (c->srcHash == srcHash && c->palHash == fullPalHash && c->bpp == bpp)
            {
                c->tick = ++g_tpCacheTick;
                g_tpCacheHits++;
                *outW = c->w;
                *outH = c->h;
                return c->rgba;
            }
        }
    }

    first = Entry_LowerBound(srcHash);
    for (i = first; i < g_entryCount && g_entries[i].srcHash == srcHash; i++)
    {
        const PackEntry* e = &g_entries[i];
        if (Mode_Bpp(e->mode) != bpp) continue;

        if (bpp != 16)
        {
            if (clut == NULL || clutCount <= 0) continue;
            if (e->palMin == 0 && e->palMax == fullPalMax)
            {
                if (e->palHash != fullPalHash) continue;
            }
            else
            {
                /* DuckStation's partial-palette quirk: hash the FIRST
                 * palMax-palMin+1 entries, not the [min..max] window. */
                int n = (int)e->palMax - (int)e->palMin + 1;
                if (n <= 0 || n > clutCount) continue;
                if (XXH3_64bits(clut, (size_t)n * 2) != e->palHash) continue;
            }
        }

        if (matchCount < (int)(sizeof(matches) / sizeof(matches[0])))
        {
            matches[matchCount++] = i;
        }
    }

    if (matchCount == 0) return NULL;

    nativeW = w16 * (16 / bpp);

    /* Decode every matching PNG once; the pack scale is the largest
     * PNG-to-subrect ratio among them (uniform in practice). */
    {
        unsigned char* pngs[64];
        int pngW[64], pngH[64];
        int anyPng = 0;

        for (i = 0; i < matchCount; i++)
        {
            pngs[i] = Entry_LoadPng(&g_entries[matches[i]], &pngW[i], &pngH[i]);
            if (pngs[i] != NULL)
            {
                const PackEntry* e = &g_entries[matches[i]];
                float sx = (float)pngW[i] / (float)e->subW;
                float sy = (float)pngH[i] / (float)e->subH;
                if (sx > scaleX) scaleX = sx;
                if (sy > scaleY) scaleY = sy;
                anyPng = 1;
            }
        }
        if (scaleX > 16.0f) scaleX = 16.0f;
        if (scaleY > 16.0f) scaleY = 16.0f;

        canvasW = (int)ceilf((float)nativeW * scaleX);
        canvasH = (int)ceilf((float)h * scaleY);

        canvas = NULL;
        if (anyPng && canvasW > 0 && canvasH > 0 && canvasW <= 16384 && canvasH <= 16384)
        {
            /* Base layer: the native upload, nearest-upscaled — sub-rects
             * the pack doesn't cover keep the original look, like
             * DuckStation's compositor. */
            unsigned char* native = DecodeNative(pixels, w16, h, clut, clutCount, bpp, nativeW);
            if (native != NULL)
            {
                canvas = (unsigned char*)malloc((size_t)canvasW * (size_t)canvasH * 4);
                if (canvas != NULL)
                {
                    BlitNearest(canvas, canvasW, canvasH, 0, 0, canvasW, canvasH, native, nativeW, h);
                }
                free(native);
            }
        }

        for (i = 0; i < matchCount; i++)
        {
            if (pngs[i] == NULL) continue;
            if (canvas != NULL)
            {
                const PackEntry* e = &g_entries[matches[i]];
                int dx = (int)((float)e->offX * scaleX);
                int dy = (int)((float)e->offY * scaleY);
                int dw = (int)ceilf((float)e->subW * scaleX);
                int dh = (int)ceilf((float)e->subH * scaleY);
                BlitNearest(canvas, canvasW, canvasH, dx, dy, dw, dh, pngs[i], pngW[i], pngH[i]);
            }
            stbi_image_free(pngs[i]);
        }
        if (canvas == NULL) return NULL;
    }

    {
        static int s_composeLog = 0;
        if (s_composeLog < 256)
        {
            SH_DBG("[TEXPACK] composed %dx%d for upload %016llX (%d sub-image%s, %dbpp)",
                   canvasW, canvasH, srcHash, matchCount, matchCount == 1 ? "" : "s", bpp);
            s_composeLog++;
        }
    }

    /* Hand the canvas to the cache (it owns every returned pointer). If the
     * cache is disabled or this canvas alone exceeds the cap, park it in the
     * transient slot instead — freed on the next compose. */
    {
        size_t bytes = (size_t)canvasW * (size_t)canvasH * 4;

        g_tpCacheMisses++;
        if (cacheCap > 0 && bytes <= cacheCap)
        {
            while (g_tpCacheCount > 0 &&
                   (g_tpCacheBytes + bytes > cacheCap || g_tpCacheCount >= TP_CACHE_MAX))
            {
                tp_cache_evict_lru();
            }
            if (g_tpCacheCount < TP_CACHE_MAX)
            {
                TpCacheEnt* c = &g_tpCache[g_tpCacheCount++];
                c->srcHash = srcHash;
                c->palHash = fullPalHash;
                c->bpp     = bpp;
                c->rgba    = canvas;
                c->w       = canvasW;
                c->h       = canvasH;
                c->bytes   = bytes;
                c->tick    = ++g_tpCacheTick;
                g_tpCacheBytes += bytes;
            }
            else
            {
                g_tpTransient = canvas;
            }
        }
        else
        {
            g_tpTransient = canvas;
        }

        if ((g_tpCacheMisses & 63) == 0)
        {
            SH_DBG("[TEXPACK] cache: %u hits / %u composes, %d entries, %u MB",
                   g_tpCacheHits, g_tpCacheMisses, g_tpCacheCount,
                   (unsigned)(g_tpCacheBytes >> 20));
        }
    }

    *outW = canvasW;
    *outH = canvasH;
    return canvas;
}
