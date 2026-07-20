/*
 * xbox_pcfeature_stubs.c - Link-time stubs for PC-port-only subsystems the
 * shared decomp calls under SH_PC_PORT (defined on Xbox too, see
 * game-code-wiring.md) but whose real implementations live in PC-only source
 * files the Xbox build deliberately does not compile: the OpenGL/PsyCross
 * renderer internals (PsyX_render.cpp / PsyX_GPU.cpp / PsyX_GTE.cpp's PGXP
 * half), the mouse cursor, HD texture packs / hi-res TIM overrides (stb_image
 * PNG decoding), map overlay DLLs, and a few PC-only debug/QoL globals.
 *
 * Every value here was picked by reading (a) how the shared game code in
 * src/ consumes the symbol and (b) its real PC definition/initializer in
 * pc_port/ or pc_port/PsyCross/, so the Xbox behaves as if each PC-only
 * feature is OFF while anything that is genuinely cross-platform shared
 * logic (bullet hitbox tuning, whole-map far-poly re-projection, letterbox
 * bar Y, PGXP integer-fallback backface culling) gets the SAME constants PC
 * ships, not a blind zero. See the per-symbol comments below for the specific
 * reasoning; a few are flagged as follow-ups rather than silent guesses.
 *
 * Checked against xbox_compat_globals.c, xbox_compat_stubs.c, xa_xbox.c,
 * map_xbox.c, pgxp_stub.c and every other file in xbox_port/src before
 * writing this — nothing here duplicates a symbol already defined there.
 *
 * Deliberately self-contained (only <stddef.h>): the values below are all
 * plain C types (int/float/pointers), so pulling in the decomp's s32/q19_12/
 * VECTOR3 headers would add fragile include-order dependencies for no
 * benefit — those typedefs are themselves just aliases for the types used
 * here, so this is ABI-identical to the real headers' declarations.
 */
#include <stddef.h> /* NULL */

/* =============================================================================
 * libc gap: pdclib (nxdk) ships stdlib.h's `double atof(const char*);`
 * prototype but never implements it (see nxdk/lib/pdclib/include/stdlib.h's
 * "TODO: atof(), strtof(), strtod(), strtold()"). pc_config.c's key=value
 * parser calls it for float config values (e.g. xa_volume, flashlight_size),
 * so this must be a REAL parser, not a stub returning 0.
 *
 * `weak` (not an #ifndef guard) is the correct duplicate-safety mechanism: an
 * #ifndef can't detect a symbol a *future* pdclib update adds since there is
 * no macro to test, but a weak definition simply loses the link to a real
 * (strong) one that shows up later with zero source changes needed here.
 * ========================================================================= */
__attribute__((weak)) double atof(const char* nptr)
{
    const char* p = nptr;
    double sign = 1.0;
    double intPart = 0.0;
    double fracPart = 0.0;
    double fracScale = 1.0;
    double result;
    int expNegative = 0;
    int exponent = 0;
    int haveDigits = 0;

    if (p == NULL)
        return 0.0;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f')
        p++;

    if (*p == '+' || *p == '-')
    {
        if (*p == '-')
            sign = -1.0;
        p++;
    }

    while (*p >= '0' && *p <= '9')
    {
        intPart = intPart * 10.0 + (double)(*p - '0');
        p++;
        haveDigits = 1;
    }

    if (*p == '.')
    {
        p++;
        while (*p >= '0' && *p <= '9')
        {
            fracScale *= 10.0;
            fracPart = fracPart * 10.0 + (double)(*p - '0');
            p++;
            haveDigits = 1;
        }
    }

    if (!haveDigits)
        return 0.0;

    result = intPart + (fracPart / fracScale);

    if (*p == 'e' || *p == 'E')
    {
        const char* expStart = p;
        p++;
        if (*p == '+' || *p == '-')
        {
            expNegative = (*p == '-');
            p++;
        }
        if (*p >= '0' && *p <= '9')
        {
            while (*p >= '0' && *p <= '9')
            {
                exponent = exponent * 10 + (*p - '0');
                p++;
            }
        }
        else
        {
            /* "1e" / "1ex" with no exponent digits: not part of the number. */
            p = expStart;
            exponent = 0;
        }
    }

    if (exponent != 0)
    {
        int i;
        double scale = 1.0;
        for (i = 0; i < exponent; i++)
            scale *= 10.0;
        result = expNegative ? (result / scale) : (result * scale);
    }

    return sign * result;
}

