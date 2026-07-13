/* Global character/asset pool — any monster spawnable in any map.
 *
 * A monster is visible/functional only when three per-charaId registries are
 * populated (see pc_port/docs/Global_Chara_Pool.md). Vanilla loads ~3 monster
 * types per map into fixed PSX-RAM windows and 4 slot-keyed VRAM parcels; all
 * monster assets together (~1.3 MB, textures ~8.75 tpages) cannot fit there,
 * so the pool keeps them PC-side:
 *   - ILM/ANM files in malloc'd buffers (region-correct sizes at runtime);
 *   - anims in dedicated g_CharaModelAnimsData slots 4+charaId, loaded via
 *     the vanilla Fs_CharaAnimDataAlloc explicit-buffer path (the same
 *     mechanism map7_s03's boss rush uses) — vanilla only ever walks slots
 *     0..3, so pool slots are invisible to its bump chain / overlap sweep;
 *   - textures in persistent virtual GL slots (256+charaId), registered by
 *     Fs_QueuePostLoadTim off a synthetic clutY>=512 desc — no VRAM bytes,
 *     no draw-path changes.
 * Native maps keep their native slots/variants: the refresh step only fills
 * registries the current map left NULL/stale, so gameplay that never spawns
 * a foreign monster is untouched. */

#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/game_boot/fs_chara_anim.h"
#include "main/fsqueue.h"
#include "main/fileinfo.h"

#include "pc_chara_pool.h"
#include "pc_config.h"
#include "hires_override.h"
#include "dll_loader.h"
#include "sh_log.h"

typedef struct
{
    s_CharaModel model;   /* pool-owned model slot (registeredCharaModels target) */
    u8*          ilmBuf;
    u8*          anmBuf;
    s32          ilmCap;
    s32          anmCap;
    s16          modelFileIdx;   /* CHARA_FILE_INFOS values this entry was loaded from; */
    s16          textureFileIdx; /* a mismatch on a later map load = region/ending patch */
    s16          animFileIdx;    /* retargeted the chara -> reload. */
    u8           loaded;
} PcPoolChara;

/* Pool everything with real chara data: monsters 2..24 plus cutscene cast
 * 25..43 (assets only — AI backfill covers monsters; actors spawn as posed
 * statues off-map). None/Harry/Padlock have no poolable data. */
#define POOL_CHARA_FIRST Chara_AirScreamer
#define POOL_CHARA_LAST  Chara_Parasite

static PcPoolChara   s_pool[Chara_Count];
/* One posed-skeleton coord array per TYPE (56-bone model cap + root), same
 * contract as g_SysWork.npcBoneCoordBuffer consumption in fs_chara_anim.c. */
static GsCOORDINATE2 s_poolBoneCoords[Chara_Count][57];
static int           s_poolReady;

/* Debug/pool spawns carry no savegame identity: Savegame_EnemyStateUpdate
 * skips flagged slots (their field_40 is just the npc slot index, not a
 * spawn-table row). Set by the console SPAWN command; cleared when a native
 * spawn reuses the slot (Chara_Spawn) or on map load. */
u8 g_PcNpcDebugSpawned[NPC_COUNT_MAX];

void Pc_NpcDebugSpawnClearAll(void)
{
    memset(g_PcNpcDebugSpawned, 0, sizeof(g_PcNpcDebugSpawned));
}

/* True when a charaId's registered model is the pool's copy (i.e. the type
 * is only spawnable here because of the pool — console list tags these). */
int Pc_CharaPool_IsPoolModel(s32 charaId)
{
    if (charaId < 0 || charaId >= Chara_Count)
    {
        return 0;
    }

    return g_WorldGfxWork.registeredCharaModels[charaId] == &s_pool[charaId].model;
}

