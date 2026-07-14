/* PC port: mouse control for the inventory screen.
 *
 * Same contract as the other menu mouse layers (options, save/load, title):
 * hit-test the game's OWN authored layout, then INJECT the controller bits the
 * stock code already reads. No scroll/SFX/streaming logic is duplicated here —
 * Inventory_Logic does all of it, we only synthesize the input.
 *
 * Hooked from the top of Inventory_Logic, BEFORE Inventory_DirectionalInputSet():
 * that function derives every movement flag (g_Inventory_IsLeftClicked, ...) from
 * g_Controller0, and when the analog stick is centred — always true for a mouse
 * user — it reads the DIGITAL ControllerFlag_LStick* bits. So injecting those
 * bits is indistinguishable from a real stick press.
 *
 * Interaction model:
 *   - hover a region (item carousel / equipped item / Option / Exit / Map)
 *     snaps the selection to it
 *   - click the focused item          -> enter (opens its command submenu)
 *   - click an off-centre carousel item -> scrolls the carousel to it
 *   - click a command row             -> enter (executes it)
 *   - right-click / click outside an open submenu -> cancel
 *   - wheel over the carousel         -> scroll one item
 */
#include "game.h"
#include "inline_no_dmpsx.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/items.h"
#include "bodyprog/sys/joy.h"
#include "pc_config.h"
#include "pc_inventory_mouse.h"
#include "pc_mouse_cursor.h"

#include <PsyX/PsyX_public.h>

/* Text-authoring reference (Gfx_StringSetPosition's origin). Pc_MouseCursor_UiPos
 * reports in that space; every table below is in the game's centre-origin prim
 * space, so subtract the reference to convert. Verified against the game's own
 * draws: the command label at string (222,-34) lands on the highlight bar at
 * prim y -146, and -34 - 112 = -146. */
#define INV_ORIGIN_X 160
#define INV_ORIGIN_Y 112

/* Selection hit boxes = the game's own SelectionOuline_ConerLines[] brackets
 * (item_screens_3.c), the ones it draws around each region. Indexed by
 * e_InventorySelectionId. */
typedef struct
{
    s16 x0, y0, x1, y1;
} s_InvBox;

static const s_InvBox INV_BOX_ITEM     = { -34,  -56,  34,  80 };
static const s_InvBox INV_BOX_EQUIPPED = { -50, -204,  50, -52 };
static const s_InvBox INV_BOX_SETTINGS = { -146, 181, -46, 213 };
static const s_InvBox INV_BOX_EXIT     = { -50,  181,  50, 213 };
static const s_InvBox INV_BOX_MAP      = {  46,  181, 146, 213 };
static const s_InvBox INV_BOX_CMD      = {  45, -204, 146, -68 };

/* Carousel slot centres (prim X) for visible offsets -3..+3 from the selected
 * item. The items are 3D models on a 30-deg-per-slot ring, not authored sprites,
 * so these come from the projection: x = 1000 * t[0] / (t[2] + 10240) with
 * t[0] = sin(d * 1024/3)/3 and t[2] = |sin(d * 1024/3 / 4)| - 1024
 * (Inventory_PlayerItemScroll + ItemScreen_CamSet's h=1000, camera at z=-10240).
 * The outer slots bunch up under the foreshortening — +-2 and +-3 sit only ~8px
 * apart — so the bands below split on the midpoints rather than on equal widths. */
#define INV_SLOT_MIN (-3)
#define INV_SLOT_MAX 3

/* Upper X bound of each band, walking left to right; index i is offset i-3. */
static const s16 INV_SLOT_BAND_MAX[7] = { -101, -80, -31, 31, 80, 101, 160 };

/* Command rows: an 80px-wide bar at x 56..136, 16px step, 12px tall. A
 * two-command item starts at y -154, a one-command item is centred at -146. */
#define INV_CMD_X0        56
#define INV_CMD_X1        136
#define INV_CMD_STEP      16
#define INV_CMD_H         12
#define INV_CMD_Y0_SINGLE (-146)
#define INV_CMD_Y0_DOUBLE (-154)

/* Pending carousel scroll: the inventory index a click asked for, or -1.
 * func_800539A4 recycles exactly ONE carousel slot per call (it evicts the item
 * that fell 7 places off the far edge), so invItemSelectedIdx must never jump by
 * more than one — a multi-slot click is walked out one stock step at a time. */
static s32 s_scrollTarget = -1;

static int Inv_InBox(const s_InvBox* b, s32 x, s32 y)
{
    return x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1;
}

/* Command count for an item — mirrors the switch in Inventory_Logic. A pure
 * lookup (it decides how many rows exist, hence where they are); the command
 * itself is still executed by the stock code. */