/* =============================================================================
 * PC_OT_SCAN - a pure-diagnostic OT-corruption walk. In the PC tree it is a
 * macro game_main.c defines locally (debug builds call Pc_OtSentinelScan,
 * release builds expand to ((void)0)); neither the macro nor
 * Pc_OtSentinelScan made it into this merge, so the single
 * PC_OT_SCAN("pre-GameStateUpdate") call site in the shared game_main.c
 * resolved as an implicitly-declared function taking a string literal.
 * Define it as the real thing the release-mode macro already was: a no-op.
 * ========================================================================= */
void PC_OT_SCAN(const char* phase)
{
    (void)phase;
}

/* =============================================================================
 * DllLoader_* - the Xbox links all 43 maps statically (map_xbox.c); there are
 * no .dll/.dylib/.so files on the disc or the HDD. The only remaining caller
 * reachable on Xbox is pc_chara_pool.c's optional "chara_global.dll" AI
 * backfill, which already treats DllLoader_Open()==NULL as "not present" and
 * disables itself with a log line — exactly PC's own missing-file path.
 * ========================================================================= */
void* DllLoader_Open(const char* path)
{
    (void)path;
    return NULL;
}

void* DllLoader_GetSymbol(void* handle, const char* name)
{
    (void)handle;
    (void)name;
    return NULL;
}

/* =============================================================================
 * GR_CopyVRAM - PC's on-the-fly Kanji glyph rasterizer (pc_kanji.c) blits
 * decoded glyph cells into PsyCross's own VRAM emulation via this hook.
 * pc_kanji.c is reused on Xbox (SDL-free), but every call site in
 * text_draw.c is gated on `g_GameRegion == Region_JPN`; main_xbox.c always
 * calls Fs_InitFileTableForRegion(Region_USA), so this is unreachable dead
 * code on Xbox today. No-op instead of wiring it into psx_vram.c is correct
 * until/unless a JP region option is ever added to the Xbox port.
 * ========================================================================= */
void GR_CopyVRAM(unsigned short* src, int x, int y, int w, int h, int dst_x, int dst_y)
{
    (void)src;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)dst_x;
    (void)dst_y;
}

/* =============================================================================
 * CollVis_CaptureHitCylinder - collision-visualizer debug overlay (dbg_overlay.c,
 * PC-only, excluded). Its single call site in collision.c is already guarded
 * by `if (g_CollVisEnabled && ...)`, and g_CollVisEnabled is hard-pinned to 0
 * in xbox_compat_globals.c, so this never actually runs; a plain no-op
 * matches dbg_overlay.c's own body (buffers a debug cylinder for on-screen
 * drawing that Xbox has no renderer for anyway).
 * ========================================================================= */
void CollVis_CaptureHitCylinder(int cx, int cy, int cz, int r)
{
    (void)cx;
    (void)cy;
    (void)cz;
    (void)r;
}

/* =============================================================================
 * Control style (control_style.c, excluded - SDL mouse-capture logic).
 * Defaults copied VERBATIM from control_style.c's own initializers: Classic
 * is control style 0, the original PSX tank-control/fixed-camera scheme,
 * which is what pad_xbox.c's straight XID->PSX-pad mapping already assumes.
 * Pc_ControlStyleUpdate (F9/R3 camera-style cycle) is a no-op: Xbox has no
 * rebindable-action layer to poll yet, so the style simply never changes
 * from Classic, which is the correct/faithful behavior, not a missing one.
 * ========================================================================= */
int g_ControlStyle      = 0; /* ControlStyle_Classic */
int g_OtsSide           = 1; /* +1 = right shoulder (control_style.c default) */
int g_PcFpsCam          = 0; /* First-person camera off */
int g_TpsCamNeedsReset  = 0; /* control_style.c default (no pending TPS reseed) */

void Pc_ControlStyleUpdate(void)
{
}

/* =============================================================================
 * PC combat tuning (pc_combat.c, excluded - SDL keyboard/controller reads).
 *
 * g_PcBulletHitActive/VertMul/RadMul are NOT a PC-only code path: the fat-
 * hitbox inflation they drive is compiled unconditionally into the SHARED
 * bodyprog_combat_8008A058.c and ray.c under `#ifdef SH_PC_PORT`, which is
 * defined on Xbox too, so this cross-platform gameplay logic is live and
 * needs PC's real tuning constants, not zeros:
 *   g_PcBulletHitActive: state flag, driven every frame by the shared bullet
 *     trace itself (bodyprog_combat_8008A058.c) - starts at 0 (idle).
 *   g_PcBulletVertMul = Q12(2.0f) = 8192: "vertical reach added each side =
 *     2x collision radius" (pc_combat.c comment) - multiplies a radius at
 *     ray.c:750, so 0 would silently shrink the player's hit-detection back
 *     to the PSX-narrow neck band; 8192 (Q12 of 2.0) is the identity PC uses.
 *   g_PcBulletRadMul  = Q12(1.0f) = 4096: "horizontal radius added = 1x
 *     collision radius" - same reasoning, identity is 4096, not 0.
 *
 * g_PcQuickTurnRequest / g_PcRearLookActive ARE genuinely PC-only QoL binds
 * (Quick Turn / Rear Look), so Pc_ExtraActionsUpdate/Pc_RearLookUpdate are
 * correct no-ops here (Xbox has no bind for them yet); the flags stay 0
 * ("no request pending" / "not held"), matching pc_combat.c's own defaults
 * and leaving normal PSX-style camera/turn behavior untouched.
 * ========================================================================= */
