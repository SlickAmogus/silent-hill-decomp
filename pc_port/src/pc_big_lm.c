/* Oversized loose character models — Stage 1 loader.
 *
 * Registry of oversized gamedata/load/ ILM replacements, keyed by
 * g_FileTable index (F3: CHARA_FILE_INFOS rows are retargeted at runtime —
 * PAL/JPN Mumbler swap, pool beta retargets — so charaId is not a stable
 * key). An accepted file gets a calloc'd PC buffer (+16B tail, F1) that the
 * world_draw.c hooks substitute for the fixed PSX-RAM slab; the FS queue
 * then freads the loose file into it via the capacity-lifted byte-replace
 * path in Fs_QueueTickRead. Every failure path returns the native slab, so
 * stock behavior is untouched when no oversized file exists. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"

#include "pc_big_lm.h"
#include "pc_config.h"
#include "sh_log.h"

/* game.h already provides u8/u16/u32/s32. */
#define LM_VALIDATE_HAVE_TYPES
#include "lm_validate.h"

enum
{
    BIGLM_STATE_ACCEPTED = 1,
    BIGLM_STATE_REJECTED = 2
};

typedef struct
{
    s16         fileIdx;  /* g_FileTable index — THE key */
    u8*         buf;      /* calloc(fileSize + PC_BIGLM_TAIL_SLACK); NULL until Redirect allocates */
    size_t      cap;
    long        fileSize; /* validated loose size the state refers to */
    s_LmHeader* native;   /* the slab pointer this redirect displaced */
    u8          state;
} PcBigLm;

/* Only oversized files (accepted or rejected) occupy slots — ordinary
 * misses and same-size replacements never enter the table, so it cannot
 * fill up from normal play. */
#define PC_BIGLM_MAX 64
static PcBigLm s_bigLm[PC_BIGLM_MAX];
static int     s_bigLmCount;

/* A buffer displaced by a mid-session file-size change stays tracked (and
 * leaked): a live s_CharaModel may still point at it, and losing
 * IsOwned/NativeOf for that pointer would resurrect the bump-cursor hijack
 * the WorldGfx_CharaLmBufferAssign patch guards against (F4). */
typedef struct
{
    u8*         buf;
    size_t      cap;
    s_LmHeader* native;
} PcBigLmRetired;

#define PC_BIGLM_RETIRED_MAX 32
static PcBigLmRetired s_retired[PC_BIGLM_RETIRED_MAX];
static int            s_retiredCount;

/* Pool-owned destination buffers, keyed by charaId so a pool realloc/free
 * overwrites the slot and can never leave a stale pointer registration. */
static struct
{
    const void* buf;
    size_t      cap;
} s_extBuf[Chara_Count];

/* Built exactly as Fs_QueueTickRead builds its probe path, so both probes
 * see the same on-disk file. */
static int BigLm_LoosePath(s32 fileIdx, char* out, size_t outSize)
{
    const s_FileInfo* file   = &g_FileTable[fileIdx];
    const char*       folder = g_FilePaths[file->pathIdx];
    char              strippedFolder[16];
    char              nameBuf[32];
    size_t            fi;
    size_t            fl;

    if (folder[0] == '\\')
    {
        folder++;
    }
    fl = strlen(folder);
    while (fl > 0 && folder[fl - 1] == '\\')
    {
        fl--;
    }
    if (fl >= sizeof(strippedFolder))
    {
        fl = sizeof(strippedFolder) - 1;
    }
    for (fi = 0; fi < fl; fi++)
    {
        strippedFolder[fi] = folder[fi];
    }
    strippedFolder[fl] = '\0';

    Fs_GetFileName(nameBuf, fileIdx);

    return snprintf(out, outSize, "gamedata/load/%s/%s", strippedFolder, nameBuf) < (int)outSize;
}

static long BigLm_ProbeSize(const char* path)
{
    long  sz = -1;
    FILE* f  = Pc_LooseFOpen(path, "rb");

    if (f == NULL)
    {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) == 0)
    {
        sz = ftell(f);
    }
    fclose(f);
    return sz;
}