static int PoolChara_Load(s32 id)
{
    s_CharaFileInfo* fi = &CHARA_FILE_INFOS[id];
    PcPoolChara*     p  = &s_pool[id];
    s_FsImageDesc    desc;
    s32              ilmSize;
    s32              anmSize;
    s32              slotId = HIRES_POOL_CHARA_SLOT_BASE + id;

    if (fi->modelFileIdx == (s16)NO_VALUE || fi->animFileIdx == (s16)NO_VALUE)
    {
        return 0;
    }

    ilmSize = Fs_GetFileSectorAlignedSize(fi->modelFileIdx);
    anmSize = Fs_GetFileSectorAlignedSize(fi->animFileIdx);
    if (ilmSize <= 0 || anmSize <= 0)
    {
        /* Zero-length file-table rows exist (e.g. HB_M1S04.ANM). */
        return 0;
    }

    if (p->ilmBuf == NULL || p->ilmCap < ilmSize)
    {
        free(p->ilmBuf);
        p->ilmBuf = (u8*)malloc(ilmSize);
        p->ilmCap = ilmSize;
    }
    if (p->anmBuf == NULL || p->anmCap < anmSize)
    {
        free(p->anmBuf);
        p->anmBuf = (u8*)malloc(anmSize);
        p->anmCap = anmSize;
    }
    if (p->ilmBuf == NULL || p->anmBuf == NULL)
    {
        SH_DBG("[POOL] chara %d: out of memory (ilm %d anm %d)", id, ilmSize, anmSize);
        return 0;
    }

    /* Synthetic virtual-slot desc (canonical encoding: hires_override.h).
     * The tpage byte only feeds the prim's page bits, which the GL override
     * path ignores; the clut word carries the slot id. */
    desc.tPage[0] = 0;
    desc.tPage[1] = 28;
    desc.u        = 0;
    desc.v        = 0;
    desc.clutX    = (s16)((slotId % 64) * 16);
    desc.clutY    = (s16)(HIRES_POOL_CLUT_ROW_BASE + (slotId / 64) * HIRES_POOL_MAX_ROWS);

    /* A reload means the file idxs changed (region/ending patch): drop the
     * old GL textures (+ row-spill aliases) and force a fresh ANM read — the
     * explicit-buffer alloc path would otherwise see the same buffer pointer
     * and skip the disc read. */
    if (p->loaded)
    {
        HiresOverride_CharaPoolSlotReset(slotId);
        if (p->animFileIdx != fi->animFileIdx)
        {
            memset(&g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(id)], 0, sizeof(s_CharaAnimData));
        }
        p->loaded = 0;
    }

    memset(&p->model, 0, sizeof(p->model));
    p->model.charaId  = (u8)id;
    p->model.isLoaded = false;
    p->model.lmHdr    = (s_LmHeader*)p->ilmBuf;
    p->model.texture  = desc;
    p->model.queueIdx = Fs_QueueStartRead(fi->modelFileIdx, p->ilmBuf);
    if (fi->textureFileIdx != (s16)NO_VALUE)
    {
        p->model.queueIdx = Fs_QueueStartReadTim(fi->textureFileIdx, FS_BUFFER_1, &desc);
    }

    /* Fs_CharaAnimDataUpdate unconditionally redirects g_CharaAnimDataIdxs
     * to the pool slot — snapshot a live NATIVE idx (map's own group slots
     * from game_load case 5) so it can be restored right after: native maps
     * must keep their vanilla anim binding (spawn-table rows, bone-coord
     * chain) with the pool on. */
    {
        s8 prevIdx = g_CharaAnimDataIdxs[id];

        Fs_CharaAnimDataAlloc(PC_CHARA_ANIM_SLOT(id), id, (s_AnmHeader*)p->anmBuf, s_poolBoneCoords[id]);

        /* Synchronous on PC (CdRead via PsyCross); 3 queue entries per chara so
         * queue-index recycling can't bite ProcessLoad's Fs_QueueIsEntryLoaded. */
        Fs_QueueWaitForEmpty();
        WorldGfx_CharaModelProcessLoad(&p->model);

        if (prevIdx >= 1 && prevIdx < CHARA_GROUP_COUNT &&
            g_CharaModelAnimsData[prevIdx].activeCharaId == id &&
            g_CharaModelAnimsData[prevIdx].activeAnmHdr != NULL)
        {
            g_CharaAnimDataIdxs[id] = prevIdx;
        }
    }

    if (!p->model.isLoaded ||
        g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(id)].activeAnmHdr == NULL)
    {
        SH_DBG("[POOL] chara %d load FAILED (model=%d anm=%p)", id,
               (int)p->model.isLoaded,
               (void*)g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(id)].activeAnmHdr);

        /* Undo any half-registered ready-state so the SPAWN gates report
         * not-ready instead of offering an invisible/garbled spawn; the next
         * map load retries. */
        if (g_CharaAnimDataIdxs[id] == (s8)PC_CHARA_ANIM_SLOT(id))
        {
            g_CharaAnimDataIdxs[id] = (s8)NO_VALUE;
        }
        if (g_WorldGfxWork.registeredCharaModels[id] == &p->model)
        {
            g_WorldGfxWork.registeredCharaModels[id] = NULL;
        }
        return 0;
    }

    p->modelFileIdx   = fi->modelFileIdx;
    p->textureFileIdx = fi->textureFileIdx;
    p->animFileIdx    = fi->animFileIdx;
    p->loaded         = 1;
    return 1;
}

