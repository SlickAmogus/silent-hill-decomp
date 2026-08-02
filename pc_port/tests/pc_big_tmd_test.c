/* pc_big_tmd_test.c — oversized loose ITEM TMD capacity lift.
 *
 * Links the REAL pc_big_tmd.c against the REAL g_FileTable/g_FilePaths from
 * src/main/fileinfo.c, so path building, slot arithmetic and the accept/reject
 * verdicts are exercised exactly as the game does them.
 *
 * Covers:
 *   1. loose files OFF  -> pure identity, no substitution  (STOCK GUARANTEE)
 *   2. loose files ON, no file present -> identity          (STOCK GUARANTEE)
 *   3. loose file present but FITS the slot -> identity     (STOCK GUARANTEE)
 *   4. oversized VALID file -> substituted, capacity lifted, resolve follows,
 *      buffer sized for GsMapModelingData's worst-case read extent
 *   5. oversized MALFORMED files -> rejected, identity restored
 *   6. item switching: modded item then stock item must NOT keep the modded
 *      buffer (the shared-FS_BUFFER_5 stale-pointer hazard)
 *   7. a second, different UNQ id also lifts (keyed per file, not per item)
 */
/* NDEBUG is set by the RelWithDebInfo build; this test IS its assertions, so
 * force them on before <assert.h> is pulled in. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"
#include "main/fileinfo.h"
#include "pc_big_tmd.h"
#include "pc_config.h"

/* --- minimal environment the module needs ------------------------------- */
s_PcConfig g_PcConfig;

FILE* Pc_LooseFOpen(const char* path, const char* mode) { return fopen(path, mode); }

FILE* g_ShDebugLog;
int   g_ShDebugEchoStdout;
void (*g_ShOverlayPushLine)(const char*);

/* fileinfo.c references the audio table from unrelated helpers this test never
 * calls; a zeroed definition satisfies the link without pulling in sound code. */
#include "bodyprog/sound/sound_system.h"
s_AudioItemData g_AudioData[1];

/* --- test data ---------------------------------------------------------- */
#define UNQ21 1516 /* ITEM/UNQ21.TMD */
#define UNQ22 1517 /* ITEM/UNQ22.TMD */

static u8 g_slab[64 * 1024]; /* stands in for FS_BUFFER_5 */

static void wr32(u8* p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

/* Build a syntactically valid TMD: 1 object, `nv` verts, `np` F3 prims
 * (ilen=3 -> 16 bytes each), padded out to at least `padTo` bytes. */
static size_t build_tmd(u8* out, u32 id, u32 nobj, u32 nv, u32 np, size_t padTo)
{
    const u32 primBytes = np * 16u;
    u32       primtop, vertop, nortop;
    size_t    total;
    u32       i;

    primtop = nobj * 28u;                /* offsets are from file+12 */
    vertop  = primtop + primBytes;
    nortop  = vertop + nv * 8u;
    total   = 12u + nortop + nv * 8u;

    memset(out, 0, padTo > total ? padTo : total);
    wr32(out + 0, id);
    wr32(out + 4, 0);
    wr32(out + 8, nobj);
    for (i = 0; i < nobj; i++)
    {
        u8* o = out + 12 + i * 28;
        wr32(o + 0,  vertop);  wr32(o + 4,  nv);
        wr32(o + 8,  nortop);  wr32(o + 12, nv);
        wr32(o + 16, primtop); wr32(o + 20, np);
        wr32(o + 24, 0);
    }
    for (i = 0; i < np; i++)
    {
        u8* p = out + 12 + primtop + i * 16;
        p[0] = 3;    /* olen */
        p[1] = 3;    /* ilen -> 4 + 12 = 16 bytes */
        p[2] = 0x20; /* flag/mode: untextured flat tri */
        p[3] = 0x20;
    }
    return padTo > total ? padTo : total;
}

static void write_file(const char* path, const u8* b, size_t n)
{
    FILE* f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(b, 1, n, f) == n);
    fclose(f);
}