int g_PcBulletHitActive = 0;
int g_PcBulletVertMul   = 8192; /* Q12(2.0f) */
int g_PcBulletRadMul    = 4096; /* Q12(1.0f) */
int g_PcQuickTurnRequest = 0;
int g_PcRearLookActive   = 0;

void Pc_ExtraActionsUpdate(void)
{
}

void Pc_RearLookUpdate(void)
{
}

/* Aim assist (pc_combat.c Pc_AimAssistFind, excluded): mouse/auto-aim body-
 * snap. Its one call site (player_control.c) checks `!= NO_VALUE` (NO_VALUE
 * is -1, decomp/types.h) to mean "assist found a target"; returning NO_VALUE
 * unconditionally is the documented "nothing found" sentinel, which falls
 * through to the plain camera-ray aim point - correct behavior for "feature
 * not implemented on Xbox" (Xbox has no mouse and no separate auto-aim pass
 * yet), not a guessed 0 that could be misread as a valid aim-point index. */
typedef struct { int vx, vy, vz; } SH_StubVec3; /* layout-matches VECTOR3 (3x s32) */

int Pc_AimAssistFind(const SH_StubVec3* camPos, const SH_StubVec3* camFwd, int aimRange, SH_StubVec3* outAimPoint)
{
    (void)camPos;
    (void)camFwd;
    (void)aimRange;
    (void)outAimPoint;
    return -1; /* NO_VALUE */
}

/* =============================================================================
 * Mouse cursor (pc_mouse_cursor.h, control_style.c/pc_mouse_cursor - excluded,
 * SDL mouse reads). Xbox has no mouse; every function reports "disabled /
 * nothing happened this frame", which is the header's own documented
 * behavior for "cursor is disabled" (not a guess - see pc_mouse_cursor.h's
 * per-function comments), so every menu/puzzle click-hint path falls back
 * cleanly to pad-only input.
 * ========================================================================= */
void Pc_MouseCursor_FrameUpdate(void)
{
}

void Pc_MouseCursor_NoteCursorDrawn(int curX, int curY)
{
    (void)curX;
    (void)curY;
}

int Pc_MouseCursor_MenuRowHover(int baseY, int stepY, int count, unsigned int visibleFlags, int* outClicked)
{
    (void)baseY;
    (void)stepY;
    (void)count;
    (void)visibleFlags;
    if (outClicked)
        *outClicked = 0;
    return -1; /* no row hovered */
}

int Pc_MouseCursor_Moved(void)
{
    return 0;
}

int Pc_MouseCursor_UiPos(int* outX, int* outY)
{
    (void)outX;
    (void)outY;
    return 0; /* "outputs untouched" per pc_mouse_cursor.h - cursor disabled */
}

int Pc_MouseCursor_LeftClicked(void)
{
    return 0;
}

int Pc_MouseCursor_LeftHeld(void)
{
    return 0;
}

int Pc_MouseCursor_RightClicked(void)
{
    return 0;
}

int Pc_MouseCursor_WheelStep(void)
{
    return 0;
}

void Pc_MouseCursor_Draw(void)
{
}

/* =============================================================================
 * PsyCross renderer-only hooks (PsyX_render.cpp / PsyX_pad.cpp, excluded -
 * OpenGL window/vsync/depth-state management Xbox's NV2A path (gpu_xbox.c /
 * gpu_nv2a.c) doesn't use).
 * ========================================================================= */

/* Options-menu display-resolution / vsync toggles: no window to resize and
 * pb_kit's own vblank wait already paces presentation (GpuNv2a_WaitVbl). */
void PsyX_ApplyWindowState(int width, int height, int fullscreen)
{
    (void)width;
    (void)height;
    (void)fullscreen;
}

void PsyX_ApplyVsync(int vsync)
{
    (void)vsync;
}

