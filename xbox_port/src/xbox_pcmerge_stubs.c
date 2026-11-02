/*
 * xbox_pcmerge_stubs.c - Xbox values for the PC port's renderer/UI hooks that
 * the shared game code reaches since the October pc-port merge.
 *
 * Each symbol here is defined on PC by PsyCross (GL renderer, SDL input,
 * OpenAL) or by a PC-only overlay (quick options F10 menu, achievement list,
 * randomizer panel, key-bind panel, plugins, texture packs). None of those
 * exist on the console, so every value is the one that makes the shared code
 * behave exactly as it did before the merge: the PSX 4:3 frame, no Hor+, no
 * GL-side effects, no PC overlay ever open. Real Xbox implementations replace
 * entries here as features are ported (see the Xbox roadmap memory).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libgte.h>
#include <libgpu.h>
#include "PsyX/PsyX_public.h"   /* PsyX_SfxOverrideFn */

/* ---- PsyCross renderer state ---------------------------------------------- */
DISPENV   activeDispEnv;
DRAWENV   activeDrawEnv;

/* The Xbox shows the PSX frame at 4:3 with no horizontal/vertical FOV knobs.
 * HScale 1.0 + pixel aspect 1.0 keep the glow-ring fill (bodyprog_80040B74.c
 * Pc_ScreenRingHalfW) clamped to the authored 160-wide half frame. */
float     g_PsxWorldHScale       = 1.0f;
float     g_PsxWorldVScale       = 1.0f;
float     g_PsxCutsceneVShift    = 0.0f;
float     g_PsxCrtAspectTrim     = 1.0f;
int       g_PsxAspectRaw         = 0;
float     g_PsxWorldDisp[2]      = { 0.0f, 0.0f };
float     g_PsxWorldOrtho[4]     = { 0.0f, 0.0f, 0.0f, 0.0f };
int       g_PsxWorldOrthoValid   = 0;
/* The OT2 layer's visible rect in prim coordinates {left, right, top, bottom}:
 * the in-game 320x224 frame around its centre, the rect PsyCross latches at
 * 4:3. The minimap places itself against it; zeros put it in mid-screen. */