static s32 Inv_CmdCount(s32 itemIdx)
{
    switch (g_SavegamePtr->items[itemIdx].command_2)
    {
        case InvCmdId_UseHealth:
        case InvCmdId_Use:
        case InvCmdId_Equip:
        case InvCmdId_Unequip:
        case InvCmdId_Reload:
        case InvCmdId_Look:
            return 1;

        case InvCmdId_EquipReload:
        case InvCmdId_UnequipReload:
        case InvCmdId_OnOff:
        case InvCmdId_UseLook:
            return 2;

        default:
            return 0;
    }
}

static void Inv_Press(u32 bits)
{
    g_Controller0->clickedBtnFlags |= bits;
    g_Controller0->heldBtnFlags    |= bits;
}

/* Move the selection to a region without the stock 7-frame settle. The region
 * transitions all reset g_Inventory_SelectionBordersDraw to 1, and Inventory_Logic
 * then REJECTS input until it climbs back past 7 — so a hover that reset it would
 * swallow the click that follows a fraction of a second later. Snapping the timer
 * to its ceiling (and Prev = Selected, which parks the bracket tween on the target)
 * keeps the click live. Same invariant the options/save-load layers follow. */
static void Inv_SnapSelection(u32 id)
{
    if (g_Inventory_SelectionId == id)
        return;

    g_Inventory_SelectionId          = id;
    g_Inventory_PrevSelectionId      = id;
    g_Inventory_SelectionBordersDraw = 8;
    g_Inventory_CmdSelectedIdx       = 0;
    Sd_PlaySfx(Sfx_MenuMove, 0, 64);
}

/* The horizontal squeeze item_screens_cam.c applies to the model matrix when the
 * viewport is genuinely stretched (Hor+ 16:9, or pillarbox off on a wide window):
 * it scales t[0], so every carousel slot moves inward and the hit bands must move
 * with it. Returns 1.0 in the default pillarboxed 4:3 case. */
static float Inv_CarouselXScale(void)
{
    extern int g_PcHorPlusEnabled;
    extern int g_PcMenuPillarbox;
    extern int g_PcWidescreenMode;

    int   stretched;
    int   sw, sh;
    float dispAspect;
    float psxAspect = 320.0f / 240.0f;

    stretched = (g_PcHorPlusEnabled && g_PcWidescreenMode == 2) ||
                (!g_PcHorPlusEnabled && !g_PcMenuPillarbox);
    if (!stretched)
        return 1.0f;

    PsyX_GetScreenSize(&sw, &sh);
    if (sh <= 0)
        return 1.0f;

    dispAspect = (float)sw / (float)sh;
    if (dispAspect <= psxAspect)
        return 1.0f;

    return psxAspect / dispAspect;
}

/* Visible carousel offset (-3..+3) under the pointer, or NO_VALUE. */
static s32 Inv_CarouselSlotAt(s32 x, s32 y)
{
    float scale;
    s32   i;

    if (y < INV_BOX_ITEM.y0 || y > INV_BOX_ITEM.y1)
        return NO_VALUE;

    scale = Inv_CarouselXScale();

    for (i = 0; i < 7; i++)
    {
        if (x <= (s32)(INV_SLOT_BAND_MAX[i] * scale))
            return i + INV_SLOT_MIN;
    }
    return INV_SLOT_MAX;
}