/* Inventory/pickup-item real-depth pass (GL depth-test toggle around the item
 * OT0 draw so a rotating item's own front faces occlude its back faces).
 * gpu_xbox.c has its own NV2A depth handling untouched by this GL-only hook;
 * wiring an equivalent Z-state toggle there is a cosmetic follow-up, not a
 * functional requirement (worst case: the PSX-original see-through remains). */
void PsyX_ForceItemDepthBegin(void)
{
}

void PsyX_ForceItemDepthEnd(void)
{
}

/* Boot-logo raw-controller skip fallback (PsyX_pad.cpp): only used when
 * g_Controller0's heldBtnFlags isn't wired yet THIS early in boot; b_konami.c
 * checks g_Controller0->heldBtnFlags FIRST and only falls back to this. Xbox
 * fills g_Controller0 from pad_xbox.c well before the logo state, and
 * skipIntros=1 already bypasses the Konami logo per the Xbox roadmap, so this
 * redundant fallback returning "not held" is safe; wiring pad_xbox.c's raw
 * button state in here would only help the ~0-frame boot window before
 * g_Controller0 is first filled. */
int PsyX_Pad_SkipButtonHeld(void)
{
    return 0;
}

/* Per-prim PGXP hints (PsyX_GPU.cpp): both early-return when g_PsxUsePgxp is
 * 0 in their real implementations too (pgxp_stub.c already pins
 * g_PsxUsePgxp = 0 on Xbox), so a no-op here is byte-identical to calling the
 * real PC function with PGXP off - not a feature loss. */
void PsyX_SetNextPrimAffine(void)
{
}

void PsyX_SetNextPrimSzExact(unsigned short s0, unsigned short s1, unsigned short s2, unsigned short s3)
{
    (void)s0;
    (void)s1;
    (void)s2;
    (void)s3;
}

/* =============================================================================
 * PGXP precise-projection shadow tables + backface tests (PsyX_GTE.cpp,
 * REUSED on Xbox as the platform-agnostic software GTE - see GTE_SRCS in
 * Makefile.nxdk - calling into PsyX_GPU.cpp, which is NOT reused).
 *
 * PsyX_PGXP_TriBackface/QuadBackface are NOT PGXP-gated at their call sites
 * (bodyprog_80055028.c's whole-map far-poly re-test) - they always run, so
 * this must reproduce the real function's *integer fallback* path exactly
 * (see PsyX_GPU.cpp: `if (g_PsxUsePgxp) {...} return intNcl <= 0 ? 1 : 0;`).
 * g_PsxUsePgxp is 0 on Xbox (pgxp_stub.c), so the float branch never applies
 * on PC either in our configuration - only the integer sign test does, and
 * that is what the shared far-poly culling code actually needs to stay
 * correct. Returning a constant 0/1 here would silently break far-ground
 * culling (docs/WholeMap_Far_Projection_Task.md).
 * ========================================================================= */
int PsyX_PGXP_TriBackface(const void* a0, const void* a1, const void* a2, int intNcl)
{
    (void)a0;
    (void)a1;
    (void)a2;
    return intNcl <= 0 ? 1 : 0;
}

int PsyX_PGXP_QuadBackface(const void* a0, const void* a1, const void* a2, const void* a3, int intN012, int intN312)
{
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return (intN012 <= 0 && intN312 >= 0) ? 1 : 0;
}

/* Shadow_Store/VShadow_Store are called unconditionally from PsyX_GTE.cpp's
 * PGXP_StoreAddr (compiled on Xbox) whenever the gte_stsxy* macros fire, but
 * PGXP_StoreAddr itself only calls them behind `if (g_PsxUsePgxp)` /
 * `if (g_PsyX_UsePerPixelFlashlight || g_PsxUsePgxp)` - both 0 on Xbox - so
 * in practice these bodies never execute. Shadow_Copy IS called
 * unconditionally from shared drawer code (water.c, bodyprog_80055028.c,
 * libgs_stub.c) every time a screen vertex is copied into a prim, with no
 * flag gate at the call site, so it must be a safe, always-callable no-op
 * (which is exactly what the real Shadow_Copy reduces to when both
 * g_PsxUsePgxp and g_PsyX_UsePerPixelFlashlight are 0, per PsyX_GPU.cpp). No
 * shadow table is implemented since nothing compiled on Xbox ever reads one
 * back (Shadow_Get/Vs_Get are GL-renderer-only and excluded). */
void Shadow_Store(void* addr, float x, float y, float w, unsigned int value)
{
    (void)addr;
    (void)x;
    (void)y;
    (void)w;
    (void)value;
}

