#include "tex_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
/* FindFirstFileEx: directory entries carry their attributes, so indexing a pack
 * tree needs no per-name stat() (see Scan_Dir). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
#include "dds_load.h" /* BC7 .dds whole-upload pack entries */

#ifndef TEXPACK_DIR
#define TEXPACK_DIR "gamedata/texturemods"
#endif

#ifndef TEXDUMP_DIR
#define TEXDUMP_DIR "gamedata/dump"
#endif

#ifdef _WIN32
/* Declared here rather than via <direct.h>, which is MSVC-only. */
int _mkdir(const char* dirname);
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
    unsigned char  isDds;        /* 1 = BC7 .dds entry (uploaded compressed, whole-upload only) */
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

/* File title = basename minus a final .png or .dds (case-insensitive). Returns
 * 0 when the name is neither; sets *isDds. `out` must hold >= 160 chars. A BC7
 * .dds pack entry keys on the same texupload-… hash name as its .png twin. */
static int FileTitle(const char* name, char* out, size_t outSize, int* isDds)
{
    size_t len = strlen(name);
    int    dds;
    if (len < 5 || len >= outSize) return 0;
    dds = (_stricmp(name + len - 4, ".dds") == 0);
    if (!dds && _stricmp(name + len - 4, ".png") != 0) return 0;
    memcpy(out, name, len - 4);
    out[len - 4] = '\0';
    if (isDds != NULL) *isDds = dds;
    return 1;
}

static void Scan_LoosePng(const char* path, const char* name)
{
    char      title[160];
    PackEntry e;
    int       isDds = 0;

    if (!FileTitle(name, title, sizeof(title), &isDds)) return;
    memset(&e, 0, sizeof(e));
    if (!ParseName(title, &e)) return;

    e.isDds     = (unsigned char)isDds;
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
        {
            int isDds = 0;
            if (!FileTitle(base, title, sizeof(title), &isDds)) continue;
            memset(&e, 0, sizeof(e));
            if (!ParseName(title, &e)) continue;
            e.isDds = (unsigned char)isDds;
        }

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

static void Scan_Dir(const char* dirPath, int depth);

/* One directory entry, with the is-it-a-directory answer already supplied by
 * the enumeration (see Scan_Dir). */
static void Scan_Entry(const char* dirPath, const char* name, int isDir, int depth)
{
    char   path[512];
    size_t nameLen = strlen(name);

    if (name[0] == '.') return;
    if (Name_IsDisabled(name)) return; /* mod-manager disabled */
    snprintf(path, sizeof(path), "%s/%s", dirPath, name);

    if (isDir)
    {
        Scan_Dir(path, depth + 1);
    }
    else if (nameLen > 4 && _stricmp(name + nameLen - 4, ".zip") == 0)
    {
        /* The launcher's Mod Manager extracts .zip packs to a sibling
         * "<zip>.extracted" folder (like .rar) and toggles that folder,
         * not the zip file. When such a folder exists, the folder is the
         * source of truth — reading the zip in place too would double-index
         * it (and, if the folder is ".disabled", override the disable).
         * A zip hand-dropped WITHOUT the launcher has no sibling folder and
         * is still read in place here. */
        char        ext[512];
        struct stat exst;
        int         handled = 0;

        snprintf(ext, sizeof(ext), "%s.extracted", path);
        if (stat(ext, &exst) == 0 && (exst.st_mode & S_IFDIR)) handled = 1;
        if (!handled)
        {
            snprintf(ext, sizeof(ext), "%s.extracted.disabled", path);
            if (stat(ext, &exst) == 0 && (exst.st_mode & S_IFDIR)) handled = 1;
        }
        if (!handled) Scan_Zip(path);
    }
    else
    {
        Scan_LoosePng(path, name);
    }
}

/* Index loose PNGs, subfolders, and .zip packs in place. .zip archives are read
 * directly (miniz) — nothing is extracted to disk. .rar is unsupported; convert
 * packs to .zip or a loose folder.
 *
 * The is-it-a-directory answer must come from the directory entry itself: a
 * stat() per name cost 1052ms of the 1104ms spent indexing a 12,227-file pack
 * tree at startup, and the S_IFDIR bit was the only thing read from it. */
static void Scan_Dir(const char* dirPath, int depth)
{
    if (depth > 8) return;

#ifdef _WIN32
    {
        char             pattern[512];
        WIN32_FIND_DATAA fd;
        HANDLE           h;

        snprintf(pattern, sizeof(pattern), "%s/*", dirPath);
        h = FindFirstFileExA(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch,
                             NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) return;

        do
        {
            int isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            /* Symlinks and junctions: stat() follows the link, so the target
             * decides, and a dangling one stays unindexed exactly as before.
             * Reparse points are rare enough to pay for a stat() here. */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                char        path[512];
                struct stat st;
                snprintf(path, sizeof(path), "%s/%s", dirPath, fd.cFileName);
                if (stat(path, &st) != 0) continue;
                isDir = (st.st_mode & S_IFDIR) != 0;
            }

            Scan_Entry(dirPath, fd.cFileName, isDir, depth);
        } while (FindNextFileA(h, &fd));

        FindClose(h);
    }
#else
    {
        DIR*           dir;
        struct dirent* de;

        dir = opendir(dirPath);
        if (dir == NULL) return;

        while ((de = readdir(dir)) != NULL)
        {
            int isDir;
#if defined(_DIRENT_HAVE_D_TYPE) || defined(DT_DIR)
            if (de->d_type == DT_DIR)
            {
                isDir = 1;
            }
            else if (de->d_type == DT_REG)
            {
                isDir = 0;
            }
            else
#endif
            {
                /* DT_UNKNOWN (some filesystems) or a symlink — ask the OS. */
                char        path[512];
                struct stat st;
                snprintf(path, sizeof(path), "%s/%s", dirPath, de->d_name);
                if (stat(path, &st) != 0) continue;
                isDir = (st.st_mode & S_IFDIR) != 0;
            }

            Scan_Entry(dirPath, de->d_name, isDir, depth);
        }
        closedir(dir);
    }
#endif
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

/* Load a pack entry's raw file bytes (loose fread or zip extract) into a
 * malloc'd buffer the caller frees with free(). Used for BC7 .dds, whose
 * blocks are handed to the GL uploader as-is rather than stbi-decoded. */
static unsigned char* Entry_LoadRaw(const PackEntry* e, size_t* outSize)
{
    unsigned char* out = NULL;

    *outSize = 0;

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
            out = (unsigned char*)malloc((size_t)sz);
            if (out != NULL && fread(out, 1, (size_t)sz, f) == (size_t)sz)
                *outSize = (size_t)sz;
            else { free(out); out = NULL; }
        }
        fclose(f);
    }
    else
    {
        mz_zip_archive zip;
        memset(&zip, 0, sizeof(zip));
        if (mz_zip_reader_init_file(&zip, g_zipPaths[e->zipIdx], 0))
        {
            size_t size = 0;
            void*  data = mz_zip_reader_extract_to_heap(&zip, e->zipEntry, &size, 0);
            mz_zip_reader_end(&zip);
            if (data != NULL)
            {
                out = (unsigned char*)malloc(size);
                if (out != NULL) { memcpy(out, data, size); *outSize = size; }
                mz_free(data);
            }
        }
    }
    return out;
}

