#include "game.h"
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/math/math.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#include <PsyX/PsyX_public.h>
/* Inventory item-preview aspect: 0 = PSX-faithful (raw 4:3-display look,
 * vertically squished in interlaced 448), 1 = square (true proportions, default).
 * Toggled by `invaspect`. g_PcInvAspectPct fine-tunes the vertical scale as a
 * percent of the geometric square factor (100 = exactly square); `invscale`. */
int g_PcInvAspectSquare = 1;
int g_PcInvAspectPct    = 125;
/* Slight Y placement match vs Duckstation (view-Y units; + = down). Carousel a
 * touch down, equipped weapon up. Tunable: invcary / inveqy. */
int g_PcInvCarouselYOff = 50;
int g_PcInvEquipYOff    = -50;
/* Off-center carousel dimming: PER-SLOT darken %. The carousel curves in Z, one
 * "slot" per Math_Sin step; darken by this % times the slot distance from center
 * (slot 1 = 30%, slot 2 = 60%, ...). 0 = no dim. Tunable: invdim. */
int g_PcInvDimStrength  = 30;
#endif

GsCOORD2PARAM D_800C3928;
s8 g_Player_WeaponAttack;
s8 __pad_bss_800C3951[3];
s32 D_800C3954;
s32 D_800C3958;
s32 D_800C395C;