void VShadow_Store(void* addr, float x, float y, float z)
{
    (void)addr;
    (void)x;
    (void)y;
    (void)z;
}

void Shadow_Copy(void* dst, const void* src)
{
    (void)dst;
    (void)src;
}

/* GTE projection-center constants PsyX_GTE.cpp writes every RTPS/RTPT call,
 * but only inside the same `if (g_PsxUsePgxp)` block as the Shadow_Store
 * above - dead code on Xbox. Defaults copied from PsyX_GPU.cpp's own
 * initializers for consistency even though nothing reads them back. */
float g_PgxpGteOfx = 0.0f;
float g_PgxpGteOfy = 0.0f;
float g_PgxpGteH   = 1.0f;
int   g_PgxpUseUnquantizedDepth = 1; /* PsyX_GPU.cpp default; also dead on Xbox */

/* =============================================================================
 * Per-pixel flashlight (PsyX_GPU.cpp shader path, excluded). Every one of
 * these is WRITTEN by shared, always-compiled game code (bodyprog_80055028.c,
 * bodyprog_80040B74.c, world_draw.c, game_main.c) even on Xbox, but only ever
 * READ BACK by the GL shader setup that isn't built here - except
 * g_PsyX_UsePerPixelFlashlight and g_PsyX_FlashlightActive, which gate
 * whether bodyprog_80040B74.c's CLASSIC PSX flat-poly flashlight cone prims
 * get added to the OT at all:
 *   if (!g_PsyX_FlashlightActive) addPrim(...);   // the PSX-original beam geometry
 * g_PsyX_FlashlightActive is itself only set to 1 when
 * `g_PsyX_UsePerPixelFlashlight && isFlashlightOn` (bodyprog_80055028.c), so
 * pinning g_PsyX_UsePerPixelFlashlight = 0 (matches PC's own default AND
 * xbox_compat_globals.c's XboxConfig_ApplyOverrides, which forces
 * g_PcConfig.perPixelFlashlight = 0 for the same "no NV2A shader" reason)
 * guarantees g_PsyX_FlashlightActive stays 0 and the classic flashlight cone
 * always renders. Getting g_PsyX_UsePerPixelFlashlight wrong here would make
 * Harry's flashlight silently invisible in every dark room.
 * ========================================================================= */
int   g_PsyX_UsePerPixelFlashlight = 0;
int   g_PsyX_FlashlightActive      = 0;
int   g_PsyX_FlashlightStyle       = 0;
int   g_PsyX_FlashlightFpsMode     = 0;
int   g_PsyX_UseFlashlightShadows  = 0;
int   g_PsyX_ShadowsAllowed        = 0;
int   g_PsyX_NoShadowCast          = 0;
float g_PsyX_FlashlightSize        = 3.0f;
float g_PsyX_FlashlightIntensity   = 1.20f;
float g_PsyX_FlashlightPos[3]        = { 0.0f, 0.0f, 0.0f };
float g_PsyX_FlashlightShadowPos[3]  = { 0.0f, 0.0f, 0.0f };
float g_PsyX_FlashlightDir[3]        = { 0.0f, 0.0f, 1.0f };
float g_PsyX_FlashlightColor[3]      = { 1.0f, 1.0f, 1.0f };

/* =============================================================================
 * Whole-map far-projection + world/UI framing (PsyX_GPU.cpp / PsyX_render.cpp
 * globals, but WRITTEN AND CONSUMED by shared, always-compiled game code on
 * Xbox too - PsyX_GTE.cpp's far re-projection block at
 * `if (g_PsxWholeMapFar) {...}` is genuinely live here, not dead code, since
 * bodyprog_80040B74.c drives g_PsxWholeMapFar/g_PsxWholeMapChunkSz every
 * chunk-draw regardless of platform). All defaults copied verbatim from
 * their real PsyCross initializers so the shared code's read-modify-write
 * dance behaves identically to PC at boot (every one of them is written
 * before being read at runtime by the same shared code, so only the
 * compile-time starting value matters for the very first frame).
 * ========================================================================= */
int   g_PsxWholeMapFar     = 0;
int   g_PsxWholeMapLastSz  = 0;
int   g_PsxWholeMapChunkSz = 0;

/* Fixed-angle camera FIX_ANG top-clip correction (PsyX_render.cpp comment:
 * "defaults are the PSX +-112/+-96" / "g_PsxWorldVShift = 20.0f"). These are
 * permanent tuned constants (no g_PcConfig field overrides them - only the
 * PC-only debug console's live `vshift`/`bartop`/`barbot` commands can, and
 * those are excluded on Xbox), so the compiled-in PC default IS the shipped
 * behavior on Xbox too, not a placeholder. */