static u8* BigLm_Slurp(const char* path, long expectSize)
{
    u8*   buf = NULL;
    FILE* f   = Pc_LooseFOpen(path, "rb");

    if (f == NULL)
    {
        return NULL;
    }
    buf = (u8*)malloc((size_t)expectSize);
    if (buf != NULL && fread(buf, 1, (size_t)expectSize, f) != (size_t)expectSize)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

/* Skeletal validation on the raw pre-fix-up bytes, before any redirect
 * buffer exists. tailSlackBytes = the +16 calloc tail Redirect guarantees
 * (F1). anmBoneCount = -1: at probe time the ANM for this fileIdx may not be
 * loaded and the FS tick must not read a second file; an out-of-range bone
 * prefix degrades (garbage placement), it does not corrupt, and grow-mode
 * enforces V13 strictly at author time. */
static int BigLm_Validate(const u8* bytes, long fileSize, char* err, u32 errCap)
{
    s_LmValidateParams params;

    params.tailSlackBytes = PC_BIGLM_TAIL_SLACK;
    params.anmBoneCount   = -1;
    params.skeletal       = 1;

    return Lm_Validate(bytes, (u32)fileSize, &params, err, errCap);
}

static PcBigLm* BigLm_Find(s32 fileIdx)
{
    int i;

    for (i = 0; i < s_bigLmCount; i++)
    {
        if (s_bigLm[i].fileIdx == (s16)fileIdx)
        {
            return &s_bigLm[i];
        }
    }
    return NULL;
}

/* Probe + validate an oversized loose file for fileIdx, caching the verdict.
 * Returns the registry entry, or NULL when the file is absent, not oversized,
 * or the table is full. Never allocates the redirect buffer (Redirect does;
 * the pool sizes its own buffer off Pc_BigLm_LooseSize). */
static PcBigLm* BigLm_Ensure(s32 fileIdx, const char* path)
{
    long     fileSize;
    long     tableSize;
    PcBigLm* e;

    fileSize  = BigLm_ProbeSize(path);
    tableSize = (long)Fs_GetFileSectorAlignedSize(fileIdx);
    if (fileSize <= tableSize)
    {
        /* Absent or fits the slot: the ordinary byte-replace path owns it. */
        return NULL;
    }

    e = BigLm_Find(fileIdx);
    if (e != NULL && e->fileSize == fileSize)
    {
        return e;
    }

    /* New or size-changed file: re-validate before anything trusts it. */
    {
        char err[160];
        u8*  tmp = BigLm_Slurp(path, fileSize);
        int  ok;

        if (tmp == NULL)
        {
            snprintf(err, sizeof(err), "read failed");
            ok = 0;
        }
        else
        {
            SH_DBG("[BIGLM] bone-prefix check skipped (ANM boneCount unknown)");
            ok = BigLm_Validate(tmp, fileSize, err, sizeof(err)) == 0;
        }
        free(tmp);

        if (e == NULL)
        {
            if (s_bigLmCount >= PC_BIGLM_MAX)
            {
                SH_DBG("[BIGLM] registry full (%d), ignoring %s — native model", PC_BIGLM_MAX, path);
                return NULL;
            }
            e = &s_bigLm[s_bigLmCount++];
            memset(e, 0, sizeof(*e));
            e->fileIdx = (s16)fileIdx;
        }
        e->fileSize = fileSize;

        if (!ok)
        {
            e->state = BIGLM_STATE_REJECTED;
            SH_DBG("[BIGLM] reject: %s (%ld B): %s — native model", path, fileSize, err);
            return e;
        }

        if (e->buf != NULL)
        {
            if (e->cap < (size_t)fileSize + PC_BIGLM_TAIL_SLACK)
            {
                if (s_retiredCount < PC_BIGLM_RETIRED_MAX)
                {
                    s_retired[s_retiredCount].buf    = e->buf;
                    s_retired[s_retiredCount].cap    = e->cap;
                    s_retired[s_retiredCount].native = e->native;
                    s_retiredCount++;
                }
                else
                {
                    SH_DBG("[BIGLM] retired-buffer table full; %p untracked", (void*)e->buf);
                }
                e->buf = NULL;
                e->cap = 0;
            }
            else
            {
                /* Reused buffer: re-zero so the F1 tail beyond the new file
                 * stays zero after the queue's fread writes only fileSize
                 * bytes. */
                memset(e->buf, 0, e->cap);
            }
        }

        e->state = BIGLM_STATE_ACCEPTED;
        SH_DBG("[BIGLM] ACCEPT %s: %ld B (table %ld B)", path, fileSize, tableSize);
    }

    return e;
}

s_LmHeader* Pc_BigLm_Redirect(s32 modelFileIdx, s_LmHeader* lmHdr)
{
    char        path[176];
    s_LmHeader* native = lmHdr;
    PcBigLm*    e;

    /* F4: the slot-reuse path can hand a previous chara's redirected buffer
     * here — resolve back to the slab it displaced so a stock chara after a
     * modded one gets its vanilla slab. */
    if (Pc_BigLm_IsOwned(lmHdr))
    {
        native = Pc_BigLm_NativeOf(lmHdr);
    }

    if (!g_PcConfig.allowLooseFiles || lmHdr == NULL ||
        modelFileIdx < 0 || modelFileIdx >= FS_FILE_COUNT)
    {
        return native;
    }
    if (!BigLm_LoosePath(modelFileIdx, path, sizeof(path)))
    {
        return native;
    }

    e = BigLm_Ensure(modelFileIdx, path);
    if (e == NULL || e->state != BIGLM_STATE_ACCEPTED)
    {
        return native;
    }

    if (e->buf == NULL)
    {
        /* calloc keeps the F1 tail zeroed forever — the queue read only
         * writes fileSize bytes. */
        size_t cap = (size_t)e->fileSize + PC_BIGLM_TAIL_SLACK;
        u8*    buf = (u8*)calloc(1, cap);

        if (buf == NULL)
        {
            SH_DBG("[BIGLM] %s: calloc(%zu) failed — native model", path, cap);
            return native;
        }
        e->buf = buf;
        e->cap = cap;
        SH_DBG("[BIGLM] REDIRECT %s (file %d) -> %p cap %zu",
               path, modelFileIdx, (void*)e->buf, e->cap);
    }

    e->native = native;
    return (s_LmHeader*)e->buf;
}

size_t Pc_BigLm_DestCapacity(const void* dest)
{
    int i;

    if (dest == NULL)
    {
        return 0;
    }
    for (i = 0; i < s_bigLmCount; i++)
    {
        if (s_bigLm[i].buf == dest && s_bigLm[i].state == BIGLM_STATE_ACCEPTED)
        {
            return s_bigLm[i].cap;
        }
    }
    for (i = 0; i < Chara_Count; i++)
    {
        if (s_extBuf[i].buf == dest)
        {
            return s_extBuf[i].cap;
        }
    }
    return 0;
}

int Pc_BigLm_IsOwned(const void* p)
{
    const u8* q = (const u8*)p;
    int       i;

    if (q == NULL)
    {
        return 0;
    }
    for (i = 0; i < s_bigLmCount; i++)
    {
        if (s_bigLm[i].buf != NULL && q >= s_bigLm[i].buf && q < s_bigLm[i].buf + s_bigLm[i].cap)
        {
            return 1;
        }
    }
    for (i = 0; i < s_retiredCount; i++)
    {
        if (q >= s_retired[i].buf && q < s_retired[i].buf + s_retired[i].cap)
        {
            return 1;
        }
    }
    return 0;
}

s_LmHeader* Pc_BigLm_NativeOf(const void* ownedPtr)
{
    const u8* q = (const u8*)ownedPtr;
    int       i;

    if (q == NULL)
    {
        return NULL;
    }
    for (i = 0; i < s_bigLmCount; i++)
    {
        if (s_bigLm[i].buf != NULL && q >= s_bigLm[i].buf && q < s_bigLm[i].buf + s_bigLm[i].cap)
        {
            return s_bigLm[i].native;
        }
    }
    for (i = 0; i < s_retiredCount; i++)
    {
        if (q >= s_retired[i].buf && q < s_retired[i].buf + s_retired[i].cap)
        {
            return s_retired[i].native;
        }
    }
    return NULL;
}

s32 Pc_BigLm_LooseSize(s32 fileIdx)
{
    char     path[176];
    PcBigLm* e;

    if (!g_PcConfig.allowLooseFiles || fileIdx < 0 || fileIdx >= FS_FILE_COUNT)
    {
        return -1;
    }
    if (!BigLm_LoosePath(fileIdx, path, sizeof(path)))
    {
        return -1;
    }

    e = BigLm_Ensure(fileIdx, path);
    if (e == NULL || e->state != BIGLM_STATE_ACCEPTED)
    {
        return -1;
    }
    return (s32)e->fileSize;
}

void Pc_BigLm_RegisterExternal(s32 key, const void* buf, size_t cap)
{
    if (key < 0 || key >= Chara_Count)
    {
        return;
    }
    s_extBuf[key].buf = buf;
    s_extBuf[key].cap = cap;
}