void Pc_Inventory_MouseUpdate(void)
{
    s32 mx, my, x, y;
    s32 clicked, rightClicked, wheel;
    s32 slotCount;

    if (!g_PcConfig.mouseCursor)
        return;

    /* Only the live inventory. Everything else gameStateSteps[1] runs (option
     * hand-off, results/save screen, the fade out) reuses these globals for
     * something else entirely. */
    if (g_GameWork.gameStateSteps[1] != 1)
    {
        s_scrollTarget = NO_VALUE;
        return;
    }

    if (!Pc_MouseCursor_UiPos(&mx, &my))
    {
        s_scrollTarget = NO_VALUE;
        return;
    }

    x = mx - INV_ORIGIN_X;
    y = my - INV_ORIGIN_Y;

    slotCount = g_SavegamePtr->inventorySlotCount;
    if (slotCount <= 0)
        return;

    /* Walk out a pending click-to-scroll. Injecting clicked+held drives the stock
     * auto-scroll path (which latches g_Inventory_IsScrolling), so it advances at
     * the same rate as holding the stick and animates itself. */
    if (s_scrollTarget != NO_VALUE)
    {
        s32 cur = g_SysWork.invItemSelectedIdx;
        s32 fwd;

        if (cur == s_scrollTarget || g_Inventory_SelectionId != InventorySelectionId_Item)
        {
            s_scrollTarget = NO_VALUE;
        }
        else
        {
            fwd = ((s_scrollTarget - cur) + slotCount) % slotCount;
            Inv_Press((fwd * 2 <= slotCount) ? ControllerFlag_LStickRight : ControllerFlag_LStickLeft);
            return;
        }
    }

    clicked      = Pc_MouseCursor_LeftClicked();
    rightClicked = Pc_MouseCursor_RightClicked();
    wheel        = Pc_MouseCursor_WheelStep();

    /* An open command submenu is modal: it owns the pointer. Hovering back over
     * the carousel must NOT silently yank the selection out from under it. */
    if (g_Inventory_SelectionId == InventorySelectionId_ItemCmd ||
        g_Inventory_SelectionId == InventorySelectionId_EquippedItemCmd)
    {
        s32 itemIdx = (g_Inventory_SelectionId == InventorySelectionId_ItemCmd)
                          ? g_SysWork.invItemSelectedIdx
                          : g_SysWork.playerCombat.weaponInventoryIdx;
        s32 count   = Inv_CmdCount(itemIdx);
        s32 rowY0   = (count >= 2) ? INV_CMD_Y0_DOUBLE : INV_CMD_Y0_SINGLE;
        s32 row     = NO_VALUE;
        s32 i;

        if (rightClicked || (clicked && !Inv_InBox(&INV_BOX_CMD, x, y)))
        {
            Inv_Press(g_GameWorkPtr->config.controllerConfig.cancel);
            return;
        }

        if (x >= INV_CMD_X0 && x <= INV_CMD_X1)
        {
            for (i = 0; i < count; i++)
            {
                s32 top = rowY0 + (i * INV_CMD_STEP);
                if (y >= top && y < (top + INV_CMD_H))
                {
                    row = i;
                    break;
                }
            }
        }

        if (row == NO_VALUE)
            return;

        /* Hover the row directly rather than injecting up/down: the stock nav
         * resets the settle timer, which would eat the click. */
        if (g_Inventory_CmdSelectedIdx != row && (Pc_MouseCursor_Moved() || clicked))
        {
            g_Inventory_CmdSelectedIdx = row;
            Sd_PlaySfx(Sfx_MenuMove, 64, 64);
        }

        if (clicked)
            Inv_Press(g_GameWorkPtr->config.controllerConfig.enter);

        return;
    }

    /* Carousel. The scroll arrows (x +-44..60, y 4..20) fall inside the -1/+1
     * bands and resolve to the same one-step scroll, so they need no case. */
    {
        s32 slot = Inv_CarouselSlotAt(x, y);
        if (slot != NO_VALUE)
        {
            if (Pc_MouseCursor_Moved() || clicked || wheel)
                Inv_SnapSelection(InventorySelectionId_Item);

            if (wheel != 0)
            {
                Inv_Press((wheel > 0) ? ControllerFlag_LStickLeft : ControllerFlag_LStickRight);
                return;
            }

            if (clicked)
            {
                if (slot == 0)
                    Inv_Press(g_GameWorkPtr->config.controllerConfig.enter);
                else
                    s_scrollTarget = ((g_SysWork.invItemSelectedIdx + slot) + slotCount) % slotCount;
            }
            return;
        }
    }

    /* Equipped item. Only selectable with something equipped — the stock Item->Up
     * transition enforces the same test, so mirror it rather than parking the
     * selection somewhere the pad could never reach. */
    if (Inv_InBox(&INV_BOX_EQUIPPED, x, y))
    {
        if (g_SysWork.playerCombat.weaponAttack < WEAPON_ATTACK(EquippedWeaponId_KitchenKnife, AttackInputType_Tap))
            return;

        if (Pc_MouseCursor_Moved() || clicked)
            Inv_SnapSelection(InventorySelectionId_EquippedItem);

        if (clicked)
            Inv_Press(g_GameWorkPtr->config.controllerConfig.enter);
        return;
    }

    /* Bottom row: Option | Exit | Map. */
    {
        u32 id = (u32)NO_VALUE;

        if (Inv_InBox(&INV_BOX_SETTINGS, x, y))
            id = InventorySelectionId_Settings;
        else if (Inv_InBox(&INV_BOX_EXIT, x, y))
            id = InventorySelectionId_Exit;
        else if (Inv_InBox(&INV_BOX_MAP, x, y))
            id = InventorySelectionId_Map;

        if (id == (u32)NO_VALUE)
            return;

        if (Pc_MouseCursor_Moved() || clicked)
            Inv_SnapSelection(id);

        if (clicked)
            Inv_Press(g_GameWorkPtr->config.controllerConfig.enter);
    }
}