float g_PsxWorldVShift    = 20.0f;
int   g_PsxFixedCamActive = 0; /* driven live by vc_main.c */
int   g_PsxCutsceneActive = 0; /* driven live by game_main.c */
int   g_PsxUIOrthoPass    = 0; /* driven live by game_main.c */
int   g_PsxMsgVShift      = 0; /* PC default: no message-box up-shift */
int   g_PsxBarOuter       = 112; /* PSX-faithful letterbox bar Y (cutscene_border.c) */
int   g_PsxBarInner       = 96;

/* =============================================================================
 * Hi-res TIM override / chunk-pool virtual-slot texture system
 * (hires_override.c, excluded - decodes loose PNG/TIM replacements to GL
 * textures). fsqueue_3.c's calls into this are all under SH_PC_PORT and
 * check the return value: "0 = registered" everywhere per hires_override.h,
 * so a nonzero "failure" return makes every call site fall back to the
 * disc-native TIM/VRAM path it already has - exactly "hi-res override is
 * off", not a crash or a silently-blank texture. None of this affects
 * Xbox's actual rendering either way: gpu_xbox.c/psx_vram.c sample real PSX
 * VRAM directly and never consult this lookup table (that consumption only
 * happens in the excluded GL AddSplit hook in PsyX_render.cpp).
 * ========================================================================= */
int HiresOverride_PoolSlotRegister(int slotId, const unsigned char* data, unsigned int size, int nativePixelW, int nativePixelH)
{
    (void)slotId;
    (void)data;
    (void)size;
    (void)nativePixelW;
    (void)nativePixelH;
    return -1;
}

void HiresOverride_PoolSlotsReset(void)
{
}

void HiresOverride_CharaPoolSlotReset(int slotId)
{
    (void)slotId;
}

int HiresOverride_PoolSlotRegisterRGBA(int slotId, int row, const unsigned char* rgba, int w, int h, int nativePixelW, int nativePixelH)
{
    (void)slotId;
    (void)row;
    (void)rgba;
    (void)w;
    (void)h;
    (void)nativePixelW;
    (void)nativePixelH;
    return -1;
}

int HiresOverride_PoolSlotRegisterRGBAKeyed(int slotId, int row, const unsigned char* rgba, int w, int h,
                                            int nativePixelW, int nativePixelH, unsigned long long contentHash)
{
    (void)slotId;
    (void)row;
    (void)rgba;
    (void)w;
    (void)h;
    (void)nativePixelW;
    (void)nativePixelH;
    (void)contentHash;
    return -1;
}

int HiresOverride_PoolSlotLoosePngRow(int slotId, int row, const unsigned char* data, unsigned int size, int nativePixelW, int nativePixelH)
{
    (void)slotId;
    (void)row;
    (void)data;
    (void)size;
    (void)nativePixelW;
    (void)nativePixelH;
    return -1;
}

int HiresOverride_RegisterLoosePngRow(const char* label, const unsigned char* data, unsigned int size,
                                      int targetVramX, int targetVramY, int targetVramW, int targetVramH,
                                      int targetClutX, int targetClutY, int originalBitDepth)
{
    (void)label;
    (void)data;
    (void)size;
    (void)targetVramX;
    (void)targetVramY;
    (void)targetVramW;
    (void)targetVramH;
    (void)targetClutX;
    (void)targetClutY;
    (void)originalBitDepth;
    return -1;
}

int HiresOverride_RegisterLoosePngAllRows(const char* label, const unsigned char* data, unsigned int size,
                                          int targetVramX, int targetVramY, int targetVramW, int targetVramH,
                                          int targetClutX, int targetClutY, int rows, int originalBitDepth)
{
    (void)label;
    (void)data;
    (void)size;
    (void)targetVramX;
    (void)targetVramY;
    (void)targetVramW;
    (void)targetVramH;
    (void)targetClutX;
    (void)targetClutY;
    (void)rows;
    (void)originalBitDepth;
    return -1;
}

int HiresOverride_RegisterRGBAKeyed(const char* label, const unsigned char* rgba, int w, int h,
                                    int targetVramX, int targetVramY, int targetVramW, int targetVramH,
                                    int targetClutX, int targetClutY, int originalBitDepth, unsigned long long contentHash)
{
    (void)label;
    (void)rgba;
    (void)w;
    (void)h;
    (void)targetVramX;
    (void)targetVramY;
    (void)targetVramW;
    (void)targetVramH;
    (void)targetClutX;
    (void)targetClutY;
    (void)originalBitDepth;
    (void)contentHash;
    return -1;
}

void HiresOverride_InvalidateVramRect(int x, int y, int w, int h)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