static void rm(const char* path) { remove(path); }

static const char* P21 = "gamedata/load/ITEM/UNQ21.TMD";
static const char* P22 = "gamedata/load/ITEM/UNQ22.TMD";

int main(void)
{
    static u8 buf[512 * 1024];
    size_t    n;
    void*     r;
    long      slot21;

    assert(mkdir("gamedata", 0755) == 0 || 1);
    mkdir("gamedata/load", 0755);
    mkdir("gamedata/load/ITEM", 0755);
    rm(P21); rm(P22);

    /* g_FileTable is filled at runtime from the detected disc region. Without
     * this every blockCount is 0, the slot size is 0, and the test would pass
     * vacuously. */
    Fs_InitFileTableForRegion(Region_USA);

    slot21 = Fs_GetFileSectorAlignedSize(UNQ21);
    printf("UNQ21 slot = %ld bytes\n", slot21);
    assert(slot21 == 6144); /* blockCount 18 * 256 = 4608 -> sector-aligned 6144 */

    /* --- 1. loose files OFF: pure identity, even with a big file present -- */
    n = build_tmd(buf, 0x41, 1, 900, 1800, 0);
    assert((long)n > slot21);
    write_file(P21, buf, n);

    g_PcConfig.allowLooseFiles = 0;
    r = Pc_BigTmd_Redirect(UNQ21, g_slab);
    assert(r == g_slab);
    assert(Pc_BigTmd_Resolve(g_slab) == g_slab);
    assert(Pc_BigTmd_DestCapacity(g_slab) == 0);
    printf("1. loose OFF -> identity                     OK\n");

    /* --- 2. loose ON, file absent: identity ------------------------------ */
    rm(P21);
    g_PcConfig.allowLooseFiles = 1;
    r = Pc_BigTmd_Redirect(UNQ21, g_slab);
    assert(r == g_slab);
    assert(Pc_BigTmd_Resolve(g_slab) == g_slab);
    assert(Pc_BigTmd_DestCapacity(g_slab) == 0);
    printf("2. loose ON, no file -> identity             OK\n");

    /* --- 3. loose file present but FITS the slot: identity --------------- */
    n = build_tmd(buf, 0x41, 1, 40, 60, 0);
    assert((long)n <= slot21);
    write_file(P21, buf, n);
    r = Pc_BigTmd_Redirect(UNQ21, g_slab);
    assert(r == g_slab); /* ordinary byte-replace path keeps ownership */
    assert(Pc_BigTmd_Resolve(g_slab) == g_slab);
    assert(Pc_BigTmd_DestCapacity(g_slab) == 0);
    printf("3. fits slot -> identity (byte-replace owns) OK\n");

    /* --- 4. oversized + valid: substituted -------------------------------- */
    n = build_tmd(buf, 0x41, 1, 900, 1800, 0);
    write_file(P21, buf, n);
    r = Pc_BigTmd_Redirect(UNQ21, g_slab);
    assert(r != NULL && r != g_slab);
    {
        size_t cap = Pc_BigTmd_DestCapacity(r);
        /* Worst case GsMapModelingData reaches: 12 + max(copySize, dataEnd),
         * where a prim is bounded at 64 bytes. Must be far beyond the file. */
        u32 primtop = 28, np = 1800, nv = 900;
        u32 vertop  = primtop + np * 16u;
        u32 nortop  = vertop + nv * 8u;
        u32 dataEnd = nortop + nv * 8u;
        u32 pEnd    = primtop + np * 64u;
        size_t need;
        if (pEnd > dataEnd) dataEnd = pEnd;
        need = 12u + (size_t)dataEnd + 28u; /* copySize includes the obj table */
        printf("   file=%zu cap=%zu (need>=%zu)\n", n, cap, need);
        assert(cap >= need);
        assert(cap > n); /* strictly larger than the file: the stub over-reads */
        assert(Pc_BigTmd_DestCapacity(g_slab) == 0); /* slab never registered */
        assert(Pc_BigTmd_Resolve(g_slab) == r);      /* consumer follows */
    }
    printf("4. oversized valid -> substituted + lifted   OK\n");

    /* --- 5. malformed oversized files are rejected ------------------------ */
    {
        struct { const char* what; u32 id, nobj, nv, np; } bad[] = {
            { "bad id",        0x99, 1,  900, 1800 },
            { "nobj=0",        0x41, 0,  900, 1800 },
            { "nobj>48",       0x41, 60, 900, 1800 },
        };
        unsigned i;
        for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        {
            n = build_tmd(buf, bad[i].id, bad[i].nobj ? bad[i].nobj : 1,
                          bad[i].nv, bad[i].np, 0);
            /* re-stamp the header fields the builder normalised */
            wr32(buf + 0, bad[i].id);
            wr32(buf + 8, bad[i].nobj);
            write_file(P21, buf, n + i + 1); /* +i keeps sizes distinct so the
                                              * verdict cache re-validates */
            r = Pc_BigTmd_Redirect(UNQ21, g_slab);
            assert(r == g_slab);
            assert(Pc_BigTmd_Resolve(g_slab) == g_slab);
            printf("5.%u malformed (%-10s) -> rejected, identity OK\n", i + 1, bad[i].what);
        }

        /* vern over the draw-time guard (0x2000) */
        n = build_tmd(buf, 0x41, 1, 900, 1800, 0);
        wr32(buf + 12 + 4, 0x2001);
        write_file(P21, buf, n);
        r = Pc_BigTmd_Redirect(UNQ21, g_slab);
        assert(r == g_slab);
        printf("5.4 malformed (%-10s) -> rejected, identity OK\n", "vern huge");
        /* truncated: object table claims arrays past EOF */
        n = build_tmd(buf, 0x41, 1, 900, 1800, 0);
        write_file(P21, buf, n / 2);
        r = Pc_BigTmd_Redirect(UNQ21, g_slab);
        assert(r == g_slab);
        printf("5.5 malformed (%-10s) -> rejected, identity OK\n", "truncated");
    }

    /* --- 6. item switch must not leave the modded buffer standing --------- */
    n = build_tmd(buf, 0x41, 1, 900, 1800, 0);
    write_file(P21, buf, n);
    r = Pc_BigTmd_Redirect(UNQ21, g_slab);            /* modded item */
    assert(r != g_slab);
    assert(Pc_BigTmd_Resolve(g_slab) == r);
    {
        void* stock = Pc_BigTmd_Redirect(UNQ22, g_slab); /* stock item, no file */
        assert(stock == g_slab);
        assert(Pc_BigTmd_Resolve(g_slab) == g_slab);  /* substitution CLEARED */
    }
    printf("6. modded->stock item clears substitution    OK\n");

    /* --- 7. a different UNQ id lifts too (keyed per file) ----------------- */
    n = build_tmd(buf, 0x41, 1, 700, 1400, 0);
    write_file(P22, buf, n);
    {
        void* a = Pc_BigTmd_Redirect(UNQ21, g_slab);
        void* b;
        assert(a != g_slab);
        b = Pc_BigTmd_Redirect(UNQ22, g_slab);
        assert(b != g_slab);
        assert(b != a); /* separate buffers, separate registry entries */
        assert(Pc_BigTmd_Resolve(g_slab) == b);
        assert(Pc_BigTmd_DestCapacity(a) > 0 && Pc_BigTmd_DestCapacity(b) > 0);
    }
    printf("7. second UNQ id lifts independently         OK\n");

    rm(P21); rm(P22);
    printf("\npc_big_tmd_test: ALL PASS\n");
    return 0;
}