void Pc_CharaPool_Refresh(void)
{
    s32 id;

    if (!g_PcConfig.globalCharaPool || !s_poolReady)
    {
        return;
    }

    for (id = POOL_CHARA_FIRST; id <= POOL_CHARA_LAST; id++)
    {
        PcPoolChara* p = &s_pool[id];

        if (!p->loaded)
        {
            continue;
        }

        /* Model: a LIVE native registration wins. Overlay transitions leave
         * registeredCharaModels of the PREVIOUS map's charas dangling at
         * slots that now hold another chara (WorldGfx_MapInitCharaLoad zeroes
         * slot charaIds before reloading, defeating the eviction NULLing),
         * so non-NULL is not enough — the slot must still be OURS. */
        {
            s_CharaModel* m = g_WorldGfxWork.registeredCharaModels[id];

            if (m == NULL || m->charaId != (u8)id)
            {
                g_WorldGfxWork.registeredCharaModels[id] = &p->model;
            }
        }

        /* Anim: keep a LIVE native binding (vanilla bone-coord layout + the
         * spawn-table row guard depend on native idxs; PoolChara_Load
         * restores it after its own alloc redirected the idx). Anything
         * stale — never set, or a 1..3 slot now owned by another chara —
         * goes to the pool slot. The pool copy is preferred over scanning
         * for leftover native slots: a map with fewer chara groups keeps the
         * PREVIOUS map's data in its unused slots (Chara_None alloc returns
         * early), and those can be repurposed mid-map without notice. */
        {
            s8  idx        = g_CharaAnimDataIdxs[id];
            int nativeLive = idx >= 1 && idx < CHARA_GROUP_COUNT &&
                             g_CharaModelAnimsData[idx].activeCharaId == id &&
                             g_CharaModelAnimsData[idx].activeAnmHdr != NULL;

            if (!nativeLive &&
                g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(id)].activeCharaId == id &&
                g_CharaModelAnimsData[PC_CHARA_ANIM_SLOT(id)].activeAnmHdr != NULL)
            {
                g_CharaAnimDataIdxs[id] = (s8)PC_CHARA_ANIM_SLOT(id);
            }
        }
    }
}

void Pc_CharaPool_OnMapLoad(void)
{
    s32 id;
    s32 n = 0;

    if (!g_PcConfig.globalCharaPool)
    {
        return;
    }

    for (id = POOL_CHARA_FIRST; id <= POOL_CHARA_LAST; id++)
    {
        PcPoolChara*     p  = &s_pool[id];
        s_CharaFileInfo* fi = &CHARA_FILE_INFOS[id];

        if (p->loaded && p->modelFileIdx == fi->modelFileIdx &&
            p->textureFileIdx == fi->textureFileIdx &&
            p->animFileIdx == fi->animFileIdx)
        {
            continue;
        }

        n += PoolChara_Load(id);
    }

    if (n != 0)
    {
        SH_DBG("[POOL] %d chara asset set(s) loaded", n);
    }

    s_poolReady = 1;
    Pc_CharaPool_Refresh();
}

/* ---- chara_global.dll: AI update funcs for every portable monster ------- */

static s_MapOverlayHdr* s_globalHdr; /* NULL until the DLL is opened */

void Pc_CharaGlobal_Open(void)
{
    void* dll;

    if (!g_PcConfig.globalCharaPool || s_globalHdr != NULL)
    {
        return;
    }

    /* Own handle, never closed: MapOverlay_Load's single-slot handle would
     * FreeLibrary it on the next transition, killing live fn ptrs. */
#if defined(_WIN32)
    dll = DllLoader_Open("maps/chara_global.dll");
#elif defined(__APPLE__)
    dll = DllLoader_Open("maps/chara_global.dylib");
#else
    dll = DllLoader_Open("maps/chara_global.so");
#endif
    if (dll == NULL)
    {
        SH_DBG("[POOL] chara_global.dll not found — no AI backfill");
        return;
    }

    s_globalHdr = (s_MapOverlayHdr*)DllLoader_GetSymbol(dll, "g_MapOverlayHeader_chara_global");
    if (s_globalHdr == NULL)
    {
        SH_DBG("[POOL] chara_global.dll: header symbol missing");
    }
}

void Pc_CharaGlobal_Backfill(void)
{
    s32 id;
    s32 n = 0;

    if (!g_PcConfig.globalCharaPool || s_globalHdr == NULL)
    {
        return;
    }

    /* NULL-only: native maps keep their native per-map AI variants, and the
     * known runtime hot-swaps (map4_s03 twinfeeler, map7_s03 incubator)
     * target slots their maps populate. The DLL header is a fresh copy per
     * LoadLibrary, so this must run on every map load (it does: called from
     * MapRegistry_Load). */
    for (id = 0; id < Chara_Count; id++)
    {
        if (g_MapOverlayHdr.charaUpdateFuncs[id] == NULL &&
            s_globalHdr->charaUpdateFuncs[id] != NULL)
        {
            g_MapOverlayHdr.charaUpdateFuncs[id] = s_globalHdr->charaUpdateFuncs[id];
            n++;
        }
    }

    if (n != 0)
    {
        SH_DBG("[POOL] AI backfill: %d chara slot(s) from chara_global", n);
    }
}