int HiresOverride_PackBudgetExceeded(void)
{
    return 0; /* budget never exceeded: no pack textures are ever composed */
}

/* =============================================================================
 * DuckStation texture-pack scanning (tex_pack.c, excluded - filesystem PNG
 * pack scan). TexPack_HasEntries() = 0 is the master "off" switch: every
 * fsqueue_3.c call site checks it BEFORE calling TexPack_Compose, so Compose
 * itself is dead code on Xbox in practice; it still returns NULL ("no pack
 * entry matched", per tex_pack.h) for safety if that ever changes.
 * ========================================================================= */
int TexPack_HasEntries(void)
{
    return 0;
}

const unsigned char* TexPack_Compose(const unsigned char* pixels, int w16, int h,
                                     const unsigned short* clut, int clutCount, int bpp,
                                     int* outW, int* outH)
{
    (void)pixels;
    (void)w16;
    (void)h;
    (void)clut;
    (void)clutCount;
    (void)bpp;
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    return NULL;
}

unsigned long long TexPack_LastComposeHash(void)
{
    return 0; /* "0 means no stable key (always re-upload)" per tex_pack.h */
}

/* =============================================================================
 * stb_image (only linked for its STB_IMAGE_IMPLEMENTATION in hires_override.c,
 * excluded on Xbox). pc_decals.c is NOT excluded and still declares/calls the
 * plain stb_image.h prototypes for its optional gamedata/decal.png loose
 * bullet-decal texture; on decode failure it already logs
 * stbi_failure_reason() and disables decals gracefully (s_texMissing = 1),
 * so "PNG decoding unavailable" is a fully supported, cosmetic-only outcome.
 * ========================================================================= */
unsigned char* stbi_load_from_memory(unsigned char const* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels)
{
    (void)buffer;
    (void)len;
    (void)desired_channels;
    if (x) *x = 0;
    if (y) *y = 0;
    if (channels_in_file) *channels_in_file = 0;
    return NULL;
}

const char* stbi_failure_reason(void)
{
    return "stb_image not built on Xbox (HD texture packs / loose PNG decals are disabled)";
}

/* =============================================================================
 * CD-XA voice/cutscene volume + pause + drain query. xa_xbox.c already
 * implements the bulk of the XaPlayer_* surface (Play/Stop/Update/SetVolume)
 * for real hardware audio; these three were the ones it didn't cover.
 * ========================================================================= */

/* g_PcXaVolume / g_PcFmvVolume: full volume, matching xa_player.c's and
 * fmv_player.cpp's own `= 1.0f` initializers - a 0.0f default would leave
 * voice lines and FMV audio inaudible even though hardware audio is wired up
 * and working (see MEMORY: DirectSound verified 2026-06-23). */
float g_PcXaVolume  = 1.0f;
float g_PcFmvVolume = 1.0f;

/* XaPlayer_SetMasterVolume: real semantics kept (clamp + store), matching
 * xa_player.c, so the in-game Options volume slider (routed through
 * pc_config.c's PcConfig_ApplyXaVolume, compiled on Xbox) has somewhere
 * correct to write; xa_xbox.c's mixer doesn't read g_PcXaVolume back yet
 * (it has its own per-source volQ7L/R path from XaPlayer_SetVolume), so
 * wiring g_PcXaVolume into Xa_XboxMixInto's gain stage is a real, tracked
 * follow-up, not something masked by this stub. */
void XaPlayer_SetMasterVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    g_PcXaVolume = v;
}

/* XaPlayer_SetPauseHold: pauses/resumes the live XA voice while a PC debug
 * console freezes the game. g_PcConsoleInputActive is hard-pinned to 0 in
 * xbox_compat_globals.c (Xbox has no debug console), so game_main.c always
 * calls this with hold=0 - a no-op is exactly correct today. Flagged as a
 * follow-up only if Xbox ever grows its own pause state that should freeze
 * XA playback (xa_xbox.c would need an exposed pause entry point first).
 *
 * Xa_IsVoiceAudioDraining: NOT WIRED TO REAL STATE - this is the one
 * genuinely uncertain default in this file. On PC it reports whether the
 * live OpenAL voice source is still producing audio, used by
 * map_msg_display.c to hold a subtitle page's AUTO-advance timer open past
 * its authored duration while the voice line is still playing. xa_xbox.c's
 * real playback state (s_xa.isPlaying / remainingSectors) is file-local and
 * not exported, and exporting it would mean editing xa_xbox.c (out of this
 * stub file's scope). Returning 0 ("never draining") makes pcVoiceHold in
 * map_msg_display.c always false, so Xbox subtitles simply advance on the
 * ORIGINAL PSX-authored page timer with no PC-style drain gating - the
 * same pacing the PSX release shipped with, not a bug, but a real feature
 * (voice-overrun protection added for PC's near-zero load latency) sitting
 * unused. Wiring this up for real would need xa_xbox.c to expose an
 * `Xa_XboxIsPlaying(void)`-style getter. */