// In USA this function is called at the start of `MainLoop`.
// NTSC-J splits this function into 3, and moves the calls into the `HP_SAFE1` overlay.
#if VERSION_IS(USA)
void ItemScreen_TmdGsFCallInit(void) // 0x8004BB10
{
    // Based on `libgs.h` `jt_init4` function?
    // Only seems to affect TMD rendering which is exclusive to item screen.

    // Gouraud triangle.
    GsFCALL4.g3[GsDivMODE_NDIV][GsLMODE_FOG]  = GsTMDfastG3LFG;
    // Textured gouraud triangle.
    GsFCALL4.tg3[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastTG3LFG;
    // Gouraud quad.
    GsFCALL4.g4[GsDivMODE_NDIV][GsLMODE_FOG]  = GsTMDfastG4LFG;
    // Textured gouraud quad.
    GsFCALL4.tg4[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastTG4LFG;
}
#elif VERSION_REGION_IS(NTSCJ)
bool ItemScreen_TmdGsFCallInitTG3(void) // JPN0 0x8004CB54
{
    GsFCALL4.tg3[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastTG3LFG;
    return false;
}

void ItemScreen_TmdGsFCallInitG3G4(void) // JPN0 0x8004CB6C
{
    GsFCALL4.g3[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastG3LFG;
    GsFCALL4.g4[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastG4LFG;
}

void ItemScreen_TmdGsFCallInitTG4(void) // JPN0 0x8004CB90
{
    GsFCALL4.tg4[GsDivMODE_NDIV][GsLMODE_FOG] = GsTMDfastTG4LFG;
}
#endif

void ItemScreen_CamSet(VbRVIEW* view, GsCOORDINATE2* coord, SVECTOR3* vec, s32 arg3) // 0x8004BB4C
{
    view->vr.vz = 10;
    view->vp.vx = 0;
    view->vp.vy = 0;
    view->vp.vz = 0;
    view->vr.vx = 0;
    view->vr.vy = 0;

    view->rz = 0;

    view->super       = coord;
    coord->coord.t[2] = -0x2800;
    coord->super      = NULL;
    coord->coord.t[0] = 0;
    coord->coord.t[1] = 0;

    vec->vx = 0;
    vec->vy = 0;
    vec->vz = 0;

    D_800C3928.scale.vz  = Q12(1.0f);
    D_800C3928.scale.vy  = Q12(1.0f);
    D_800C3928.scale.vx  = Q12(1.0f);
    D_800C3928.rotate.vz = 0;
    D_800C3928.rotate.vy = 0;
    D_800C3928.rotate.vx = 0;
    D_800C3928.trans.vz  = 0;
    D_800C3928.trans.vy  = 0;
    D_800C3928.trans.vx  = 0;

    coord->param = &D_800C3928;

    ItemScreen_ItemRotate((SVECTOR*)vec, coord);
    vbSetRefView(view);
}

void func_8004BBF4(VbRVIEW* arg0, GsCOORDINATE2* arg1, SVECTOR* arg2) // 0x8004BBF4
{
    u16     vx;
    VECTOR  vec;
    SVECTOR sVec;

    vx  = arg2->vx;
    arg2->vx = 0;

    ItemScreen_ItemRotate(arg2, arg1);

    arg2->vx = vx;

    ItemScreen_ItemRotate(arg2, arg1);

    sVec.vx = 0;
    sVec.vy = 0;
    sVec.vz = 0;

    gte_ApplyMatrix(&arg1->coord, &sVec, &vec);
    vbSetRefView(arg0);
}

void GameFs_TmdDataAlloc(s32* buf) // 0x8004BCBC
{
    GsMapModelingData((unsigned long*)&buf[1]);
}

void ItemScreen_ItemRotate(SVECTOR* arg0, GsCOORDINATE2* arg1) // 0x8004BCDC
{
    MATRIX mat;

    mat.t[0] = arg1->coord.t[0];
    mat.t[1] = arg1->coord.t[1];
    mat.t[2] = arg1->coord.t[2];

    Math_RotMatrixZxyNegGte(arg0, &mat);

    arg1->coord = mat;

    ScaleMatrix(&arg1->coord, &arg1->param->scale);

    arg1->flg = false;
}

void func_8004BD74(s32 displayItemIdx, GsDOBJ2* arg1, s32 arg2)  // 0x8004BD74
{
    MATRIX viewMat;
    MATRIX localToScreenMat;
    MATRIX worldMat;
    s32 i;
    s32 j;

#ifdef SH_PC_PORT
    /* Silently skip degenerate cases that would crash on x86-64.
     * scale.vx==0 was the unequip-anim divide-by-zero (item_screens_3.c:2812)
     * that used to kill the process with SIGFPE. */
    if (arg1->coord2 == NULL) return;
    if (g_Items_Transforms[displayItemIdx].scale.vx == 0) return;
    if (arg1->tmd == NULL) return;
    /* tmd non-NULL but MALFORMED: a valid loadableItemIdx (< nobj) can still
     * resolve to a garbage TMD object when the inventory holds an item not in
     * this map's item TMD (hit on the gas tank with give-all-weapons). Absurd
     * vert/prim counts or NULL data pointers make GsSortObject4J over-read ->
     * wild GTE vertex read -> AV crash. Skip the draw instead of crashing. */
    {
        struct TMD_STRUCT* _t = (struct TMD_STRUCT*)arg1->tmd;
        int _bad = (_t->vertop == NULL || _t->primtop == NULL ||
                    _t->vern == 0 || _t->vern > 0x2000 ||
                    _t->primn == 0 || _t->primn > 0x2000);
        if (_bad) {
            return;
        }
    }
#endif
    Vw_CoordToWorldAndViewMatrices(arg1->coord2, &worldMat, &viewMat);

    localToScreenMat = viewMat;

    for (i = 0; i < 3; i++)
    {
        for (j = 0; j < 3; j++)
        {
            viewMat.m[i][j] = Q12(viewMat.m[i][j]) / g_Items_Transforms[displayItemIdx].scale.vx;
        }
    }

    if (arg2 != 3 && displayItemIdx < 7)
    {
        for (i = 0; i < 3; i++)
        {
            for (j = 0; j < 3; j++)
            {
                viewMat.m[i][j] -= Q12_MULT(viewMat.m[i][j], Math_Sin((g_Items_Coords[displayItemIdx].coord.t[2] + 0x400) >> 2));
            }
        }
    }

    GsSetLightMatrix(&viewMat);
#ifdef SH_PC_PORT
    {
        /* Correct for the horizontal stretch introduced when PsyCross maps
         * PSX pixel coords (320×240, 4:3) onto a wider-than-4:3 viewport.
         * Scale row-0 of the ls matrix (view-space X axis) and the X
         * translation by psxAspect/screenAspect so items appear with the
         * same proportions they had on the original 4:3 display.
         * The 2D background is unaffected — it goes through a separate path.
         *
         * Skip entirely when the inventory screen is pillarboxed: PsyCross
         * then renders the 4:3 ortho into a centered 4:3 viewport, so there
         * is NO horizontal stretch to undo — applying it here would squish
         * the item preview. The inventory is a 2D screen (g_PcHorPlusEnabled
         * forced to 0 by game_main.c), so pillarbox is active whenever
         * g_PcMenuPillarbox is set. */
        extern int g_PcHorPlusEnabled;
        extern int g_PcMenuPillarbox;
        int _pillarboxed = (!g_PcHorPlusEnabled && g_PcMenuPillarbox);
        int _sw, _sh;
        PsyX_GetScreenSize(&_sw, &_sh);
        float _dispAspect = (float)_sw / (float)_sh;
        float _psxAspect = 320.0f / 240.0f;
        float _corrApplied = 1.0f;
        if (!_pillarboxed && _dispAspect > _psxAspect) {
            float _corr = _psxAspect / _dispAspect;
            int _j;
            for (_j = 0; _j < 3; _j++)
                localToScreenMat.m[0][_j] = (s16)((float)localToScreenMat.m[0][_j] * _corr);
            localToScreenMat.t[0] = (s32)((float)localToScreenMat.t[0] * _corr);
            _corrApplied = _corr;
        }
        (void)_corrApplied;

        /* Square mode (toggle `invaspect 1`): item models project symmetrically
         * into the gsScreenW x gsScreenH framebuffer, which is then shown at 4:3,
         * so a square model comes out at aspect (4/3)/(gsW/gsH) — ~1.87x too wide
         * in interlaced 448. Scale the Y size axis by that factor (x invscale%)
         * to restore true (square-pixel) proportions. Independent of window/
         * pillarbox since the horizontal path already normalizes to a 4:3 display. */
        if (g_PcInvAspectSquare && g_GameWork.gsScreenWidth > 0) {
            /* Scale only the size rows (m[1]) — NOT the Y translation (t[1]):
             * the model must get taller IN PLACE. Scaling t[1] also moved
             * off-center items (the equipped weapon, placed high) further up. */
            float _fY = (4.0f / 3.0f) * ((float)g_GameWork.gsScreenHeight / (float)g_GameWork.gsScreenWidth)
                        * ((float)g_PcInvAspectPct / 100.0f);
            int _j;
            for (_j = 0; _j < 3; _j++)
                localToScreenMat.m[1][_j] = (s16)((float)localToScreenMat.m[1][_j] * _fY);
        }

        /* Slight Y placement match vs Duckstation: carousel items (idx < 7) a
         * touch down, equipped weapon (idx 7) a touch up. Translation only. */
        if (displayItemIdx == 7)
            localToScreenMat.t[1] += g_PcInvEquipYOff;
        else if (displayItemIdx < 7)
            localToScreenMat.t[1] += g_PcInvCarouselYOff;
    }
#endif
    GsSetLsMatrix(&localToScreenMat);

#ifdef SH_PC_PORT
    /* Off-center carousel dimming (the PSX depth-cue the no-light TMD render
     * path drops). The carousel curves in Z: the centered item sits at
     * t[2]=Q8(-4) (nearest), side items further back. Darken the whole model in
     * proportion to how far past center it sits, up to g_PcInvDimStrength% at a
     * full Q8(1.0) of extra depth. Equipped (idx 7) and any non-carousel index
     * stay full bright. g_PcItemDimNum (256 = full) is read by the GsTMDfast*
     * renderers in libgs_stub.c; reset after the draw so nothing else dims. */
    {
        extern int g_PcItemDimNum;
        g_PcItemDimNum = 256;
        if (displayItemIdx < 7 && g_PcInvDimStrength > 0) {
            /* depth past center = t[2]+Q8(4) = |Math_Sin(slot*256)| (Q12):
             * slot1~1567, slot2~2896, slot3~3784. Quantize to slot distance and
             * darken g_PcInvDimStrength% per slot. */
            s32 _depth = g_Items_Coords[displayItemIdx].coord.t[2] + Q8(4.0f);
            s32 _slot  = (_depth < 800) ? 0 : (_depth < 2200) ? 1 : (_depth < 3300) ? 2 : 3;
            s32 _dim   = _slot * g_PcInvDimStrength;
            if (_dim > 100) _dim = 100;
            g_PcItemDimNum = 256 - (256 * _dim) / 100;
        }
    }
#endif

#ifdef SH_PC_PORT
    /* Force the GL depth test on for this item ONLY when the world is frozen
     * (status menu etc.). During active gameplay the in-world pickup preview shares
     * OT0 with live world geometry, so forcing depth there occluded the preview and
     * dropped world faces. g_forceItemDepth (PsyX_render.cpp) is consumed by the
     * shared GsDrawOt(OT0) and cleared after it draws. */
    { extern int g_forceItemDepth;
      g_forceItemDepth = (g_SysWork.sysState != SysState_Gameplay) ? 1 : 0; }
#endif
    if (arg2 == 2)
    {
        GsClearOt(0, 0, &g_OrderingTable1[g_ActiveBufferIdx]);
        GsSortOt(&g_OrderingTable1[g_ActiveBufferIdx], &g_OrderingTable0[g_ActiveBufferIdx]);
        GsSortObject4J(arg1, &g_OrderingTable1[g_ActiveBufferIdx], 1, (u32*)PSX_SCRATCH);
    }
    else
    {
        GsSortObject4J(arg1, &g_OrderingTable0[g_ActiveBufferIdx], 1, (u32*)PSX_SCRATCH);
    }

#ifdef SH_PC_PORT
    { extern int g_PcItemDimNum; g_PcItemDimNum = 256; }
#endif
}

/** Removing this causes the models of items to appear farther from the camera.
 * `Gfx_Items_ViewPointAdjustment`?
 */
void func_8004BFE8(void) // 0x8004BFE8
{
    // Save constant rotation matrix in stack.
    PushMatrix();

    // Read distance from viewpoint to screen.
    D_800C3954 = ReadGeomScreen();

    // Read GTE offset value.
    ReadGeomOffset(&D_800C3958, &D_800C395C);

    // Set distance between projection plane and viewpoint. Results in FOV change.
    GsSetProjection(1000);

    g_Player_WeaponAttack = g_SysWork.playerCombat.weaponAttack;
}

/** Possible failsafe?
 * Used when exiting the inventory screen or going into options and map menus
 * through the inventory.
 *
 * Removing it doesn't affect the game.
 *
 * @note From a member of the PS Decomp Discord:
 * "[The function] essentially just tries to restore projection/offset settings
 * and the transform matrix to what they were in the main game, but I'm guessing
 * the game sets them at the start of every frame anyway, so it doesn't really
 * achieve anything."
 */
void func_8004C040(void) // 0x8004C040
{
    // Reset constant rotation matrix from stack.
    PopMatrix();

    GsSetProjection(D_800C3954);
    SetGeomOffset(D_800C3958, D_800C395C);
}