float     g_PcHudRect[4]         = { -160.0f, 160.0f, -112.0f, 112.0f };
float     g_PcWorldOrthoRect[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
int       g_PsxFrame3dClass      = 0;
int       g_PsxItemTakeActive    = 0;
int       g_PsxUi2dBucket        = -1;
uintptr_t g_PsxOtNoBucketLo      = 0;
uintptr_t g_PsxOtNoBucketHi      = 0;
int       g_PsxWholeMapProbe     = 0;
double    g_wmTChunks, g_wmTXform, g_wmTMesh;

/* PSX fog density the shared code compares against 1.0 ("thinned fog" spawn
 * rules, whole-map gating). 1.1 is PsyCross's PSX-matched default; the Xbox
 * fog itself is the PSX depth cue in the prims and never reads this. */
float     g_PsyX_FogStrength     = 1.1f;
float     g_PsyX_CharaFade       = 1.0f;
int       g_PsyX_ShadowMapSize   = 1024;
int       g_PsyX_ItemDepthProbe  = 0;
int       g_PsyX_WheelNotches    = 0;

/* Framebuffer-feedback (dream blur / loading trail) knobs. The Xbox blur is its
 * own demand-driven readback (gpu_nv2a.c) and does not consult these. */
int       g_PsxFeedbackStoreAllowed = 0;
int       g_PsxFeedbackHoldFrame    = 0;
float     g_PsxFeedbackDampBlend    = 1.0f;
int       g_cfg_dreamFeedback       = 1;

/* Image adjustments / filtering rows in the PC options pages. */
int       g_cfg_textureFilter       = 0;
int       g_cfg_anisoLevel          = 8;
float     g_cfg_brightness          = 1.0f;
float     g_cfg_contrast            = 1.0f;
float     g_cfg_saturation          = 1.0f;
int       g_cfg_calibBar            = 0;
int       g_cfg_disableDpadMovement = 0;
int       g_dbg_texturelessMode     = 0;
int       g_dbg_wireframeMode       = 0;
int       g_skipSwapInterval        = 0;

PsyX_SfxOverrideFn g_PsyX_SfxOverride = NULL;

int   GetVideoMode(void)                          { return 0; } /* MODE_NTSC */
float GR_LivePixelAspect(void)                    { return 1.0f; }
int   GR_HorPlusHalfWidths(float* a, float* b)    { (void)a; (void)b; return 0; }
void  GR_SetPsxDisplayBuffers(int x0, int y0, int x1, int y1, int w, int h)
{ (void)x0; (void)y0; (void)x1; (void)y1; (void)w; (void)h; }
void  GR_SetSceneFbRedirect(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; }

void  PsyX_SetNextPrimDepthSz(unsigned int sz)    { (void)sz; }
void  PsyX_CancelNextPrimSz(void)                 { }
void  PsyX_SetOtViewZShift(int shiftEff)          { (void)shiftEff; }
void  PsyX_SetNextPrimNoFlashlight(void)          { }
void  PsyX_SetNextPrimAlpha(int a)                { (void)a; }
void  PsyX_ItemProbeModelBegin(int s, int sl, int a) { (void)s; (void)sl; (void)a; }
void  PsyX_ItemProbeSortPrim(void* p, int otz)    { (void)p; (void)otz; }
void  PsyX_ItemProbeEndFrame(void)                { }
void  PsyX_SPUAL_SetOutputMode(int mode)          { (void)mode; }

/* Whole-map far band (PC only: GPU-resident meshes + a z-buffered far pass).
 * Pc_WholeMapDrawActive() is false on Xbox, so these are unreachable; NULL
 * returns are the "nothing uploaded" answer the callers already handle. */
void          PsyX_FarMeshFrameBegin(const PsyX_FarMeshFrame* f) { (void)f; }
void          PsyX_FarMeshFree(unsigned handle)                   { (void)handle; }
void          PsyX_FarMeshQueue(unsigned handle, const float base[3], const float eye[3],
                                const PsyX_FarMeshRange* ranges, int rangeCount)
{ (void)handle; (void)base; (void)eye; (void)ranges; (void)rangeCount; }
unsigned      PsyX_FarMeshUpload(unsigned handle, const PsyX_FarMeshVertex* v, int vertexCount,
                                 const unsigned* indices, int indexCount)
{ (void)handle; (void)v; (void)vertexCount; (void)indices; (void)indexCount; return 0; }
PsyX_FarQuad* PsyX_FarQuadAlloc(int* index)                      { (void)index; return NULL; }
void          PsyX_FarQuadsReset(void)                            { }

/* ---- PC-only overlays: never open on the console --------------------------- */
int   g_PcQuickOptionsActive = 0;
void  Pc_QuickOptions_Update(int a, int b, int c, int d, int e, int f, int g, int h)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; }
void  Pc_QuickOptions_Close(void)                 { }
void  Pc_QuickOptions_PreloadGL(void)             { }
void  Pc_QuickOptions_InvalidateRows(void)        { }
int   Pc_QuickOptions_ShowsMinimapRows(void)      { return 0; }

void  Pc_RaToast_Preload(void)                    { }

int   Pc_RandoSettings_IsOpen(void)               { return 0; }
int   Pc_RandoSettings_MapButtonArbiter(int a, int b) { (void)a; (void)b; return 0; }
void  Pc_RandoSettings_Update(int a, int b, int c, int d, int e, int f)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }

void  Pc_BindPanel_Open(void)                     { }
int   Pc_BindPanel_Update(void)                   { return 0; }
int   PcBinds_ParseConfigLine(const char* line)   { (void)line; return 0; }

void  Pc_FreeCamHud_Preload(void)                 { }
void  Pc_Plugins_OnNewGame(void)                  { }
void  Pc_Plugins_OnRender(void)                   { }
void  Pc_Plugins_OnUpdate(void)                   { }
int   g_DllAllowUnrecognized = 0;

/* A pad is the only input device; no mouse ever owns the cursor. */
int          Pc_ControllerAttached(void)          { return 1; }
unsigned int Pc_MouseCursor_BoundPadBits(void)    { return 0; }
int          Pc_MouseCursor_PuzzleActive(void)    { return 0; }

/* Console-only object picking + spawn list (pc_pick.c / pc_console_cmd.c). */
int         Pc_SpawnList_Count(void)              { return 0; }
const char* Pc_SpawnList_Name(int i)              { (void)i; return ""; }
int         Pc_SpawnList_Ready(int i)             { (void)i; return 0; }
void        Pc_SpawnList_Spawn(int i)             { (void)i; }
void        Pc_Console_Print(const char* line)    { (void)line; }
const char* Pc_Console_CharaName(int charaId)     { (void)charaId; return ""; }

/* pc_combat.c is excluded for its SDL binds; the manual-reload request it owns
 * is never raised here, and the heal flash only follows a Quick Heal (also a
 * bind). The low-health glow is plain prims: xbox_combat_fx.c. */