void XaPlayer_SetPauseHold(int hold)
{
    (void)hold;
}

/* Xa_IsVoiceAudioDraining: real implementation now lives in xa_xbox.c (reads
 * s_xa.isPlaying, which stays set until the ring fully drains). The old
 * return-0 stub let voiced subtitle pages auto-advance mid-line. */

/* =============================================================================
 * Misc PC-only globals with no HAL of their own.
 * ========================================================================= */

/* g_ShOverlayToastLine: NULL function pointer, matching main_pc.c's own
 * `= NULL` initializer; sh_log.h's SH_DBG_ECHO macro already null-checks it
 * before calling, so this is the documented "no overlay toast" state, not a
 * missing wire-up (Xbox is blind to the screen anyway - see
 * testing-workflow.md - so there is nowhere to draw a toast). */
void (*g_ShOverlayToastLine)(const char* line) = NULL;

/* g_PcUnlimitedEnemies: off, matching main_pc.c's `= 0` (a debug/randomizer
 * toggle, not something that should silently be on by default). */
int g_PcUnlimitedEnemies = 0;

/* g_PcWidescreenMode: 0 = Pillarbox/PSX-faithful. Technically inert on Xbox
 * today - every read site is `g_PcHorPlusEnabled && g_PcWidescreenMode==N`,
 * and g_PcHorPlusEnabled is hard-pinned to 0 in xbox_compat_globals.c - but
 * 0 is also the exact value XboxConfig_ApplyOverrides forces
 * g_PcConfig.widescreenMode to ("PC 1 (Hor+): pillarbox instead"), so this
 * stays consistent with that intent if a future Xbox Hor+ mode ever reads it. */
int g_PcWidescreenMode = 0;

/* g_cfg_bilinearFiltering/psxDither/postProcess/tonemap: GL post-process
 * shader toggles (PsyX_render.cpp), read on Xbox only by options.c's menu
 * display/cycle logic - gpu_nv2a.c has its own fixed NEAREST-filtered,
 * no-post-process NV2A pipeline and never consults these. Defaults copied
 * verbatim from PsyX_render.cpp's own initializers (psxDither defaults ON,
 * matching the PSX-authentic dithered look Xbox's own renderer already aims
 * for; the others default off). */
int g_cfg_bilinearFiltering = 0;
int g_cfg_psxDither         = 1;
int g_cfg_postProcess       = 0;
int g_cfg_tonemap           = 0;

/* =============================================================================
 * Positional (azimuth) 3D audio hooks (PsyX_SPUAL.cpp - OpenAL-only). Xbox
 * has its own software SPU (audio_xbox.c, 24 voices, real hardware output via
 * dsound_xbox.c) that has no positional-panning stage yet - it mixes each
 * voice's L/R volume from SpuSetVoiceAttr only. No-op for now; wiring these
 * into audio_xbox.c's mixer (deriving a stereo pan from azimuthQ12 per
 * voice) would give real positional/5.1 audio and is a known, tracked
 * follow-up (see xbox-roadmap.md's audio v2 item), not something this stub
 * silently forecloses.
 * ========================================================================= */
void PsyX_SPUAL_SetNextKeyOnAzimuth(int azimuthQ12)
{
    (void)azimuthQ12;
}

void PsyX_SPUAL_ClearNextKeyOnAzimuth(void)
{
}

void PsyX_SPUAL_SetVoiceAzimuth(int voiceIdx, int azimuthQ12)
{
    (void)voiceIdx;
    (void)azimuthQ12;
}

/* =============================================================================
 * PcPort_GetGameDiscPath (main_pc.c, excluded - launcher disc-image
 * selection/region autodetect). Its only reachable Xbox caller is
 * lang_text.c's ReadDiscFile (fan-translation/PAL-patch text re-read from a
 * PC-style disc image path), which already null-checks the path and returns
 * NULL on failure; main_xbox.c fixes the region to USA directly via
 * Fs_InitFileTableForRegion(Region_USA) without going through this launcher
 * path at all, so NULL here just means "no fan-translation disc rescan on
 * Xbox", falling back to the compiled-in US text exactly as intended.
 * ========================================================================= */
const char* PcPort_GetGameDiscPath(void)
{
    return NULL;
}