/* Decode one pack entry to RGBA8. .png goes through stb_image, .dds through the
 * BC7 CPU decoder (mip 0) — both yield the same (rgba, w, h) the compositor
 * wants. The two allocators are not interchangeable, so *outStbi reports which
 * free() the buffer needs; NULL means the caller does not care. */
static unsigned char* Entry_LoadImage(const PackEntry* e, int* outW, int* outH, int* outStbi)
{
    unsigned char* data = NULL;
    size_t         size = 0;
    unsigned char* rgba = NULL;

    if (outStbi != NULL) *outStbi = 1;

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

    /* Sniff the magic rather than trusting the extension: the file decides. */
    if (size >= 4 && data[0] == 'D' && data[1] == 'D' && data[2] == 'S' && data[3] == ' ')
    {
        rgba = Dds_DecodeRgba(data, (int)size, outW, outH);
        if (outStbi != NULL) *outStbi = 0;
    }
    else
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

/* Nearest blit of src (srcW x srcH) into dst at (dx, dy, dw, dh).
 *
 * The base-layer upscale below is 85-90% of the compose CPU cost, so this is
 * split into two paths that produce byte-identical output to the naive
 * per-pixel `sx = x * srcW / dw` loop it replaces (verified by an exhaustive
 * OLD-vs-NEW harness over integer/non-integer ratios, tiny and oversized
 * sources, and every clipping direction). */
static void BlitNearest(unsigned char* dst, int dstW, int dstH,
                        int dx, int dy, int dw, int dh,
                        const unsigned char* src, int srcW, int srcH)
{
    int x, y, x0, x1, y0, y1;

    if (dw <= 0 || dh <= 0 || srcW <= 0 || srcH <= 0) return;

    /* Exact integer upscale landing wholly inside the canvas — the pack case
     * (always a whole 4x). Every source texel maps to a kx*ky block, so one
     * destination row is built per source row with 32-bit stores and then
     * replicated: no divides, no bounds test, no 4-byte memcpy per pixel. */
    if (dw % srcW == 0 && dh % srcH == 0 &&
        dx >= 0 && dy >= 0 && dx + dw <= dstW && dy + dh <= dstH)
    {
        int    kx       = dw / srcW;
        int    ky       = dh / srcH;
        size_t rowBytes = (size_t)dw * 4;
        int    sy;

        for (sy = 0; sy < srcH; sy++)
        {
            unsigned int*       o  = (unsigned int*)&dst[((size_t)(dy + sy * ky) * (size_t)dstW + (size_t)dx) * 4];
            const unsigned int* in = (const unsigned int*)&src[(size_t)sy * (size_t)srcW * 4];
            int                 sx, k, w = 0;

            for (sx = 0; sx < srcW; sx++)
            {
                unsigned int px = in[sx];
                for (k = 0; k < kx; k++) o[w++] = px;
            }
            for (k = 1; k < ky; k++)
            {
                memcpy(&dst[((size_t)(dy + sy * ky + k) * (size_t)dstW + (size_t)dx) * 4],
                       o, rowBytes);
            }
        }
        return;
    }

    /* Clip the loop bounds once instead of testing every pixel. */
    y0 = (dy < 0) ? -dy : 0;
    y1 = (dstH - dy < dh) ? (dstH - dy) : dh;
    x0 = (dx < 0) ? -dx : 0;
    x1 = (dstW - dx < dw) ? (dstW - dx) : dw;
    if (x1 <= x0 || y1 <= y0) return;

    /* sx is loop-invariant across rows: pay for the divide once per column. */
    {
        static int* s_col    = NULL;
        static int  s_colCap = 0;
        int         need     = x1 - x0;

        if (need > s_colCap)
        {
            int* nc = (int*)realloc(s_col, (size_t)need * sizeof(int));
            if (nc != NULL)
            {
                s_col    = nc;
                s_colCap = need;
            }
        }
        if (s_col != NULL && s_colCap >= need)
        {
            for (x = x0; x < x1; x++)
                s_col[x - x0] = (int)(((long long)x * srcW) / dw);

            for (y = y0; y < y1; y++)
            {
                int                 sy = (int)(((long long)y * srcH) / dh);
                unsigned int*       o  = (unsigned int*)&dst[((size_t)(dy + y) * (size_t)dstW + (size_t)(dx + x0)) * 4];
                const unsigned int* in = (const unsigned int*)&src[(size_t)sy * (size_t)srcW * 4];
                for (x = 0; x < need; x++) o[x] = in[s_col[x]];
            }
            return;
        }
    }

    /* Column table unavailable (allocation failed) — per-pixel divide. */
    for (y = y0; y < y1; y++)
    {
        int sy = (int)(((long long)y * srcH) / dh);
        for (x = x0; x < x1; x++)
        {
            int sx = (int)(((long long)x * srcW) / dw);
            memcpy(&dst[((size_t)(dy + y) * (size_t)dstW + (size_t)(dx + x)) * 4],
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
#define TP_CACHE_MAX 4096 /* entry-count cap alongside texpack_cache_mb; big packs churn many canvases */

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

/* Content key of the most recent compose (pixels+palette+bpp folded to u64),
 * for the pool registrars' redundant-upload skip. See TexPack_LastComposeHash. */
static unsigned long long g_tpLastHash = 0;

unsigned long long TexPack_LastComposeHash(void)
{
    return g_tpLastHash;
}

/* BC7 .dds whole-upload result of the most recent compose. When set, compose
 * returned NULL (no RGBA canvas) and the upload site uploads these file bytes
 * compressed instead. Bytes are malloc'd and owned here — freed at the start
 * of the next compose (the site consumes them before that runs). */
static int            g_tpLastIsDds = 0;
static unsigned char* g_tpDdsBytes  = NULL;
static size_t         g_tpDdsSize   = 0;

int TexPack_LastComposeIsDds(void)
{
    return g_tpLastIsDds;
}

/* 1 when the most recent compose paid the decode+upscale (or .dds read) cost,
 * 0 when it hit the compose cache or matched no pack entry. Drives the caller's
 * per-frame compose budget: a cache hit must not consume it. */
static int g_tpLastBuilt = 0;

int TexPack_LastComposeWasBuilt(void)
{
    return g_tpLastBuilt;
}

const unsigned char* TexPack_LastComposeDds(size_t* outSize)
{
    if (outSize != NULL) *outSize = g_tpDdsSize;
    return g_tpDdsBytes;
}

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

    g_tpLastHash  = 0;
    g_tpLastIsDds = 0;
    g_tpLastBuilt = 0;
    if (g_tpDdsBytes != NULL)
    {
        free(g_tpDdsBytes);
        g_tpDdsBytes = NULL;
        g_tpDdsSize  = 0;
    }

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

    /* Fold the same identity the cache keys on into one u64 for the pool
     * registrars. hash_combine so (src=A,pal=B) != (src=B,pal=A). */
    g_tpLastHash = srcHash ^ (fullPalHash + 0x9E3779B97F4A7C15ULL +
                              (srcHash << 6) + (srcHash >> 2)) ^ (unsigned long long)bpp;

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

    /* Past this point every exit has read and decoded pack files. */
    g_tpLastBuilt = 1;

    nativeW = w16 * (16 / bpp);

    /* Exact twins: the SAME pack file present in both formats. Scan_Dir walks
     * subfolders, so converting a pack in place — or parking the .dds in a
     * "DDS/" folder beside the .png it came from — indexes BOTH copies (the
     * 12,227-file SLES-01514 pack indexes 24,452 entries that way). They carry
     * one parsed name, so they blit the same rect with the same pixels and only
     * the directory enumeration order decides which lands last: the pack costs
     * double the decode work for a result that is a coin flip. Drop the .png
     * twin — .dds wins on every other load path (fsqueue_3.c probes .dds first)
     * and the whole-cover collapse below already assumes it. A twin is only a
     * twin at EQUAL priority, so a higher-priority .png from a different pack
     * still wins on load order. */
    if (matchCount > 1)
    {
        int a, b, n = 0;
        for (a = 0; a < matchCount; a++)
        {
            const PackEntry* ea  = &g_entries[matches[a]];
            int              dup = 0;

            if (!ea->isDds)
            {
                for (b = 0; b < matchCount; b++)
                {
                    const PackEntry* eb = &g_entries[matches[b]];
                    if (!eb->isDds || eb->priority != ea->priority) continue;
                    if (eb->mode == ea->mode &&
                        eb->offX == ea->offX && eb->offY == ea->offY &&
                        eb->subW == ea->subW && eb->subH == ea->subH &&
                        eb->palMin == ea->palMin && eb->palMax == ea->palMax &&
                        eb->palHash == ea->palHash)
                    {
                        dup = 1;
                        break;
                    }
                }
            }
            if (!dup) matches[n++] = matches[a];
        }
        matchCount = n;
    }

    /* DDS-first: a pack shipping both FOO.dds and FOO.png (converter run with
     * "keep both", or a pre-mixed pack) matches BOTH for this upload, which
     * used to leave matchCount > 1 — skipping the compressed fast path below,
     * so the .png silently won. When a whole-cover .dds is present and every
     * other match is a whole-cover twin of no HIGHER load-order priority,
     * collapse to the .dds alone. A higher-priority whole-cover .png from a
     * DIFFERENT pack still wins (load order), and a sub-rect match keeps every
     * entry on the composited path (both formats decode there). matches[] is in
     * ascending (priority, seq) order — scan from the end for the winner. */
    if (matchCount > 1)
    {
        int di;
        for (di = matchCount - 1; di >= 0; di--)
        {
            const PackEntry* d = &g_entries[matches[di]];
            if (d->isDds && d->offX == 0 && d->offY == 0 &&
                (int)d->subW == nativeW && (int)d->subH == h)
                break;
        }
        if (di >= 0)
        {
            int allTwins = 1;
            for (i = 0; i < matchCount; i++)
            {
                const PackEntry* t = &g_entries[matches[i]];
                if (i == di) continue;
                if (t->offX != 0 || t->offY != 0 ||
                    (int)t->subW != nativeW || (int)t->subH != h ||
                    t->priority > g_entries[matches[di]].priority)
                {
                    allTwins = 0;
                    break;
                }
            }
            if (allTwins)
            {
                matches[0] = matches[di];
                matchCount = 1;
            }
        }
    }

    /* BC7 .dds whole-upload fast path: a single .dds entry covering the entire
     * native texture is uploaded compressed — the compositor can't blit BC7
     * blocks, and a whole-cover has nothing to blit over. Signal it via
     * TexPack_LastComposeIsDds(); the upload site hands the file to Dds_Upload.
     * A partial or multi-entry .dds cannot take this path — the compositor has
     * to blit 32-bit pixels — so it falls through to the RGBA path below, where
     * Entry_LoadImage decodes its BC7 blocks on the CPU. */
    if (matchCount == 1 && g_entries[matches[0]].isDds)
    {
        const PackEntry* e = &g_entries[matches[0]];
        if (e->offX == 0 && e->offY == 0 &&
            (int)e->subW == nativeW && (int)e->subH == h)
        {
            size_t         rawSize = 0;
            unsigned char* raw     = Entry_LoadRaw(e, &rawSize);
            s_DdsBptc      probe;

            if (raw != NULL && Dds_ParseBptc(raw, (int)rawSize, &probe))
            {
                g_tpDdsBytes  = raw;
                g_tpDdsSize   = rawSize;
                g_tpLastIsDds = 1;
                *outW = probe.width;
                *outH = probe.height;
                return NULL; /* DDS, not RGBA — g_tpLastHash already set for keying */
            }
            free(raw);
        }
    }

    /* Decode every matching image once; the pack scale is the largest
     * image-to-subrect ratio among them (uniform in practice). */
    {
        unsigned char* imgs[64];
        int imgW[64], imgH[64], imgStbi[64];
        int anyImage = 0;

        for (i = 0; i < matchCount; i++)
        {
            imgs[i] = Entry_LoadImage(&g_entries[matches[i]], &imgW[i], &imgH[i], &imgStbi[i]);
            if (imgs[i] != NULL)
            {
                const PackEntry* e = &g_entries[matches[i]];
                float sx = (float)imgW[i] / (float)e->subW;
                float sy = (float)imgH[i] / (float)e->subH;
                if (sx > scaleX) scaleX = sx;
                if (sy > scaleY) scaleY = sy;
                anyImage = 1;
            }
        }
        if (scaleX > 16.0f) scaleX = 16.0f;
        if (scaleY > 16.0f) scaleY = 16.0f;

        canvasW = (int)ceilf((float)nativeW * scaleX);
        canvasH = (int)ceilf((float)h * scaleY);

        canvas = NULL;
        if (anyImage && canvasW > 0 && canvasH > 0 && canvasW <= 16384 && canvasH <= 16384)
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
            if (imgs[i] == NULL) continue;
            if (canvas != NULL)
            {
                const PackEntry* e = &g_entries[matches[i]];
                int dx = (int)((float)e->offX * scaleX);
                int dy = (int)((float)e->offY * scaleY);
                int dw = (int)ceilf((float)e->subW * scaleX);
                int dh = (int)ceilf((float)e->subH * scaleY);
                BlitNearest(canvas, canvasW, canvasH, dx, dy, dw, dh, imgs[i], imgW[i], imgH[i]);
            }
            if (imgStbi[i]) stbi_image_free(imgs[i]);
            else free(imgs[i]);
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

/* ---- Texture dumping (dump_textures) ---------------------------------------
 * DuckStation dumps the piece of a texture a draw actually samples, through the
 * palette that draw selects. This does the same, without a draw hook: the piece
 * is the UV bounding box of one MODEL over the sheet, and the palette is the
 * CLUT row that model's primitives carry — both baked into the .ILM/.PLM/.IPD
 * the engine has just loaded (TexPack_DumpScanLm reads them straight out of the
 * PSX-layout bytes, the same fields pc_port/tools/clut_tool.py parses offline).
 * The result is one concise image per object with its correct colours, instead
 * of a whole sheet per palette that a modder has to hunt through.
 *
 * The two halves meet by TIM NAME, which is also the material name
 * (Material_TimFileNameGet appends the extension and nothing else). Because an
 * LM is what queues its own materials' TIM reads, the geometry is normally
 * registered BEFORE the upload arrives; a sheet whose model shows up later
 * (shared BG sheets are referenced by up to 118 chunks) is served from a held
 * copy of the upload, so the crops appear the moment that chunk loads.
 *
 * Rows the geometry never selects still get dumped, using the union of the rows
 * it does select: a material's CLUT row is bumped at runtime for palette
 * effects, which moves the SAME geometry onto another row. A sheet no model
 * ever references at all (fonts, HUD, 2D screens) falls back to the whole
 * upload, which is what "the object" means for those.
 *
 * Every name is one TexPack_Compose entry would match this very upload by, so
 * the dump folder IS a texture pack: the crops blit back over the native base
 * layer at their offsets and reproduce the native decode exactly. */

/* Deflate level for the dump PNGs. This runs inline on a load frame, so it
 * trades a little size for latency: measured over real disc TIMs, level 6 is
 * 2.1x the CPU of level 3 for 9% smaller files. */
#ifndef TEXDUMP_PNG_LEVEL
#define TEXDUMP_PNG_LEVEL 3
#endif

/* Rows past this are never uploaded (HIRES_POOL_MAX_ROWS), so never dumped. */
#define DUMP_MAX_ROWS 16

/* Crops per (sheet, row). TexPack_Compose stops collecting matches at 64 per
 * upload, and a modder may stack a second pack on top of the dump, so keep well
 * clear of that: over the whole disc corpus a row averages 1.1 objects (BG) /
 * 2.1 (characters), and the cap is only reached by merging the two boxes whose
 * union wastes least — never by dropping one. */
#define DUMP_MAX_REGIONS 16

/* An upload with no geometry yet is held this many further uploads before it is
 * written whole-cover. The LM that names a sheet is what queues its read, so a
 * sheet still unclaimed after a load batch has no model at all. */
#define DUMP_PENDING_GRACE 96

/* Held-upload cache: dev-mode only, and only the raw TIM bytes (a 4bpp 256x256
 * sheet is 32 KB). Bounded so a long session cannot grow without limit. */
#define DUMP_HOLD_MAX_BYTES (64u * 1024u * 1024u)
#define DUMP_HOLD_MAX_COUNT 2048

/** Sub-rect of a sheet in native texels, corners INCLUSIVE. */
typedef struct {
    unsigned short x0, y0, x1, y1;
} DumpRect;

/** Every object box registered for one sheet name, per CLUT row. */
typedef struct {
    char          name[12];
    unsigned char count[DUMP_MAX_ROWS];
    DumpRect      rect[DUMP_MAX_ROWS][DUMP_MAX_REGIONS];
} DumpRegions;

/** A retained copy of one upload, so late geometry can still be cropped. */
typedef struct {
    char               name[12];
    unsigned char*     pixels;
    unsigned short*    clut;
    unsigned long long srcHash;
    int                w16, h, bpp, clutW, clutRows;
    size_t             bytes;
    unsigned int       gen;     /* upload counter at the time it was held */
    int                emitted; /* 1 once geometry-driven crops were written */
} DumpHeld;

static unsigned long long* g_dumpSeen      = NULL; /* sorted name hashes handled this session */
static int                 g_dumpSeenCount = 0;
static int                 g_dumpSeenCap   = 0;
static int                 g_dumpWritten   = 0;
static int                 g_dumpFailed    = 0;
static int                 g_dumpCrops     = 0; /* geometry-driven entries */
static int                 g_dumpWhole     = 0; /* whole-upload fallbacks */

static DumpRegions** g_dumpRegions     = NULL;
static int           g_dumpRegionCount = 0;
static int           g_dumpRegionCap   = 0;

static DumpHeld*    g_dumpHeld      = NULL;
static int          g_dumpHeldCount = 0;
static int          g_dumpHeldCap   = 0;
static size_t       g_dumpHeldBytes = 0;
static unsigned int g_dumpGen       = 0;

static unsigned int Dump_Rd32(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int Dump_Rd16(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

/* Index of the first seen-key >= k. */
static int Dump_LowerBound(unsigned long long k)
{
    int lo = 0, hi = g_dumpSeenCount;
    while (lo < hi)
    {
        int mid = (lo + hi) / 2;
        if (g_dumpSeen[mid] < k) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

/* 1 when k is new (and now recorded), 0 when this entry was already handled. */
static int Dump_MarkSeen(unsigned long long k)
{
    int at = Dump_LowerBound(k);

    if (at < g_dumpSeenCount && g_dumpSeen[at] == k) return 0;

    if (g_dumpSeenCount == g_dumpSeenCap)
    {
        int                 cap = g_dumpSeenCap ? g_dumpSeenCap * 2 : 256;
        unsigned long long* grown =
            (unsigned long long*)realloc(g_dumpSeen, (size_t)cap * sizeof(unsigned long long));
        if (grown == NULL) return 0;
        g_dumpSeen    = grown;
        g_dumpSeenCap = cap;
    }

    memmove(&g_dumpSeen[at + 1], &g_dumpSeen[at],
            (size_t)(g_dumpSeenCount - at) * sizeof(unsigned long long));
    g_dumpSeen[at] = k;
    g_dumpSeenCount++;
    return 1;
}

static void Dump_EnsureDir(void)
{
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;

#ifdef _WIN32
    _mkdir(TEXDUMP_DIR);
#else
    mkdir(TEXDUMP_DIR, 0755);
#endif
    atexit(TexPack_DumpFlush);
    SH_DBG("[TEXDUMP] dump_textures on — writing per-object crops to %s/", TEXDUMP_DIR);
}

/* Sheet name key: the file/material stem, uppercased, extension dropped. */
static void Dump_NameKey(const char* src, char* out)
{
    int i = 0;

    if (src != NULL)
    {
        for (; i < 8; i++)
        {
            char c = src[i];
            if (c == '\0' || c == '.' || c == ' ') break;
            out[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
        }
    }
    while (i < 12) out[i++] = '\0';
}

/* ---- object boxes ---------------------------------------------------------- */

static DumpRegions* Dump_RegionsFind(const char* key, int create)
{
    int i;

    if (key[0] == '\0') return NULL;

    for (i = 0; i < g_dumpRegionCount; i++)
    {
        if (memcmp(g_dumpRegions[i]->name, key, 12) == 0) return g_dumpRegions[i];
    }
    if (!create) return NULL;

    if (g_dumpRegionCount == g_dumpRegionCap)
    {
        int           cap   = g_dumpRegionCap ? g_dumpRegionCap * 2 : 128;
        DumpRegions** grown = (DumpRegions**)realloc(g_dumpRegions, (size_t)cap * sizeof(DumpRegions*));
        if (grown == NULL) return NULL;
        g_dumpRegions   = grown;
        g_dumpRegionCap = cap;
    }

    g_dumpRegions[g_dumpRegionCount] = (DumpRegions*)calloc(1, sizeof(DumpRegions));
    if (g_dumpRegions[g_dumpRegionCount] == NULL) return NULL;
    memcpy(g_dumpRegions[g_dumpRegionCount]->name, key, 12);
    return g_dumpRegions[g_dumpRegionCount++];
}

static unsigned int Dump_RectArea(const DumpRect* r)
{
    return (unsigned int)(r->x1 - r->x0 + 1) * (unsigned int)(r->y1 - r->y0 + 1);
}

static int Dump_RectContains(const DumpRect* a, const DumpRect* b) /* a covers b */
{
    return a->x0 <= b->x0 && a->y0 <= b->y0 && a->x1 >= b->x1 && a->y1 >= b->y1;
}

static void Dump_RectUnion(DumpRect* dst, const DumpRect* a, const DumpRect* b)
{
    dst->x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    dst->y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    dst->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    dst->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
}

/* Add one box to a row's list: swallowed boxes collapse, and a full list merges
 * the newcomer into whichever box wastes the least area — never drops it, so no
 * texel an object draws can fall out of the dump. */
static void Dump_RectAdd(DumpRect* list, unsigned char* count, const DumpRect* add)
{
    int          i, n = (int)*count, bestIdx = 0;
    unsigned int bestCost = 0;

    for (i = 0; i < n; i++)
    {
        if (Dump_RectContains(&list[i], add)) return;
    }
    for (i = 0; i < n;)
    {
        if (Dump_RectContains(add, &list[i])) list[i] = list[--n];
        else i++;
    }
    if (n < DUMP_MAX_REGIONS)
    {
        list[n++] = *add;
        *count    = (unsigned char)n;
        return;
    }

    for (i = 0; i < n; i++)
    {
        DumpRect     u;
        unsigned int cost;
        Dump_RectUnion(&u, &list[i], add);
        cost = Dump_RectArea(&u) - Dump_RectArea(&list[i]);
        if (i == 0 || cost < bestCost)
        {
            bestCost = cost;
            bestIdx  = i;
        }
    }
    Dump_RectUnion(&list[bestIdx], &list[bestIdx], add);
    *count = (unsigned char)n;
}

/* ---- entry naming + writing ------------------------------------------------- */

static void Dump_EntryName(char* out, size_t outSize, int bpp,
                           unsigned long long srcHash, unsigned long long palHash,
                           int w16, int h, int ox, int oy, int sw, int sh)
{
    if (bpp == 16)
    {
        snprintf(out, outSize, "texupload-C16-%016llX-%dx%d-%d-%d-%dx%d.png",
                 srcHash, w16, h, ox, oy, sw, sh);
    }
    else
    {
        snprintf(out, outSize, "texupload-%s-%016llX-%016llX-%dx%d-%d-%d-%dx%d-P0-%d.png",
                 (bpp == 4) ? "P4" : "P8", srcHash, palHash, w16, h, ox, oy, sw, sh,
                 (bpp == 4) ? 15 : 255);
    }
}

static int Dump_WritePng(const char* name, const unsigned char* rgba, int w, int h)
{
    char   path[320];
    void*  png;
    size_t pngLen = 0;
    FILE*  f;
    int    ok = 0;

    png = tdefl_write_image_to_png_file_in_memory_ex(rgba, w, h, 4, &pngLen,
                                                     TEXDUMP_PNG_LEVEL, MZ_FALSE);
    if (png == NULL)
    {
        g_dumpFailed++;
        return 0;
    }

    snprintf(path, sizeof(path), "%s/%s", TEXDUMP_DIR, name);
    f = fopen(path, "wb");
    if (f == NULL)
    {
        g_dumpFailed++;
    }
    else
    {
        if (fwrite(png, 1, pngLen, f) == pngLen)
        {
            g_dumpWritten++;
            ok = 1;
        }
        else
        {
            g_dumpFailed++;
        }
        fclose(f);
    }
    mz_free(png);

    if (g_dumpWritten > 0 && (g_dumpWritten & 255) == 0)
    {
        SH_DBG("[TEXDUMP] %d entries written to %s/ (%d object crops, %d whole sheets, %d failed)",
               g_dumpWritten, TEXDUMP_DIR, g_dumpCrops, g_dumpWhole, g_dumpFailed);
    }
    return ok;
}

/* Write every crop of one CLUT row. The row is decoded ONCE, and only if some
 * crop of it is actually new. */
static void Dump_EmitRow(const DumpHeld* rec, int row, const DumpRect* rects, int nRects, int wholeCover)
{
    unsigned long long    palHash = 0;
    const unsigned short* pal     = NULL;
    unsigned char*        rowRgba = NULL;
    int                   nativeW = rec->w16 * (16 / rec->bpp);
    int                   i;

    if (rec->bpp != 16)
    {
        int n = (rec->bpp == 4) ? 16 : 256;
        pal   = rec->clut + (size_t)row * (size_t)rec->clutW;
        if (n > rec->clutW) n = rec->clutW;
        palHash = XXH3_64bits(pal, (size_t)n * 2);
    }

    for (i = 0; i < nRects; i++)
    {
        char           name[192];
        char           path[320];
        FILE*          f;
        unsigned char* crop;
        int            ox = rects[i].x0, oy = rects[i].y0;
        int            sw = (int)rects[i].x1 - ox + 1;
        int            sh = (int)rects[i].y1 - oy + 1;
        int            y, wrote;

        if (ox >= nativeW || oy >= rec->h) continue;
        if (ox + sw > nativeW) sw = nativeW - ox;
        if (oy + sh > rec->h) sh = rec->h - oy;
        if (sw <= 0 || sh <= 0) continue;

        Dump_EntryName(name, sizeof(name), rec->bpp, rec->srcHash, palHash,
                       rec->w16, rec->h, ox, oy, sw, sh);

        /* The file name IS the identity, so dedupe on it: two objects with the
         * same box, or two CLUT rows holding the same palette, collapse to one
         * entry, as they must. */
        if (!Dump_MarkSeen(XXH3_64bits(name, strlen(name)))) continue;

        Dump_EnsureDir();
        snprintf(path, sizeof(path), "%s/%s", TEXDUMP_DIR, name);
        f = fopen(path, "rb");
        if (f != NULL)
        {
            fclose(f); /* dumped by an earlier session */
            continue;
        }

        if (rowRgba == NULL)
        {
            rowRgba = DecodeNative(rec->pixels, rec->w16, rec->h, pal, rec->clutW, rec->bpp, nativeW);
            if (rowRgba == NULL) return;
        }

        if (sw == nativeW && sh == rec->h)
        {
            wrote = Dump_WritePng(name, rowRgba, nativeW, rec->h);
        }
        else
        {
            crop = (unsigned char*)malloc((size_t)sw * (size_t)sh * 4);
            if (crop == NULL) continue;
            for (y = 0; y < sh; y++)
            {
                memcpy(&crop[(size_t)y * (size_t)sw * 4],
                       &rowRgba[(((size_t)(oy + y) * (size_t)nativeW) + (size_t)ox) * 4],
                       (size_t)sw * 4);
            }
            wrote = Dump_WritePng(name, crop, sw, sh);
            free(crop);
        }

        if (!wrote) continue;
        if (wholeCover) g_dumpWhole++;
        else g_dumpCrops++;
    }

    free(rowRgba);
}

/* Emit one held upload. allowWholeCover=0 leaves a sheet with no geometry yet
 * untouched (it stays held); =1 writes the whole upload for one that never got
 * any. */
static void Dump_EmitHeld(DumpHeld* rec, int allowWholeCover)
{
    const DumpRegions* regs     = Dump_RegionsFind(rec->name, 0);
    int                nativeW  = rec->w16 * (16 / rec->bpp);
    int                rows     = (rec->bpp == 16) ? 1 : rec->clutRows;
    DumpRect           anyRow[DUMP_MAX_REGIONS];
    unsigned char      anyCount = 0;
    int                row, done = 0;

    if (rows < 1) rows = 1;

    if (regs == NULL)
    {
        DumpRect whole;

        if (!allowWholeCover) return;

        whole.x0 = 0;
        whole.y0 = 0;
        whole.x1 = (unsigned short)(nativeW - 1);
        whole.y1 = (unsigned short)(rec->h - 1);
        for (row = 0; row < rows; row++)
        {
            Dump_EmitRow(rec, row, &whole, 1, 1);
        }
        rec->emitted = 1;
        return;
    }

    /* A material's CLUT row is bumped at runtime for palette effects, moving the
     * same geometry onto a row no prim names statically — so a row with no boxes
     * of its own is dumped through the union of the boxes that do exist. */
    for (row = 0; row < DUMP_MAX_ROWS; row++)
    {
        int i;
        for (i = 0; i < (int)regs->count[row]; i++)
        {
            Dump_RectAdd(anyRow, &anyCount, &regs->rect[row][i]);
        }
    }

    for (row = 0; row < rows; row++)
    {
        if (regs->count[row] > 0)
        {
            Dump_EmitRow(rec, row, regs->rect[row], (int)regs->count[row], 0);
            done = 1;
        }
        else if (anyCount > 0)
        {
            Dump_EmitRow(rec, row, anyRow, (int)anyCount, 0);
            done = 1;
        }
    }
    /* No box survived validation: leave it pending rather than call it dumped. */
    if (done) rec->emitted = 1;
}

/* ---- held uploads ---------------------------------------------------------- */

static DumpHeld* Dump_HeldFind(const char* key)
{
    int i;
    if (key[0] == '\0') return NULL;
    for (i = 0; i < g_dumpHeldCount; i++)
    {
        if (memcmp(g_dumpHeld[i].name, key, 12) == 0) return &g_dumpHeld[i];
    }
    return NULL;
}

static void Dump_HeldRelease(int idx)
{
    DumpHeld* rec = &g_dumpHeld[idx];

    free(rec->pixels);
    free(rec->clut);
    g_dumpHeldBytes -= rec->bytes;
    g_dumpHeld[idx] = g_dumpHeld[--g_dumpHeldCount];
}

/* Pending sheets are written whole-cover once the load batch that could have
 * claimed them is long past; emitted ones are simply dropped to stay in budget. */
static void Dump_HeldTrim(void)
{
    int i;

    for (i = 0; i < g_dumpHeldCount;)
    {
        if (!g_dumpHeld[i].emitted && g_dumpHeld[i].gen + DUMP_PENDING_GRACE < g_dumpGen)
        {
            Dump_EmitHeld(&g_dumpHeld[i], 1);
            Dump_HeldRelease(i);
            continue;
        }
        i++;
    }

    while (g_dumpHeldCount > 0 &&
           (g_dumpHeldBytes > DUMP_HOLD_MAX_BYTES || g_dumpHeldCount > DUMP_HOLD_MAX_COUNT))
    {
        int oldest = 0;
        for (i = 1; i < g_dumpHeldCount; i++)
        {
            if (g_dumpHeld[i].gen < g_dumpHeld[oldest].gen) oldest = i;
        }
        if (!g_dumpHeld[oldest].emitted) Dump_EmitHeld(&g_dumpHeld[oldest], 1);
        Dump_HeldRelease(oldest);
    }
}

void TexPack_DumpFlush(void)
{
    while (g_dumpHeldCount > 0)
    {
        if (!g_dumpHeld[0].emitted) Dump_EmitHeld(&g_dumpHeld[0], 1);
        Dump_HeldRelease(0);
    }
    if (g_dumpWritten > 0 || g_dumpFailed > 0)
    {
        SH_DBG("[TEXDUMP] %d entries in %s/ (%d object crops, %d whole sheets, %d failed)",
               g_dumpWritten, TEXDUMP_DIR, g_dumpCrops, g_dumpWhole, g_dumpFailed);
    }
}

/* ---- public entry points ---------------------------------------------------- */

static void Dump_AddRegion(const char* sheetName, int row, int x0, int y0, int x1, int y1)
{
    char         key[12];
    DumpRegions* regs;
    DumpRect     r;

    if (!g_PcConfig.dumpTextures) return;
    if (row < 0 || row >= DUMP_MAX_ROWS) return;
    if (x1 < x0 || y1 < y0 || x0 < 0 || y0 < 0) return;
    if (x1 > 0xFFFF || y1 > 0xFFFF) return;

    Dump_NameKey(sheetName, key);
    regs = Dump_RegionsFind(key, 1);
    if (regs == NULL) return;

    r.x0 = (unsigned short)x0;
    r.y0 = (unsigned short)y0;
    r.x1 = (unsigned short)x1;
    r.y1 = (unsigned short)y1;
    Dump_RectAdd(regs->rect[row], &regs->count[row], &r);
}

void TexPack_DumpScanLm(const unsigned char* lm)
{
    unsigned int matCount, matOff, modelCount, modelOff;
    DumpRect*    box;
    unsigned char* boxValid;
    unsigned int m, i;

    if (!g_PcConfig.dumpTextures || lm == NULL) return;
    /* '0' = LM_HEADER_MAGIC. Version 7 is the port's wide high-poly ILM, whose
     * primitives have a different layout — its narrow spine carries no prims. */
    if (lm[0] != '0' || lm[1] != 6) return;

    matCount   = lm[3];
    matOff     = Dump_Rd32(&lm[4]);
    modelCount = lm[8];
    modelOff   = Dump_Rd32(&lm[12]);

    /* The engine walks these very offsets a few lines later with no bounds check
     * of its own; refuse only the obviously wild ones so a garbage header cannot
     * make dump mode the thing that crashes. */
    if (matCount == 0 || modelCount == 0) return;
    if (matOff < 20 || matOff > 0x400000 || modelOff < 20 || modelOff > 0x400000) return;

    box      = (DumpRect*)malloc((size_t)matCount * DUMP_MAX_ROWS * sizeof(DumpRect));
    boxValid = (unsigned char*)malloc((size_t)matCount * DUMP_MAX_ROWS);
    if (box == NULL || boxValid == NULL)
    {
        free(box);
        free(boxValid);
        return;
    }

    for (m = 0; m < modelCount; m++)
    {
        const unsigned char* mh = &lm[modelOff + m * 16];
        unsigned int         meshCount = mh[8];
        unsigned int         meshOff   = Dump_Rd32(&mh[12]);
        unsigned int         k;

        if (meshCount == 0 || meshOff < 20 || meshOff > 0x400000) continue;
        memset(boxValid, 0, (size_t)matCount * DUMP_MAX_ROWS);

        for (k = 0; k < meshCount; k++)
        {
            const unsigned char* msh       = &lm[meshOff + k * 24];
            unsigned int         primCount = msh[0];
            unsigned int         primOff   = Dump_Rd32(&msh[4]);
            unsigned int         p;

            if (primCount == 0 || primOff < 20 || primOff > 0x400000) continue;

            for (p = 0; p < primCount; p++)
            {
                const unsigned char* pb  = &lm[primOff + p * 20];
                unsigned int         mat = (Dump_Rd16(&pb[6]) >> 8) & 0x7F;
                int                  row, slot, nUv, uvi;
                static const int     uvOff[4] = { 0, 4, 8, 0xA };

                if (mat >= matCount) continue;

                /* row = prim CLUT - the material's baked base CLUT, both still
                 * the values from the file: Material_FsImageApply has not run
                 * yet (it needs the reformatted header this call produces). */
                row = (int)(Dump_Rd16(&pb[2]) >> 6) -
                      (int)(Dump_Rd16(&lm[matOff + mat * 24 + 0x10]) >> 6);
                if (row < 0 || row >= DUMP_MAX_ROWS) continue;

                /* A character triangle sets its 4th vertex index to 0xFF and
                 * leaves UV3 garbage (0,0); counting it drags the box to the
                 * sheet origin. Verified over the corpus: 6109 hits, every one
                 * in a .ILM, zero in 431185 .IPD + 10735 .PLM primitives. */
                nUv  = (pb[0xF] == 0xFF) ? 3 : 4;
                slot = (int)mat * DUMP_MAX_ROWS + row;

                for (uvi = 0; uvi < nUv; uvi++)
                {
                    unsigned int   uv = Dump_Rd16(&pb[uvOff[uvi]]);
                    unsigned short u  = (unsigned short)(uv & 0xFF);
                    unsigned short v  = (unsigned short)(uv >> 8);

                    if (!boxValid[slot])
                    {
                        boxValid[slot] = 1;
                        box[slot].x0 = box[slot].x1 = u;
                        box[slot].y0 = box[slot].y1 = v;
                    }
                    else
                    {
                        if (u < box[slot].x0) box[slot].x0 = u;
                        if (v < box[slot].y0) box[slot].y0 = v;
                        if (u > box[slot].x1) box[slot].x1 = u;
                        if (v > box[slot].y1) box[slot].y1 = v;
                    }
                }
            }
        }

        /* One box per (object, palette row) — the shape DuckStation dumps. */
        for (i = 0; i < matCount * DUMP_MAX_ROWS; i++)
        {
            if (!boxValid[i]) continue;
            Dump_AddRegion((const char*)&lm[matOff + (i / DUMP_MAX_ROWS) * 24],
                           (int)(i % DUMP_MAX_ROWS),
                           box[i].x0, box[i].y0, box[i].x1, box[i].y1);
        }
    }

    free(box);
    free(boxValid);

    /* Sheets already uploaded now have geometry: crop them where they stand. */
    for (i = 0; i < matCount; i++)
    {
        char      key[12];
        DumpHeld* rec;

        Dump_NameKey((const char*)&lm[matOff + i * 24], key);
        rec = Dump_HeldFind(key);
        if (rec != NULL) Dump_EmitHeld(rec, 0);
    }
}

void TexPack_DumpUpload(const char* timName, const unsigned char* pixels, int w16, int h,
                        const unsigned short* clut, int clutW, int clutRows, int bpp)
{
    char               key[12];
    DumpHeld*          rec;
    unsigned long long srcHash;
    size_t             pixBytes, clutBytes = 0;

    if (!g_PcConfig.dumpTextures) return;
    if (pixels == NULL || w16 <= 0 || h <= 0) return;
    /* 24bpp has no mode token in the pack grammar, so a dump of one could never
     * be loaded back — and nothing replaces those uploads today either. */
    if (bpp != 4 && bpp != 8 && bpp != 16) return;
    if (bpp != 16 && (clut == NULL || clutW <= 0)) return;

    if (bpp == 16) clutRows = 1;
    if (clutRows < 1) clutRows = 1;
    if (clutRows > DUMP_MAX_ROWS) clutRows = DUMP_MAX_ROWS;

    Dump_EnsureDir();
    Dump_NameKey(timName, key);
    /* Byte-for-byte the hash TexPack_Compose matches entries on. */
    srcHash  = XXH3_64bits(pixels, (size_t)w16 * (size_t)h * 2);
    pixBytes = (size_t)w16 * (size_t)h * 2;
    g_dumpGen++;

    /* No file name to match geometry by: nothing can ever claim this upload, so
     * write it whole from the caller's buffers and hold nothing. */
    if (key[0] == '\0')
    {
        DumpHeld borrowed;

        memset(&borrowed, 0, sizeof(borrowed));
        borrowed.pixels   = (unsigned char*)pixels;
        borrowed.clut     = (unsigned short*)clut;
        borrowed.srcHash  = srcHash;
        borrowed.w16      = w16;
        borrowed.h        = h;
        borrowed.bpp      = bpp;
        borrowed.clutW    = clutW;
        borrowed.clutRows = clutRows;
        Dump_EmitHeld(&borrowed, 1);
        return;
    }

    rec = Dump_HeldFind(key);
    if (rec != NULL && rec->srcHash == srcHash)
    {
        rec->gen = g_dumpGen;
        Dump_EmitHeld(rec, 0);
        Dump_HeldTrim();
        return;
    }
    if (rec != NULL)
    {
        /* Same name, different art (a re-used slot): the old copy will never be
         * claimed by geometry now, so settle it before dropping it. */
        if (!rec->emitted) Dump_EmitHeld(rec, 1);
        Dump_HeldRelease((int)(rec - g_dumpHeld));
    }

    if (g_dumpHeldCount == g_dumpHeldCap)
    {
        int       cap   = g_dumpHeldCap ? g_dumpHeldCap * 2 : 128;
        DumpHeld* grown = (DumpHeld*)realloc(g_dumpHeld, (size_t)cap * sizeof(DumpHeld));
        if (grown == NULL) return;
        g_dumpHeld    = grown;
        g_dumpHeldCap = cap;
    }

    rec = &g_dumpHeld[g_dumpHeldCount];
    memset(rec, 0, sizeof(*rec));
    memcpy(rec->name, key, 12);
    rec->pixels = (unsigned char*)malloc(pixBytes);
    if (rec->pixels == NULL) return;
    memcpy(rec->pixels, pixels, pixBytes);
    if (bpp != 16)
    {
        clutBytes = (size_t)clutW * (size_t)clutRows * 2;
        rec->clut = (unsigned short*)malloc(clutBytes);
        if (rec->clut == NULL)
        {
            free(rec->pixels);
            return;
        }
        memcpy(rec->clut, clut, clutBytes);
    }
    rec->srcHash  = srcHash;
    rec->w16      = w16;
    rec->h        = h;
    rec->bpp      = bpp;
    rec->clutW    = clutW;
    rec->clutRows = clutRows;
    rec->bytes    = pixBytes + clutBytes;
    rec->gen      = g_dumpGen;
    g_dumpHeldBytes += rec->bytes;
    g_dumpHeldCount++;

    /* Crops only: with no geometry yet this stays held until one turns up, or
     * until the grace period says none ever will. */
    Dump_EmitHeld(rec, 0);
    Dump_HeldTrim();
}