/* Texture packs (tex_pack.c / texpack_lazy.c) are PC-only. */
void  TexPackLazy_Pump(void)                      { }
void  TexPackLazy_RegisterSlotSource(int slot, int fileIdx, int tpage, int clut)
{ (void)slot; (void)fileIdx; (void)tpage; (void)clut; }
void  TexPack_DumpScanLm(const void* raw)         { (void)raw; }
void  TexPack_DumpUpload(const void* a, int b, int c, int d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
int   TexPack_LastComposeWasBuilt(void)           { return 0; }

/* The game's data folder, relative: fs_path_xbox.c resolves it to Q:\gamedata. */
const char* PcPort_GetGameDataPath(void)          { return "gamedata"; }

/* The Xbox log is flushed by its own 60-vblank cadence (sh_log_xbox.c). */
void  Sh_LogPeriodicFlush(void)                   { }

char* getcwd(char* buf, size_t size)
{
    if (buf == NULL || size < 4)
        return NULL;
    strcpy(buf, "Q:\\");
    return buf;
}

long long _ftelli64(FILE* f) { return (long long)ftell(f); }

/* ---- Hi-res override entry points added since the merge -------------------
 * hires_override.c is the PC's GL texture-override table; the Xbox hi-res
 * path is xbox_respool.c / xbox_pcfeature_stubs.c. These newer calls (DDS
 * packs, the pinned PAL font grid, per-slot CLUT stamps) find no entry here,
 * which is the documented "no override" answer for each. */
int  HiresOverride_PinVramRect(int x, int y, int w, int h, int cx, int cy, int bpp, int pin)
{ (void)x; (void)y; (void)w; (void)h; (void)cx; (void)cy; (void)bpp; (void)pin; return -1; }
int  HiresOverride_DropVramRect(int x, int y, int w, int h, int cx, int cy, int bpp)
{ (void)x; (void)y; (void)w; (void)h; (void)cx; (void)cy; (void)bpp; return -1; }
void HiresOverride_PoolSlotSetNativeClut(int slotId, int clutX, int clutY)
{ (void)slotId; (void)clutX; (void)clutY; }
int  HiresOverride_RegisterDdsKeyed(const char* label, const unsigned char* dds, size_t ddsSize,
                                    int vx, int vy, int vw, int vh, int cx, int cy, int bpp,
                                    unsigned long long hash)
{ (void)label; (void)dds; (void)ddsSize; (void)vx; (void)vy; (void)vw; (void)vh; (void)cx; (void)cy; (void)bpp; (void)hash; return 0; }
unsigned short HiresOverride_RestampValidate(unsigned short newClut, unsigned short baseClut,
                                             unsigned short oldClut, unsigned short oldBase,
                                             const char* matName)
{ (void)baseClut; (void)oldClut; (void)oldBase; (void)matName; return newClut; }
void HiresOverride_SetForceNearestUpload(int on)  { (void)on; }

/* ---- stb_truetype probe in lang_quick.c ------------------------------------
 * The PC picks a system TTF that can draw the quick menu's CJK glyphs; the
 * Xbox has no quick menu and no system fonts, so the probe always misses. */
int stbtt_GetFontOffsetForIndex(const unsigned char* data, int index) { (void)data; (void)index; return -1; }
int stbtt_InitFont(void* info, const unsigned char* data, int offset) { (void)info; (void)data; (void)offset; return 0; }
int stbtt_FindGlyphIndex(const void* info, int codepoint)             { (void)info; (void)codepoint; return 0; }

/* ---- Modern glTF item meshes (pc_modern_*.c, GL-only) ----------------------
 * Every lookup misses, which is the documented stock-item answer: SelectRead
 * hands back the original read destination, LinkObject/Emit change nothing. */
const void* Pc_ModernMesh_Find(int fileIdx)                  { (void)fileIdx; return NULL; }
void*       Pc_ModernMesh_SelectRead(int fileIdx, void* dst) { (void)fileIdx; return dst; }
int         Pc_ModernMesh_LinkObject(int fileIdx, void* obj) { (void)fileIdx; (void)obj; return 0; }
int         Pc_ModernMesh_Emit(void* obj, void* ot, int shift) { (void)obj; (void)ot; (void)shift; return 0; }

/* pc_pick.c maps a mouse click into the viewport; there is no mouse. */
int PsyX_MapWindowToViewport(int mx, int my, float* fx, float* fy)
{ (void)mx; (void)my; (void)fx; (void)fy; return 0; }
