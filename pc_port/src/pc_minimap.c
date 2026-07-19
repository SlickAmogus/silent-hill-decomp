/* Config-only PC minimap overlay (top-left).
 *
 * Drawn with PSX PRIMITIVES into the game's own display list — the same path
 * Pc_HealFlashUpdate and every other 2D HUD element uses.
 *
 * The previous implementation drew with raw OpenGL from PsyX's post-capture hook.
 * That was abandoned after it repeatedly killed the NVIDIA driver's async worker
 * thread: borrowing PsyX's live GL context meant any piece of state we failed to
 * restore (framebuffer binding, blend equation, scissor box — all found and fixed
 * in turn) corrupted the renderer. The final run settled it: the map file never
 * loaded (0 reads) and it still crashed, so the fault was purely in our GL
 * drawing. Emitting ordinary prims removes that entire class of failure.
 *
 * Harry's position comes from the game's own world->map transform (func_80067914)
 * polled in query mode, so the marker can't drift from the real paper map.
 *
 * The area map IMAGE is not drawn yet — a textured prim needs the map in VRAM,
 * which is the residency problem that sent us to GL in the first place. The route
 * now is to register it through the hires-override system (the same mechanism
 * texture packs use to back a prim with a PC texture). Until then the panel shows
 * a north-up grid that scrolls with Harry.
 *
 * Off by default: g_PcConfig.minimap == 0 early-returns before any work. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_config.h"
#include "sh_log.h"

/* Set while polling func_80067914 for coords only (suppresses its arrow draw). */
int g_PcMapQueryOnly = 0;

/* Panel geometry in the 2D screen space (origin = screen centre, 4:3 is
 * x -160..+160, y -120..+120). Top-left corner, inset a little. */
#define MM_SIZE   72
#define MM_X0     (-(SCREEN_WIDTH / 2) + 10)
#define MM_Y0     (-(SCREEN_HEIGHT / 2) + 10)
#define MM_X1     (MM_X0 + MM_SIZE)
#define MM_Y1     (MM_Y0 + MM_SIZE)

#define MM_GRID_LINES 5   /* per axis */
#define MM_CELL       40  /* world units between grid lines (CHUNK_CELL_SIZE) */
#define MM_VIEW       100 /* world half-extent shown around Harry */

/* Map-cell extents the paper map spans; used to place Harry on the panel. */
#define MM_HALF_X 160
#define MM_HALF_Z 240

static int mm_clamp(int v, int lo, int hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

void Pc_MinimapUpdate(void)
{
    static TILE     s_panel[2];
    static TILE     s_grid[2][MM_GRID_LINES * 2];
    static POLY_F3  s_mark[2];
    static DR_TPAGE s_tp[2];
    int      buf, i, n;
    s32      packed;
    s32      px, py;
    q3_12    ang;
    s32      ca, sa;
    GsOT_TAG* ot;

    if (!g_PcConfig.minimap) return;
    if (g_GameWork.gameState != GameState_InGame ||
        g_SysWork.sysState   != SysState_Gameplay) return;

    buf = g_ActiveBufferIdx;
    ot  = &g_OtTags0[buf][4];

    /* --- Harry's cell on the paper map (query mode: no arrow drawn) --- */
    g_PcMapQueryOnly = 1;
    packed = func_80067914((s32)g_SavegamePtr->paperMapIdx, 0, 0, (u16)Q12(1.0f));
    g_PcMapQueryOnly = 0;

    if (packed != 0)
    {
        s32 mx = (s32)(s16)(packed & 0xFFFF);
        s32 mz = (s32)(s16)((packed >> 16) & 0xFFFF);
        px = MM_X0 + (((mx + MM_HALF_X) * MM_SIZE) / (2 * MM_HALF_X));
        py = MM_Y0 + (((mz + MM_HALF_Z) * MM_SIZE) / (2 * MM_HALF_Z));
    }
    else
    {
        px = MM_X0 + MM_SIZE / 2;   /* unmapped area: sit in the middle */
        py = MM_Y0 + MM_SIZE / 2;
    }
    px = mm_clamp(px, MM_X0 + 4, MM_X1 - 4);
    py = mm_clamp(py, MM_Y0 + 4, MM_Y1 - 4);

    /* --- marker: small triangle pointing along Harry's heading (screen +Y is down,
     *     so "north" is -Y at angle 0) --- */
    ang = g_SysWork.playerWork.player.rotation.vy;
    ca  = Math_Cos(ang);
    sa  = Math_Sin(ang);
    {
        static const s32 lx[3] = {  0, -3,  3 };
        static const s32 ly[3] = { -5,  3,  3 };
        s32 vx[3], vy[3];
        for (i = 0; i < 3; i++)
        {
            vx[i] = px + ((lx[i] * ca - ly[i] * sa) >> 12);
            vy[i] = py + ((lx[i] * sa + ly[i] * ca) >> 12);
        }
        setPolyF3(&s_mark[buf]);
        setRGB0(&s_mark[buf], 255, 216, 48);
        setXY3(&s_mark[buf], vx[0], vy[0], vx[1], vy[1], vx[2], vy[2]);
    }

    /* --- north-up grid, scrolling with Harry (stands in for the map image) --- */
    {
        s32 hx = g_SysWork.playerWork.player.position.vx >> 12;
        s32 hz = g_SysWork.playerWork.player.position.vz >> 12;
        n = 0;
        for (i = 0; i < MM_GRID_LINES; i++)
        {
            /* line i sits at the i-th cell boundary within the view window */
            s32 wx = ((hx - MM_VIEW) / MM_CELL + i) * MM_CELL;
            s32 wz = ((hz - MM_VIEW) / MM_CELL + i) * MM_CELL;
            s32 gx = MM_X0 + (((wx - hx) + MM_VIEW) * MM_SIZE) / (2 * MM_VIEW);
            s32 gy = MM_Y0 + (((wz - hz) + MM_VIEW) * MM_SIZE) / (2 * MM_VIEW);

            if (gx > MM_X0 && gx < MM_X1)
            {
                setTile(&s_grid[buf][n]);
                setRGB0(&s_grid[buf][n], 72, 88, 104);
                setWH(&s_grid[buf][n], 1, MM_SIZE);
                setXY0(&s_grid[buf][n], gx, MM_Y0);
                n++;
            }
            if (gy > MM_Y0 && gy < MM_Y1)
            {
                setTile(&s_grid[buf][n]);
                setRGB0(&s_grid[buf][n], 72, 88, 104);
                setWH(&s_grid[buf][n], MM_SIZE, 1);
                setXY0(&s_grid[buf][n], MM_X0, gy);
                n++;
            }
        }
    }

    /* --- panel background --- */
    setTile(&s_panel[buf]);
    setSemiTrans(&s_panel[buf], 1);
    setRGB0(&s_panel[buf], 16, 20, 30);
    setWH(&s_panel[buf], MM_SIZE, MM_SIZE);
    setXY0(&s_panel[buf], MM_X0, MM_Y0);

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 0, 0, 0)); /* abr=0 = 50% blend */

    /* AddPrim pushes to the head, so the LAST one added is processed FIRST:
     * add front-to-back (marker, grid, panel) and the tpage last. */
    AddPrim(ot, &s_mark[buf]);
    for (i = 0; i < n; i++)
        AddPrim(ot, &s_grid[buf][i]);
    AddPrim(ot, &s_panel[buf]);
    AddPrim(ot, &s_tp[buf]);
}

/* The GL overlay is gone; keep the symbol so dbg_overlay.c's call is harmless. */
void Pc_MinimapDraw(void) {}
