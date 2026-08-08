/* pc_autopilot.c — env-gated capture harness for headless/driven in-game
 * screenshots. STRICTLY ADDITIVE and OFF BY DEFAULT.
 *
 * Nothing in this file runs unless SH_AUTOPILOT is set in the environment:
 * Pc_Autopilot_Tick() returns on its first line when the feature is disabled,
 * and the single call site in game_main.c is itself inside #ifdef SH_PC_PORT.
 * With SH_AUTOPILOT unset the game behaves exactly as it does today.
 *
 * Why this exists: this checkout has no SH_CAPTURE_FRAME (that lives in the
 * sibling SlickAmogus checkout's PsyCross) and PsyCross's F12 screenshot key
 * needs real keyboard focus, which a rootless-XWayland session refuses to hand
 * to a synthetic XTEST client. To photograph the modded health-drink item model
 * from the real engine we need a route that needs neither a window manager nor
 * a human.
 *
 * What it does, per frame:
 *   1. Injects synthetic pad edges into g_Controller0 (the same struct
 *      Joy_ControllerDataUpdate just filled) to walk the state machine:
 *      title -> new game -> gameplay -> inventory -> item selection.
 *   2. Grants the health drink through the existing console command path.
 *   3. Calls the existing (previously unused) SH_TakeScreenshot() helper in
 *      libgs_stub.c at scripted frame numbers.
 *
 * Env:
 *   SH_AUTOPILOT=1              enable (required — absent = total no-op)
 *   SH_AUTOPILOT_DIR=<dir>      screenshot output dir (default ".")
 *   SH_AUTOPILOT_ITEM=<n>       inventory slot index to select (default 0)
 *   SH_AUTOPILOT_QUIT=<frame>   exit(0) at this frame (default 0 = never)
 *   SH_AUTOPILOT_FILM=<n>       dump every n-th inventory frame (0 = off)
 *   SH_AUTOPILOT_FILM_FROM=<f>  first inventory-held frame to dump (default 140)
 *   SH_AUTOPILOT_FILM_TO=<f>    last inventory-held frame to dump (default 500)
 *   SH_AUTOPILOT_WIRE=1         force PsyCross wireframe mode during filming
 *   SH_AUTOPILOT_FOURCLASS=1    grant 10+ real items and cycle the four-class proof items
 *   SH_AUTOPILOT_FOURCLASS_EXAMINE=1
 *                               film carousel then pickup/examine for all four classes
 *   SH_AUTOPILOT_WORLDPICKUP=1  T25: walk the REAL map2_s00 health-drink world pickup.
 *                               Places Harry on the pickup's own map point and presses
 *                               the real action button; every subsequent step (trigger
 *                               match -> SysState_EventCallback -> func_800E7D54 ->
 *                               Event_ItemTake -> GameFs_UniqueItemModelLoad ->
 *                               func_80054A04 -> Gfx_PickupItemAnimate) is stock code.
 *                               No draw hook, no forced scale, no synthetic model link.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sys/joy.h"
#include "hires_override.h"
#include "pc_modern_mesh.h"
#include "pc_item_unq.h"
#include "sh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void SH_TakeScreenshot(const char* filename); /* libgs_stub.c */

static int   s_enabled = -1;   /* -1 = not yet probed */
static char  s_dir[256]  = ".";
static int   s_itemSlot  = 0;
static long  s_quitFrame = 0;
static long  s_frame     = 0;
static int   s_gaveItem  = 0;
static int   s_shotIdx   = 0;
static int   s_unqLoad   = 0;   /* SH_AUTOPILOT_UNQ: exercise UNQ*.TMD path */
static int   s_unqItemId = 32;  /* SH_AUTOPILOT_UNQITEM; 32 = InvItemId_HealthDrink */
static int   s_unqReady  = 0;   /* set once func_80054A04 has linked the model */
static long  s_unqFrames = 0;   /* frames since the model became drawable */
static int   s_splicedLogged = 0;
static int   s_film      = 0;   /* SH_AUTOPILOT_FILM: dump interval, 0 = off */
static long  s_filmFrom  = 140; /* inventory `held` frame to start dumping */
static long  s_filmTo    = 500; /* inventory `held` frame to stop dumping */
static int   s_filmIdx   = 0;   /* separate zero-padded counter for film frames */
static int   s_wire      = 0;   /* SH_AUTOPILOT_WIRE: drive g_dbg_wireframeMode */
static int   s_fourClass = 0;   /* SH_AUTOPILOT_FOURCLASS: real multi-item acceptance inventory */
static int   s_worldPickup = 0; /* SH_AUTOPILOT_WORLDPICKUP: real map2_s00 health-drink pickup */
static int   s_wpPhase     = 0; /* 0 settle, 1 placed, 2 msg, 3 filming pickup,
                                 * 4 answering Yes, 5 awaiting gameplay,
                                 * 6 requesting inventory, 7 done */
static long  s_wpPhaseFrames = 0;
static int   s_wpTriggered = 0;
static int   s_wpPlaced    = 0;
static int   s_wpAttempts  = 0;
static long  s_wpFilmCount = 0;  /* hard cap so a stuck phase cannot fill the disk */
static long  s_wpFilmCap   = 1200;
static long  s_wpPickupFilmFrames = 240; /* how long to hold on the take-screen */
static long  s_wpInvFilmFrames    = 620; /* how long to hold on the carousel */
static int   s_wpInvSelected = 0;

/* r108 STEP 3 -- DRIVE THE CURSOR ACROSS THE MODERN-LINKED SLOTS.
 *
 * t37 (rev 106/107) linked FOUR items source=modern -- slots 1/2/6 as well as
 * slot 3 -- but the harness selected HealthDrink once at held>=60 and never
 * moved again, so every one of its 128 filmed frames was the slot-3 view.
 * The measured max of 3 distinct meshes in one frame was therefore a property
 * of OUR camera, not of the engine. Rotating the selection re-windows the
 * seven-slot carousel around each modern item in turn.
 *
 * These four ids are exactly the ones t37's engine log proved reach
 * [UNIFIEDITEM] carousel link source=modern on map2_s00. Nothing here grants
 * anything; the grant already happened in world-pickup phase 5, after the
 * genuine Event_ItemTake. This only moves the cursor the way a player would. */
static const u8 s_wpModernRotation[] = {
    32,  /* HealthDrink     -> UNQ21 cube      file 1516, t37 slot 3 */
    226, /* GasolineTank    -> UNQE2 hexprism  file 1592, t37 slot 1 */
    88,  /* ChannelingStone -> UNQ56 cylinder  file 1543, t37 slot 2 */
    163  /* HyperBlaster    -> UNQA3 cone      file 1586, t37 slot 6 */
};
static int   s_wpRotIdx    = 0;
static u8    s_wpRotItem   = 32;
static int   s_wpInvDone     = 0;
static int   s_fourClassExamine = 0; /* carousel + examine proof; otherwise fully inert */
static int   s_fourClassOverrideReady = 0;
static int   s_fourClassExaminePhase = 0; /* 0 select, 1 carousel, 2 await load, 3 examine */
static int   s_fourClassExamineIndex = 0;
static long  s_fourClassPhaseFrames = 0;

/* Draw-side stock-texture provenance for the four-class harness. The stock
 * TMD renderer otherwise sees only tpage/clut, not inventory identity. */
static unsigned int s_fourClassStockCheckerTexture = 0;
static int s_fourClassStockTextureLogged = 0;
static long  s_gameplayTick = -1;
static long  s_inventoryTick = -1;
static long  s_uniqueLinkTick = -1;
static long  s_firstCaptureTick = -1;
static long  s_lastCaptureTick = -1;
static int   s_captureCount = 0;
static unsigned long long s_tickStartSwapCount = 0;
static int   s_swapCountPrimed = 0;

extern int GsDeterministicClockEnabled(void); /* libgs_stub.c */
extern unsigned long long GsDeterministicFrameCount(void); /* libgs_stub.c */

static void milestone_capture(void)
{
    if (s_firstCaptureTick < 0)
        s_firstCaptureTick = s_frame;
    s_lastCaptureTick = s_frame;
    s_captureCount++;
}

static void milestone_signature(void)
{
    printf("[AUTOPILOT_SIGNATURE] gameplay=%ld inventory=%ld unique_link=%ld first_capture=%ld last_capture=%ld captures=%d d_game_inventory=%ld d_inventory_link=%ld d_link_first=%ld d_first_last=%ld\n",
           s_gameplayTick, s_inventoryTick, s_uniqueLinkTick,
           s_firstCaptureTick, s_lastCaptureTick, s_captureCount,
           s_inventoryTick - s_gameplayTick,
           s_uniqueLinkTick - s_inventoryTick,
           s_firstCaptureTick - s_uniqueLinkTick,
           s_lastCaptureTick - s_firstCaptureTick);
    fflush(stdout);
}


static void autopilot_probe(void)
{
    const char* v = getenv("SH_AUTOPILOT");
    s_enabled = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (!s_enabled)
        return;

    v = getenv("SH_AUTOPILOT_DIR");
    if (v && v[0]) {
        strncpy(s_dir, v, sizeof(s_dir) - 1);
        s_dir[sizeof(s_dir) - 1] = '\0';
    }
    v = getenv("SH_AUTOPILOT_ITEM");
    if (v && v[0]) s_itemSlot = atoi(v);
    v = getenv("SH_AUTOPILOT_QUIT");
    if (v && v[0]) s_quitFrame = atol(v);
    v = getenv("SH_AUTOPILOT_UNQ");
    if (v && v[0] && v[0] != '0') s_unqLoad = 1;
    v = getenv("SH_AUTOPILOT_UNQITEM");
    if (v && v[0]) s_unqItemId = atoi(v);
    v = getenv("SH_AUTOPILOT_FILM");
    if (v && v[0]) s_film = atoi(v);
    v = getenv("SH_AUTOPILOT_FILM_FROM");
    if (v && v[0]) s_filmFrom = atol(v);
    v = getenv("SH_AUTOPILOT_FILM_TO");
    if (v && v[0]) s_filmTo = atol(v);
    v = getenv("SH_AUTOPILOT_WIRE");
    if (v && v[0] && v[0] != '0') s_wire = 1;
    v = getenv("SH_AUTOPILOT_FOURCLASS");
    if (v && v[0] && v[0] != '0') s_fourClass = 1;
    v = getenv("SH_AUTOPILOT_FOURCLASS_EXAMINE");
    if (v && v[0] && v[0] != '0') {
        s_fourClassExamine = 1;
        s_fourClass = 1;
        s_unqLoad = 1;
    }
    v = getenv("SH_AUTOPILOT_WORLDPICKUP");
    if (v && v[0] && v[0] != '0') {
        s_worldPickup = 1;
        /* The world-pickup route must NOT arm any of the SYNTHETIC DRAW paths:
         * the whole point is that only stock code touches the model.
         * s_unqLoad and s_fourClassExamine both force a draw we construct
         * ourselves (Gfx_PickupItemAnimate / a forced gameStateSteps[1]=13),
         * so they stay hard-off here.
         *
         * s_fourClass is NOT in that class and no longer forced off (r106).
         * All it does is call Inventory_AddSpecialItem -- it changes what is
         * IN the inventory and nothing about how anything is drawn. Zeroing it
         * made the carousel half (gate half 1) and the pickup half (half 2)
         * mutually exclusive IN OUR OWN PROBE, which was then nearly reported
         * as "both halves cannot share a session". That would have been the
         * same error as rev 95's {32,133,163} ceiling and rev 104's 3-asset
         * ceiling: our harness mistaken for the engine.
         *
         * The grant is deliberately deferred to world-pickup phase 5, i.e.
         * AFTER Event_ItemTake has completed, so nothing is in the inventory
         * that the route under test did not put there. */
        s_unqLoad = 0;
        s_fourClassExamine = 0;
    }

    /* PsyCross exposes wireframe through GR_SetWireframe(glPolygonMode GL_LINE),
     * but its F1 toggle is compiled out under NDEBUG. The mode flag itself is a
     * plain global, so set it here: this is the engine's own rasterizer path,
     * not a reconstruction. */
    if (s_wire) {
        extern int g_dbg_wireframeMode;
        g_dbg_wireframeMode = 1;
        SH_DBG("[AUTOPILOT] wireframe mode forced on");
    }

    SH_DBG("[AUTOPILOT] enabled dir=%s item=%d quit=%ld unq=%d unqItem=%d fourClass=%d fourClassExamine=%d",
           s_dir, s_itemSlot, s_quitFrame, s_unqLoad, s_unqItemId, s_fourClass,
           s_fourClassExamine);
}

void Pc_Autopilot_LogStockTexture(u8 itemId, const void* object)
{
    const GsDOBJ2* drawObject = (const GsDOBJ2*)object;
    const struct TMD_STRUCT* tmd;
    const unsigned char* primitive;
    int primitiveCount;

    if (s_enabled != 1 || !s_fourClass || s_fourClassStockTextureLogged ||
        itemId != InvItemId_Handgun || drawObject == NULL || drawObject->tmd == NULL)
        return;
    tmd = (const struct TMD_STRUCT*)drawObject->tmd;
    primitive = (const unsigned char*)tmd->primtop;
    primitiveCount = (int)tmd->primn;
    while (primitive != NULL && primitiveCount-- > 0)
    {
        int words = primitive[1];
        if (words == 0)
            return;
        if ((primitive[3] & 4) != 0)
        {
            u16 clut = (u16)(primitive[6] | (primitive[7] << 8));
            u16 tpage = (u16)(primitive[10] | (primitive[11] << 8));
            unsigned int texture = HiresOverride_LookupByTpageClut(tpage, clut,
                                                                    NULL, NULL, NULL, NULL, NULL, NULL);
            if (texture != 0 && texture == s_fourClassStockCheckerTexture)
            {
                s_fourClassStockTextureLogged = 1;
                SH_DBG("[T17_STOCK_TEXTURE] item=%u source=installed-override name=T17-four-class-checker texture=%u measured=tpage%u/clut%u bytes=identical-to-class4",
                       (unsigned)itemId, texture, (unsigned)tpage, (unsigned)clut);
                return;
            }
        }
        primitive += (1 + words) * 4;
    }
}

static void autopilot_register_fourclass_override(void)
{
    unsigned char checker[64 * 64 * 4];
    int x;
    int y;

    if (!s_fourClass || s_fourClassOverrideReady)
        return;
    /* The checker override belongs to the T17 stock-texture-substitution proof,
     * which is NOT part of this gate. Registering it here would repaint the
     * proof items and hide the modded GLB's own embedded texture, which is
     * precisely what shot 3/4 have to show. Skip it for the examine gate. */
    if (s_fourClassExamine) {
        s_fourClassOverrideReady = 1;
        SH_DBG("[T21_EXAMINE] checker override registration SKIPPED; items render with their own textures");
        return;
    }
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            int cell = ((x >> 3) ^ (y >> 3)) & 1;
            unsigned char* pixel = &checker[(y * 64 + x) * 4];
            pixel[0] = cell ? 255 : 0;
            pixel[1] = cell ? 0 : 255;
            pixel[2] = 255;
            pixel[3] = 255;
        }
    }
    /* T17 measured the stock Handgun live at tpage=15/clut=523
     * (CLUT cell 176,8), with currentOverride=0. Health Drink's measured
     * tpage=15/clut=75 was occupied by the owner's loose row-1 texture, so it
     * is intentionally not used for class 2. Both registrations below upload
     * this exact same checker byte array. */
    if (HiresOverride_RegisterRGBA("T17 four-class checker handgun",
                                   checker, 64, 64,
                                   960, 0, 64, 256, 176, 8, 4) != 0 ||
        HiresOverride_RegisterRGBA("T17 four-class checker hyperblaster",
                                   checker, 64, 64,
                                   832, 0, 32, 256, 240, 4, 4) != 0)
    {
        SH_DBG("[T15_FOURCLASS] checker override registration failed");
        return;
    }
    s_fourClassStockCheckerTexture = HiresOverride_LookupByTpageClut(15, 523,
                                                                      NULL, NULL, NULL, NULL, NULL, NULL);
    if (s_fourClassStockCheckerTexture == 0)
    {
        SH_DBG("[T17_FOURCLASS] checker registration did not win measured stock key tpage=15/clut=523");
        return;
    }
    s_fourClassOverrideReady = 1;
    SH_DBG("[T17_FOURCLASS] identical checker bytes registered keys class2=tpage15/clut523 texture=%u class4=tpage13/clut271",
           s_fourClassStockCheckerTexture);
}

static int autopilot_inventory_has(u8 itemId)
{
    int i;

    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == itemId)
            return 1;
    return 0;
}

static void autopilot_give_item(u8 itemId, u8 itemCount)
{
    int stackable = (itemId >= InvItemId_HealthDrink && itemId <= InvItemId_Ampoule) ||
                    itemId >= InvItemId_HandgunBullets;

    if (stackable || !autopilot_inventory_has(itemId))
        Inventory_AddSpecialItem(itemId, itemCount ? itemCount : 1);
}

static void autopilot_give_fourclass_inventory(void)
{
    /* CEILING LIFTED 2026-08-04 (Ric: "we want multiple imported items visible on
     * the carousel at a time. That's a part of acceptance criteria here.").
     *
     * This list previously held 14 hand-picked ids whose intersection with
     * map0_s00's LOADABLE_INVENTORY_ITEMS pack was only {32,133,163} -- three
     * items. That three was reported as the map's honest ceiling; it was not.
     * It was OUR harness limit mistaken for a game limit.
     *
     * The pack (map0_s00_anim_info.c:178) is what actually gates carousel
     * residency, and every one of its 7 real ids has a UNQ identity that
     * resolves in the file table (UNQ56=1543, UNQ85=1581, UNQ86=1582,
     * UNQE2=1592, plus UNQ21/UNQ83/UNQA3). Granting the pack set makes every
     * resident inventory item modern-backed, which is the precondition for
     * several distinct imported meshes appearing in one carousel frame. */
    static const u8 itemIds[] = {
        InvItemId_HealthDrink,     /* 32  -> UNQ21 */
        InvItemId_ChannelingStone, /* 88  -> UNQ56 */
        InvItemId_RockDrill,       /* 130 -> UNQ85 */
        InvItemId_Chainsaw,        /* 133 -> UNQ83 */
        InvItemId_Katana,          /* 134 -> UNQ86 */
        InvItemId_HyperBlaster,    /* 163 -> UNQA3 */
        InvItemId_GasolineTank     /* 226 -> UNQE2 */
    };
    int i;

    for (i = 0; i < (int)(sizeof(itemIds) / sizeof(itemIds[0])); i++)
        autopilot_give_item(itemIds[i], itemIds[i] == InvItemId_HandgunBullets ? 60 : 1);
    g_SavegamePtr->clearGameEndings |= GameEndingFlag_Ufo;
    SH_DBG("[T15_FOURCLASS] populated inventory slots=%u items=14",
           (unsigned)g_SavegamePtr->inventorySlotCount);
}

static int autopilot_find_item_slot(u8 itemId)
{
    int i;

    for (i = 0; i < g_SavegamePtr->inventorySlotCount; i++)
        if (g_SavegamePtr->items[i].id_0 == itemId)
            return i;
    return -1;
}

/* Which of the 7 carousel display slots currently holds the selected item.
 * This is the same lookup the item screen itself uses (item_screens_3.c:3163,
 * D_800C3E18[i] == g_SysWork.invItemSelectedIdx), so it resolves the exact
 * GsDOBJ2 the inventory draw loop will submit for this item. */
static int autopilot_carousel_slot(void)
{
    int i;

    for (i = 0; i < 7; i++)
        if (D_800C3E18[i] == g_SysWork.invItemSelectedIdx)
            return i;
    return -1;
}

static void autopilot_select_item(u8 itemId)
{
    int slot = autopilot_find_item_slot(itemId);

    if (slot < 0) {
        SH_DBG("[T15_FOURCLASS] select failed item=%u", (unsigned)itemId);
        return;
    }
    g_SysWork.invItemSelectedIdx = slot;
    /* T15 originally changed only the selection index. Normal controller
     * scrolling also calls func_800539A4 to populate the entering carousel
     * slot; bypassing that call meant far-away targets (notably HyperBlaster)
     * never reached Gfx_Items_Display or the modern-mesh probe. Rebuild the
     * seven-slot window through the inventory's own setup routine after each
     * proof-only direct jump so item identity and geometry stay coupled. */
    Gfx_Items_Draw();
    SH_DBG("[T15_FOURCLASS] select item=%u slot=%d carousel rebuilt", (unsigned)itemId, slot);
}

/* Drive one synthetic button edge. heldBtnFlags/clickedBtnFlags are exactly
 * what Joy_ControllerDataUpdate computed this frame; overwriting them here
 * (immediately after that call) is indistinguishable, to every consumer, from
 * a real pad press. */
static void press(u32 flags)
{
    g_Controller0->heldBtnFlags    = (e_ControllerFlags)flags;
    g_Controller0->clickedBtnFlags = (e_ControllerFlags)flags;
    g_Controller0->pulsedBtnFlags     = (e_ControllerFlags)flags;
    g_Controller0->pulsedGuiBtnFlags  = (e_ControllerFlags)flags;
}

static void capture(const char* tag)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/auto_%02d_%s.bmp", s_dir, s_shotIdx++, tag);
    SH_TakeScreenshot(path);
    milestone_capture();
    SH_DBG("[AUTOPILOT] frame %ld state=%d shot %s",
           s_frame, (int)g_GameWork.gameState, path);
}

/* Film frames get their own 4-digit counter so lexical sort == temporal order
 * (capture()'s %02d would put "100" before "99" once the count passes 99). */
static void film_capture(void)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/film_%04d.bmp", s_dir, s_filmIdx++);
    SH_TakeScreenshot(path);
    milestone_capture();
}

/* Capture hygiene: the menu mouse pointer (Pc_MouseCursor_Draw, overlay OT
 * layer 4) draws on top of the inventory carousel and lands right over the
 * item being validated. While the autopilot is driving a capture there is no
 * human to move it, so suppress it. Gated on the autopilot being active, so
 * normal play is untouched. */
int Pc_Autopilot_SuppressCursor(void)
{
    if (s_enabled < 0)
        autopilot_probe();
    return s_enabled == 1;
}

void Pc_Autopilot_Tick(void)
{
    if (s_enabled < 0)
        autopilot_probe();
    if (!s_enabled)
        return;

    if (GsDeterministicClockEnabled()) {
        unsigned long long swapCount = GsDeterministicFrameCount();
        if (s_captureCount > 0 && s_swapCountPrimed && swapCount != s_tickStartSwapCount + 1) {
            printf("[AUTOPILOT_SIGNATURE] FAIL swap_boundary previous_tick=%llu current_tick=%llu expected_delta=1\n",
                   s_tickStartSwapCount, swapCount);
            fflush(stdout);
            exit(6);
        }
        s_tickStartSwapCount = swapCount;
        s_swapCountPrimed = 1;
    }

    s_frame++;

    /* Periodic state trace so a headless run is diagnosable from the log. */
    if ((s_frame % 60) == 0) {
        SH_DBG("[AUTOPILOT] frame %ld state=%d step1=%d",
               s_frame, (int)g_GameWork.gameState,
               (int)g_GameWork.gameStateSteps[1]);
    }

    switch (g_GameWork.gameState)
    {
        /* Boot / logos / intro movies: hammer Start to skip through. */
        case GameState_KonamiLogo:
        case GameState_KcetLogo:
        case GameState_MovieIntroFadeIn:
        case GameState_MovieIntroAlternate:
        case GameState_MovieIntro:
        case GameState_MovieOpening:
            if ((s_frame % 8) == 0) press(ControllerFlag_Start);
            break;

        /* Main menu: the cursor starts on "Load Game"? No — on New Game.
         * Confirm with Circle (the game's `enter` binding). */
        case GameState_MainMenu:
            if ((s_frame % 20) == 0) press(ControllerFlag_Circle);
            break;

        /* Gameplay: grant the item once, then open the item screen.
         *
         * The item button is only consumed by SysState_Gameplay_Update, and
         * only once gameplay is actually live, so drive the SAME transition
         * that button takes (SysWork_StateSetNext(SysState_StatusMenu)) once
         * the system state settles. That reaches GameState_LoadStatusScreen ->
         * GameState_InventoryScreen through the stock path. */
        case GameState_InGame:
        {
            static long s_ingameEnter = 0;

            if (s_ingameEnter == 0) {
                s_ingameEnter = s_frame;
                s_gameplayTick = s_frame;
                SH_DBG("[AUTOPILOT] gameplay reached at frame %ld", s_frame);
            }

            if (s_worldPickup) {
                /* ============================================================
                 * T25 WORLD PICKUP ROUTE
                 *
                 * Goal: exercise the pickup a PLAYER performs, with zero
                 * synthetic model handling. This block does exactly two
                 * things a player's hands do:
                 *   (a) put Harry on the health drink's own map point, facing
                 *       into its trigger box (a walk we cannot script blind in
                 *       a headless session), and
                 *   (b) press the action button.
                 *
                 * Everything after that is stock engine code:
                 *   Event_Update (events_main.c:172) evaluates
                 *     TriggerType_TouchObbFacing against MAP_POINTS[67] and
                 *     TriggerActivationType_Button against the pad edge
                 *   -> g_MapEventSysState = SysState_EventCallback, param 3
                 *   -> SysState_EventCallback_Update (game_sys_states.c:1176)
                 *   -> g_MapOverlayHdr.mapEventFuncs[3] == func_800E7D54
                 *   -> map2_s00.c:298 Event_ItemTake(InvItemId_HealthDrink,...)
                 *   -> Event_InvItemCmd -> GameFs_UniqueItemModelLoad(32)
                 *   -> func_80054A04(32)  (links FS_BUFFER_5 into D_800C3E08,
                 *      calls Pc_ModernMesh_LinkObject(Pc_ItemUnq_FromItemId(32)))
                 *   -> Gfx_PickupItemAnimate(32)  (its own scale ramp)
                 *
                 * We never call func_80054A04, Gfx_PickupItemAnimate,
                 * GameFs_UniqueItemModelLoad or touch g_Items_Transforms[9].
                 * ============================================================ */
                static long s_wpEnter = 0;
                long wpHeld;
                extern u32    g_MapEventParam;
                extern q19_12 g_Items_PickupScale;

                if (s_wpEnter == 0) {
                    s_wpEnter = s_frame;
                    s_gameplayTick = s_frame;
                    SH_DBG("[T25_WORLDPICKUP] gameplay reached at frame %ld", s_frame);
                }
                wpHeld = s_frame - s_wpEnter;

                /* Let the map finish loading and the player spawn settle.
                 * Entering map2_s00 fresh, the FIRST thing a player hits is the
                 * exit-cafe cutscene: map2_s00_events_data.c:373 is a
                 * TriggerType_None entry (param 8, MapEvent_CutsceneExitCafe)
                 * gated on EventFlag_146. TriggerType_None returns immediately
                 * from Event_Update (events_main.c:127-133), so NO later event --
                 * including the health drink at entry 143 -- can be reached until
                 * it has played. Its flags_8_13=1 makes SysState_EventCallback_Update
                 * set EventFlag_146 on entry, so it runs exactly once.
                 * Skip it the way a player does: the skip button. */
                if (s_wpPhase == 0) {
                    if (wpHeld > 90 && (s_wpPhaseFrames % 10) == 0)
                        press(g_GameWorkPtr->config.controllerConfig.skip);
                    s_wpPhaseFrames++;
                    if (wpHeld > 150 && g_SysWork.sysState == SysState_Gameplay &&
                        Savegame_EventFlagGet(EventFlag_146)) {
                        s_wpPhase = 1;
                        s_wpPhaseFrames = 0;
                        SH_DBG("[T25_WORLDPICKUP] intro cutscene cleared (flag146 set) at frame %ld", s_frame);
                    } else if (wpHeld > 4000) {
                        SH_DBG("[T25_WORLDPICKUP] TIMEOUT clearing intro cutscene; sysState=%d flag146=%d",
                               (int)g_SysWork.sysState, (int)Savegame_EventFlagGet(EventFlag_146));
                        s_wpPhase = 4;
                    }
                    break;
                }

                if (s_wpPhase == 1 && !s_wpPlaced) {
                    /* MAP_POINTS[67] of map2_s00 is the health drink's own
                     * point of interest (map2_s00_events_data.c:1160-1166,
                     * pointOfInterestIdx = 67, sysState = SysState_EventCallback,
                     * eventParam = 3). Read the live map point rather than
                     * hardcoding coordinates, so this stays correct if the
                     * table moves. */
                    const s_MapPoint2d* mp = &g_MapOverlayHdr.mapPoints[67];

                    g_SysWork.playerWork.player.position.vx = mp->positionX;
                    g_SysWork.playerWork.player.position.vz = mp->positionZ;
                    /* Face into the OBB. Event_CollideObbFacingCheck's
                     * half-sin/half-cos test is satisfied over a wide arc at
                     * the point itself; 135 deg sits inside it for triggerParam0=28. */
                    g_SysWork.playerWork.player.rotation.vy = Q12_ANGLE(135.0f);
                    s_wpPlaced = 1;
                    s_wpPhaseFrames = 0;
                    SH_DBG("[T25_WORLDPICKUP] placed at POI67 pos=(%d,%d) rotY=%d flag175=%d",
                           (int)g_SysWork.playerWork.player.position.vx,
                           (int)g_SysWork.playerWork.player.position.vz,
                           (int)g_SysWork.playerWork.player.rotation.vy,
                           (int)Savegame_EventFlagGet(EventFlag_M2S00_PickupHealthDrink));
                    break;
                }

                /* Press the REAL action button. Event_Update tests
                 * g_Controller0->clickedBtnFlags & controllerConfig.action
                 * (events_main.c:194) — the same bit a pad press sets. */
                if (s_wpPhase == 1) {
                    s_wpPhaseFrames++;
                    /* Re-assert the stance and press on the SAME frame. The
                     * player object keeps simulating between ticks (idle drift,
                     * collision pushout, camera ease), and the OBB facing test
                     * is both positional and directional, so a stance written
                     * on one frame is not guaranteed to still hold on the frame
                     * the button edge is evaluated. */
                    {
                        const s_MapPoint2d* mp2 = &g_MapOverlayHdr.mapPoints[67];
                        g_SysWork.playerWork.player.position.vx = mp2->positionX;
                        g_SysWork.playerWork.player.position.vz = mp2->positionZ;
                        g_SysWork.playerWork.player.rotation.vy = Q12_ANGLE(135.0f);
                    }
                    if (g_SysWork.sysState == SysState_Gameplay &&
                        (s_wpPhaseFrames % 6) == 0 && s_wpAttempts < 300) {
                        press(g_GameWorkPtr->config.controllerConfig.action);
                        s_wpAttempts++;
                        if (s_wpAttempts <= 3 || (s_wpAttempts % 50) == 0)
                            SH_DBG("[T25_WORLDPICKUP] action pressed attempt=%d sysState=%d clicked=0x%x attackRecv=%d",
                                   s_wpAttempts, (int)g_SysWork.sysState,
                                   (unsigned)g_Controller0->clickedBtnFlags,
                                   (int)g_SysWork.playerWork.player.attackReceived);
                    }
                    if (s_wpPhaseFrames == 60) {
                        /* Walk g_MapOverlayHdr.mapEvents exactly the way
                         * Event_Update does and report where it stops, so a
                         * non-firing trigger can be attributed to the real
                         * gate instead of guessed at. Read-only. */
                        const s_EventData* ev = &g_MapOverlayHdr.mapEvents[-1];
                        int idx = -1;
                        for (;;) {
                            ev++; idx++;
                            if (idx >= 256) { SH_DBG("[T25_EVWALK] cap at 256"); break; }
                            if (ev->triggerType == NO_VALUE) {
                                SH_DBG("[T25_EVWALK] sentinel at idx=%d", idx);
                                break;
                            }
                            if ((int)ev->sysState == SysState_EventCallback && (int)ev->eventParam == 3)
                                SH_DBG("[T25_EVWALK] HEALTHDRINK entry idx=%d poi=%d trig=%d act=%d disFlag=%d disSet=%d reqFlag=%d",
                                       idx, (int)ev->pointOfInterestIdx, (int)ev->triggerType,
                                       (int)ev->activationType, (int)ev->disabledEventFlag,
                                       (int)Savegame_EventFlagGet(ev->disabledEventFlag),
                                       (int)ev->requiredEventFlag);
                            if ((int)ev->triggerType == TriggerType_None)
                                SH_DBG("[T25_EVWALK] TriggerType_None idx=%d param=%d disFlag=%d disSet=%d reqFlag=%d reqSet=%d",
                                       idx, (int)ev->eventParam, (int)ev->disabledEventFlag,
                                       (int)Savegame_EventFlagGet(ev->disabledEventFlag),
                                       (int)ev->requiredEventFlag,
                                       ev->requiredEventFlag ? (int)Savegame_EventFlagGet(ev->requiredEventFlag) : -1);
                        }
                    }
                    if ((s_wpPhaseFrames % 60) == 0) {
                        const s_MapPoint2d* mpd = &g_MapOverlayHdr.mapPoints[67];
                        extern bool Event_CollideObbFacingCheck(s_MapPoint2d* mapPoint);
                        int obb = Event_CollideObbFacingCheck((s_MapPoint2d*)mpd);
                        int dark = (g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & 2) ? 1 : 0;
                        int fl   = g_SysWork.field_2388.isFlashlightOn_15 ? 1 : 0;
                        int e0   = (g_SysWork.field_2388.field_1C[0].effectsInfo_0.field_0.s_field_0.field_0 & 1) ? 1 : 0;
                        int e1   = (g_SysWork.field_2388.field_1C[1].effectsInfo_0.field_0.s_field_0.field_0 & 1) ? 1 : 0;
                        SH_DBG("[T25_WORLDPICKUP] probe f=%ld pos=(%d,%d) rotY=%d obbFacing=%d busy=%d flag175=%d actionMask=0x%x dark=%d flashlight=%d e0=%d e1=%d darkGate=%d attackRecv=%d clicked=0x%x held=0x%x",
                               s_wpPhaseFrames,
                               (int)g_SysWork.playerWork.player.position.vx,
                               (int)g_SysWork.playerWork.player.position.vz,
                               (int)g_SysWork.playerWork.player.rotation.vy,
                               obb, (int)Player_IsBusy(),
                               (int)Savegame_EventFlagGet(EventFlag_M2S00_PickupHealthDrink),
                               (unsigned)g_GameWorkPtr->config.controllerConfig.action,
                               dark, fl, e0, e1,
                               (dark && !fl && (e0 || e1)) ? 1 : 0,
                               (int)g_SysWork.playerWork.player.attackReceived,
                               (unsigned)g_Controller0->clickedBtnFlags,
                               (unsigned)g_Controller0->heldBtnFlags);
                    }
                    /* The HEALTH DRINK event specifically: param 3 is
                     * func_800E7D54 in map2_s00_header.c's g_MapEventFuncs.
                     * Any other param is a different event and must not be
                     * mistaken for the pickup. */
                    if (g_SysWork.sysState == SysState_EventCallback && g_MapEventParam == 3) {
                        s_wpTriggered = 1;
                        s_uniqueLinkTick = s_frame;
                        s_wpPhase = 2;
                        s_wpPhaseFrames = 0;
                        SH_DBG("[T25_WORLDPICKUP] EVENT FIRED param=3 (func_800E7D54) frame=%ld", s_frame);
                    } else if (s_wpPhaseFrames > 2500) {
                        SH_DBG("[T25_WORLDPICKUP] TIMEOUT waiting for param=3; sysState=%d param=%d",
                               (int)g_SysWork.sysState, (int)g_MapEventParam);
                        s_wpPhase = 4;
                    }
                    break;
                }

                /* Phase 2: func_800E7D54 step 0/1 shows map message 46 and
                 * waits for a confirm before advancing to step 2's
                 * Event_ItemTake. Nudge the message along with the same
                 * action edge a player would use. */
                if (s_wpPhase == 2) {
                    s_wpPhaseFrames++;
                    if (g_SysWork.sysStateSteps[0] < 2 && (s_wpPhaseFrames % 24) == 0)
                        press(g_GameWorkPtr->config.controllerConfig.action);
                    if (g_SysWork.sysStateSteps[0] >= 2) {
                        s_wpPhase = 3;
                        s_wpPhaseFrames = 0;
                        SH_DBG("[T25_WORLDPICKUP] reached Event_ItemTake step0=2 frame=%ld", s_frame);
                    } else if (s_wpPhaseFrames > 900) {
                        SH_DBG("[T25_WORLDPICKUP] TIMEOUT at message; step0=%d",
                               (int)g_SysWork.sysStateSteps[0]);
                        s_wpPhase = 3;
                        s_wpPhaseFrames = 0;
                    }
                }

                /* Phase 3: Event_ItemTake is running its own state machine.
                 * Film it. Do NOT press anything: EventState_SelectionDialog
                 * holds the take-screen with the model spinning until the
                 * player answers Yes/No, which is exactly the view under test. */
                if (s_wpPhase == 3) {
                    s_wpPhaseFrames++;
                    if (s_wpPhaseFrames == 1)
                        SH_DBG("[T25_WORLDPICKUP] filming pickup; step1=%d",
                               (int)g_SysWork.sysStateSteps[1]);
                    if ((s_wpPhaseFrames % 60) == 0)
                        SH_DBG("[T25_WORLDPICKUP] pickup f=%ld step1=%d pickupScale=%d scale=(%d,%d,%d) coordT=(%d,%d,%d)",
                               s_wpPhaseFrames, (int)g_SysWork.sysStateSteps[1],
                               (int)g_Items_PickupScale,
                               (int)g_Items_Transforms[9].scale.vx,
                               (int)g_Items_Transforms[9].scale.vy,
                               (int)g_Items_Transforms[9].scale.vz,
                               (int)g_Items_Coords[9].coord.t[0],
                               (int)g_Items_Coords[9].coord.t[1],
                               (int)g_Items_Coords[9].coord.t[2]);

                    /* T26: the take-screen is only the FIRST half of the shot.
                     * Once it has been filmed long enough to show the hold-up
                     * and spin, answer the dialog the way a player does so the
                     * item actually enters the inventory and the SAME session
                     * can then show it in the carousel.
                     *
                     * Event_ItemTake's EventState_SelectionDialog is waiting on
                     * Event_DisplayMapMsg(true, msg, 3, NO_VALUE, 0, true).
                     * Gfx_MapMsg_Draw returns selectedEntryIdx+1 on a confirm
                     * (map_msg_display.c:534), and Event_DisplayMapMsg maps
                     * MapMsgState_SelectEntry0 -> step 3 == EventState_TakeItem.
                     * So: leave selectedEntryIdx at 0 ("Yes", its init value at
                     * map_msg_display.c:142) and press the real ENTER bit. The
                     * confirm branch at map_msg_display.c:421 is the stock one. */
                    if (s_wpPhaseFrames > s_wpPickupFilmFrames) {
                        s_wpPhase = 4;
                        s_wpPhaseFrames = 0;
                        SH_DBG("[T25_WORLDPICKUP] pickup filmed %ld frames; answering Yes",
                               s_wpPickupFilmFrames);
                    }
                }

                /* Phase 4: hold "Yes" and press enter until Event_ItemTake
                 * unwinds through EventState_TakeItem -> Inventory_AddSpecialItem
                 * and restores gameplay (events_util.c:1203-1218). */
                if (s_wpPhase == 4) {
                    s_wpPhaseFrames++;
                    g_MapMsg_Select.selectedEntryIdx = 0; /* "Yes" */
                    if ((s_wpPhaseFrames % 8) == 0)
                        press(g_GameWorkPtr->config.controllerConfig.enter);
                    if (autopilot_inventory_has((u8)InvItemId_HealthDrink)) {
                        s_wpPhase = 5;
                        s_wpPhaseFrames = 0;
                        SH_DBG("[T25_WORLDPICKUP] item TAKEN: health drink in inventory, slots=%u",
                               (unsigned)g_SavegamePtr->inventorySlotCount);
                    } else if (s_wpPhaseFrames > 900) {
                        SH_DBG("[T25_WORLDPICKUP] TIMEOUT answering Yes; step1=%d sysState=%d",
                               (int)g_SysWork.sysStateSteps[1], (int)g_SysWork.sysState);
                        s_wpPhase = 5;
                        s_wpPhaseFrames = 0;
                    }
                }

                /* Phase 5: let control come back, then request the status menu
                 * exactly the way the ITEM button does
                 * (SysWork_StateSetNext(SysState_StatusMenu)). The engine then
                 * runs GameState_LoadStatusScreen -> GameState_InventoryScreen
                 * on its own; the inventory half of the shot is filmed from the
                 * GameState_InventoryScreen case below. */
                if (s_wpPhase == 5) {
                    s_wpPhaseFrames++;
                    /* BOTH GATE HALVES, ONE SESSION (r106). The health drink is
                     * now in the inventory because the GENUINE route put it
                     * there (Event_ItemTake -> GameFs_UniqueItemModelLoad ->
                     * Gfx_PickupItemAnimate, filmed in phases 3-4). Only now do
                     * we grant the rest of map2_s00's carousel pack, so the
                     * pickup half is measured on an inventory the route owns and
                     * the carousel half gets several distinct imported meshes.
                     *
                     * map2_s00 can host both halves because its loadableItems is
                     * its OWN 35-entry array (build_gen/extracted_data/
                     * map2_s00_extracted_data.c:23, bound via DT_SYMBOLIC in
                     * maps/map2_s00.so), not the exe's 8-entry map0_s00 copy.
                     * All 7 authored ids are in it -- see
                     * FINDING-map2s00-both-halves-r106.md. */
                    if (s_fourClass && !s_gaveItem) {
                        autopilot_give_fourclass_inventory();
                        s_gaveItem = 1;
                        SH_DBG("[T25_WORLDPICKUP] granted carousel pack AFTER genuine take; slots=%u",
                               (unsigned)g_SavegamePtr->inventorySlotCount);
                    }
                    if (g_SysWork.sysState == SysState_Gameplay &&
                        s_wpPhaseFrames > 60 && (s_wpPhaseFrames % 60) == 0) {
                        SysWork_StateSetNext(SysState_StatusMenu);
                        SH_DBG("[T25_WORLDPICKUP] requested status menu at frame %ld", s_frame);
                    }
                    if (s_wpPhaseFrames > 1200) {
                        SH_DBG("[T25_WORLDPICKUP] TIMEOUT reaching inventory; sysState=%d gameState=%d",
                               (int)g_SysWork.sysState, (int)g_GameWork.gameState);
                        s_wpPhase = 7;
                    }
                }

                /* Film every s_film-th frame across placement and pickup so the
                 * .mov shows the whole interaction, not just the model. */
                if (s_film > 0 && s_wpPhase >= 1 && s_wpPhase < 7 &&
                    s_wpFilmCount < s_wpFilmCap && (s_frame % s_film) == 0) {
                    film_capture();
                    s_wpFilmCount++;
                }
                break;
            }

            if (!s_gaveItem && (s_frame - s_ingameEnter) > 120) {
                if (s_fourClass)
                    autopilot_give_fourclass_inventory();
                else {
                    extern void Pc_ConsoleExec(const char* line);
                    Pc_ConsoleExec("GIVE HEALTHDRINK");
                }
                s_gaveItem = 1;
                SH_DBG("[AUTOPILOT] granted test inventory at frame %ld (sysState=%d)",
                       s_frame, (int)g_SysWork.sysState);
                break;
            }

            /* UNQ*.TMD examine/pickup route (SH_AUTOPILOT_UNQ=1).
             *
             * This is the ONLY path that renders ITEM/UNQ21.TMD. The inventory
             * carousel instead draws the per-map item pack out of FS_BUFFER_8
             * (Gfx_Items_Display), so it can never show a modded UNQ21.
             *
             * Sequence mirrors Event_ItemTake's own state machine:
             *   GameFs_UniqueItemModelLoad -> Fs_QueueChunksLoad -> func_80054A04
             *   -> Gfx_PickupItemAnimate every frame (which is what actually
             *   submits the model into OT0 and pauses the world behind it).
             */
            /* UNQ*.TMD load is now driven from the inventory case below, where
             * the item camera actually works. Nothing extra to do in gameplay. */

            /* Wait for the grant to settle, then request the status menu. */
            if (s_gaveItem && (s_frame - s_ingameEnter) > 240 &&
                (s_fourClass || g_SysWork.sysState == SysState_Gameplay))
            {
                static long s_lastReq = 0;
                if (s_frame - s_lastReq > 60) {
                    s_lastReq = s_frame;
                    SysWork_StateSetNext(SysState_StatusMenu);
                    SH_DBG("[AUTOPILOT] requested status menu at frame %ld", s_frame);
                }
            }
            break;
        }

        /* Item screen: select the requested slot, then photograph it.
         *
         * NOTE on which model this is: the inventory CAROUSEL draws from the
         * per-map item pack in FS_BUFFER_8 (Gfx_Items_Display / g_Item_MapLoadableItems),
         * NOT from ITEM/UNQ21.TMD. UNQ21 is the model used by the pickup /
         * examine path (GameFs_UniqueItemModelLoad -> FS_BUFFER_5 ->
         * func_80054A04 -> Gfx_PickupItemAnimate). To photograph the modded
         * 48-gon UNQ21 we therefore have to exercise that second path
         * explicitly, which is what SH_AUTOPILOT_UNQ=1 does below. */
        case GameState_InventoryScreen:
        {
            static long s_invEnter = 0;
            long        held;

            if (s_invEnter == 0) {
                s_invEnter = s_frame;
                s_inventoryTick = s_frame;
                SH_DBG("[AUTOPILOT] entered inventory at frame %ld", s_frame);
            }
            held = s_frame - s_invEnter;
            autopilot_register_fourclass_override();

            /* ================================================================
             * T26 — INVENTORY HALF OF THE WORLD-PICKUP SESSION
             *
             * This is the SAME run that just filmed the take-screen: the item
             * was picked up through the stock Event_ItemTake chain, granted by
             * Inventory_AddSpecialItem, and the status menu was then requested
             * through SysWork_StateSetNext(SysState_StatusMenu) — the same
             * transition the ITEM button drives. Nothing is re-granted and
             * nothing is spliced; the carousel draws whatever the inventory's
             * own code resolves for the item now in the savegame.
             *
             * The carousel link site (item_screens_3.c:4003) reports its own
             * provenance via [UNIFIEDITEM] carousel link source=modern|stock-pack,
             * so the log proves which geometry each view used.
             * ================================================================ */
            if (s_worldPickup) {
                if (!s_wpInvSelected && held >= 60) {
                    autopilot_select_item((u8)InvItemId_HealthDrink);
                    g_Inventory_SelectionId     = InventorySelectionId_Item;
                    g_Inventory_PrevSelectionId = InventorySelectionId_Item;
                    s_wpInvSelected = 1;
                    SH_DBG("[T25_WORLDPICKUP] inventory reached; selected health drink slot=%d",
                           autopilot_find_item_slot((u8)InvItemId_HealthDrink));
                }

                /* r108 STEP 3: advance the cursor to the next modern-linked
                 * item every 140 frames. 140 is well clear of the item
                 * screen's own scroll animation, which autopilot_select_item
                 * settles by rebuilding the seven-slot window through
                 * Gfx_Items_Draw -- the inventory's own routine, not ours. */
                if (s_wpInvSelected && held >= 200 && ((held - 200) % 140) == 0) {
                    int n = (int)(sizeof(s_wpModernRotation) / sizeof(s_wpModernRotation[0]));
                    s_wpRotIdx = (s_wpRotIdx + 1) % n;
                    s_wpRotItem = s_wpModernRotation[s_wpRotIdx];
                    autopilot_select_item(s_wpRotItem);
                    SH_DBG("[T26_ROTATE] held=%ld -> item=%u rotIdx=%d/%d",
                           held, (unsigned)s_wpRotItem, s_wpRotIdx, n);
                }

                if (s_wpInvSelected && (held % 60) == 0) {
                    int slot = autopilot_carousel_slot();
                    GsDOBJ2* obj = (slot >= 0) ? &g_Items_ItemsModelData[slot] : NULL;
                    s32 fileIdx = Pc_ItemUnq_FromItemId(s_wpRotItem);
                    int modern = (obj != NULL && obj->id == (unsigned long)fileIdx + 1u);

                    SH_DBG("[T26_INVENTORY] carousel view held=%ld source=%s item=%u file=%d slot=%d objid=%lu tmd=%s",
                           held, modern ? "modern" : "stock-pack",
                           (unsigned)s_wpRotItem, (int)fileIdx, slot,
                           obj != NULL ? obj->id : 0ul,
                           (obj != NULL && obj->tmd != NULL) ? "linked" : "null");
                }

                if (s_wpInvSelected && s_film > 0 && s_wpFilmCount < s_wpFilmCap &&
                    (s_frame % s_film) == 0) {
                    film_capture();
                    s_wpFilmCount++;
                }

                if (!s_wpInvDone && held > (60 + s_wpInvFilmFrames)) {
                    s_wpInvDone = 1;
                    s_wpPhase = 7;
                    SH_DBG("[T26_INVENTORY] sequence complete: pickup + carousel filmed in one session, film frames=%ld",
                           s_wpFilmCount);
                    milestone_signature();
                    if (g_ShDebugLog) fflush(g_ShDebugLog);
                    exit(0);
                }
                break;
            }

            /* Force the selection directly rather than walking the cursor —
             * the item screen's own scroll animation makes a keypress walk
             * timing-dependent, and this reaches the exact same globals the
             * cursor code writes. */
            if (held == 60) {
                g_Inventory_SelectionId     = InventorySelectionId_Item;
                g_Inventory_PrevSelectionId = InventorySelectionId_Item;
                g_Inventory_SelectedItemIdx = s_itemSlot;
                g_SysWork.invItemSelectedIdx = s_itemSlot;
                SH_DBG("[AUTOPILOT] forced item slot %d", s_itemSlot);
            }

            /* Move the real carousel between all four acceptance objects. The
             * item-screen state machine animates from g_Inventory_SelectedItemIdx
             * toward g_SysWork.invItemSelectedIdx; only the target is changed. */
            if (s_fourClass && !s_fourClassExamine) {
                if (held == 140) autopilot_select_item(InvItemId_SteelPipe);
                if (held == 300) autopilot_select_item(InvItemId_Handgun);
                if (held == 460) autopilot_select_item(InvItemId_HouseKey);
                if (held == 620) autopilot_select_item(InvItemId_HyperBlaster);
            }

            /* T21: one continuous four-class carousel -> examine proof, driven
             * through the REAL examine path.
             *
             * The previous (T18) driver called Gfx_PickupItemAnimate, which is
             * the WORLD PICKUP route: a different draw slot (9), a different
             * transform, and its own scale ramp. That is why examine shots came
             * out grotesquely oversized — it was never the examine screen at all.
             *
             * Stock examine is entered by InvCmdId_Look (item_screens_2.c:1090):
             * it sets SelectionBordersDraw, SelectionId = Examine, and
             * gameStateSteps[1] = 13 / [2] = 0. That is the WHOLE transition for
             * every non-paper item — no model load, no second draw call. Cases
             * 13/14/15 (item_screens_3.c:3106-3159) write no coords, no scale and
             * no rotation; they only tick D_800AE190 and ScrollTransitionTimer,
             * which drive the 2D glare/fade overlay. Gfx_ItemScreens_DrawInit
             * stays gated `gameStateSteps[1] < 21`, so the examined item is still
             * the ordinary carousel slot, drawn by the ordinary carousel loop,
             * still spinning on `rotate.vy -= 0x10` (item_screens_3.c:3167).
             *
             * So the driver only has to write what Look writes and let the stock
             * state machine run. Carousel and examine then provably share one
             * GsDOBJ2 — which is exactly the claim the gate asks us to prove. */
            if (s_fourClassExamine && held >= 140) {
                /* Exactly the two items the gate names: one ORIGINAL item that
                 * must render from the stock map pack, and one item carrying a
                 * modder-supplied replacement GLB. Four shots total.
                 *
                 * BOTH must be present in THIS map's loadableItems, or the
                 * carousel link site is never reached at all: item_screens_3.c:3999
                 * resolves obj = GsGetTMDObject(tmd_hdr, loadableItemIdx) and the
                 * whole link (stock AND modern) sits inside `if (obj != NULL)`.
                 * Measured live from [ITEMPICK] map-pack link lines, this map's
                 * pack holds exactly three: 32 (Health Drink, loadableIdx 0),
                 * 133 (Chainsaw, idx 1) and 163 (Hyper Blaster, idx 2).
                 *
                 * Steel Pipe (129) and House Key (65) are NOT in it. Earlier runs
                 * that used them produced `objid=0 tmd=null` and an empty examine
                 * box -- the item was never linked, so there was nothing to draw. */
                static const u8 itemIds[] = {
                    InvItemId_Chainsaw,
                    InvItemId_HyperBlaster
                };
                static const int itemCount = (int)(sizeof(itemIds) / sizeof(itemIds[0]));
                u8  itemId = itemIds[s_fourClassExamineIndex];
                s32 fileIdx = Pc_ItemUnq_FromItemId(itemId);

                if (s_fourClassExaminePhase == 0) {
                    /* Leave the pickup driver disarmed for the whole sequence:
                     * s_unqReady stays 0, so Pc_Autopilot_Draw returns on its
                     * first line and Gfx_PickupItemAnimate is never reached. */
                    s_unqReady = 0;
                    s_unqFrames = 0;
                    autopilot_select_item(itemId);
                    s_fourClassPhaseFrames = 0;
                    s_fourClassExaminePhase = 1;
                    SH_DBG("[T21_EXAMINE] state=carousel item=%u class=%d",
                           (unsigned)itemId, s_fourClassExamineIndex + 1);
                    if (itemId == InvItemId_Chainsaw)
                        SH_DBG("[T21_STOCK_TEXTURE] item=133 source=retail-vram no-installed-override map-pack loadableIdx=1");
                } else if (s_fourClassExaminePhase == 1) {
                    /* Hold in the carousel long enough to film a full spin, then
                     * read the LIVE draw object the carousel is submitting for
                     * this item and report its provenance. obj->id is stamped by
                     * Pc_ModernMesh_LinkObject as fileIdx+1, so this is measured
                     * from the actual GsDOBJ2, not inferred from the registry. */
                    s_fourClassPhaseFrames++;
                    if (s_fourClassPhaseFrames >= 150) {
                        int slot = autopilot_carousel_slot();
                        GsDOBJ2* obj = (slot >= 0) ? &g_Items_ItemsModelData[slot] : NULL;
                        int modern = (obj != NULL && obj->id == (unsigned long)fileIdx + 1u);

                        SH_DBG("[UNIFIEDITEM] carousel link source=%s item=%u file=%d slot=%d objid=%lu tmd=%s",
                               modern ? "modern" : "stock-pack",
                               (unsigned)itemId, (int)fileIdx, slot,
                               obj != NULL ? obj->id : 0ul,
                               (obj != NULL && obj->tmd != NULL) ? "linked" : "null");

                        /* Exactly what InvCmdId_Look writes for a non-paper item
                         * (item_screens_2.c:1090-1097). Nothing else. */
                        g_Inventory_SelectionBordersDraw = 1;
                        g_Inventory_SelectionId          = InventorySelectionId_Examine;
                        g_GameWork.gameStateSteps[1]     = 13;
                        g_GameWork.gameStateSteps[2]     = 0;

                        s_fourClassPhaseFrames = 0;
                        s_fourClassExaminePhase = 2;
                        SH_DBG("[T21_EXAMINE] look issued item=%u step1=13", (unsigned)itemId);
                    }
                } else if (s_fourClassExaminePhase == 2) {
                    /* Stock case 13 advances to 14 on its own once the transition
                     * timer saturates at 0x20 AND the fs queue is drained. Wait
                     * for the engine to make that transition — do not force it. */
                    s_fourClassPhaseFrames++;
                    if (g_GameWork.gameStateSteps[1] == 14) {
                        int slot = autopilot_carousel_slot();
                        GsDOBJ2* obj = (slot >= 0) ? &g_Items_ItemsModelData[slot] : NULL;
                        int modern = (obj != NULL && obj->id == (unsigned long)fileIdx + 1u);

                        SH_DBG("[UNIFIEDITEM] examine link source=%s item=%u file=%d slot=%d objid=%lu tmd=%s",
                               modern ? "modern" : "stock-pack",
                               (unsigned)itemId, (int)fileIdx, slot,
                               obj != NULL ? obj->id : 0ul,
                               (obj != NULL && obj->tmd != NULL) ? "linked" : "null");
                        SH_DBG("[T21_EXAMINE] state=examine item=%u class=%d step1=%d real-look-path",
                               (unsigned)itemId, s_fourClassExamineIndex + 1,
                               (int)g_GameWork.gameStateSteps[1]);
                        s_fourClassPhaseFrames = 0;
                        s_fourClassExaminePhase = 3;
                    } else if (s_fourClassPhaseFrames > 300) {
                        SH_DBG("[T21_EXAMINE] TIMEOUT waiting for step1=14 item=%u step1=%d",
                               (unsigned)itemId, (int)g_GameWork.gameStateSteps[1]);
                        s_fourClassPhaseFrames = 0;
                        s_fourClassExaminePhase = 3;
                    }
                } else if (s_fourClassExaminePhase == 3) {
                    /* Film the examine view long enough to show the item spinning
                     * under the glare overlay, then unwind through stock case 15
                     * by writing exactly what case 14 writes on an action press
                     * (item_screens_3.c:3128-3135). */
                    s_fourClassPhaseFrames++;
                    if (s_fourClassPhaseFrames >= 150) {
                        g_Inventory_ScrollTransitionTimer = 0;
                        g_Inventory_SelectionBordersDraw  = 0;
                        g_GameWork.gameStateSteps[1]      = 15;
                        g_GameWork.gameStateSteps[2]      = 0;
                        D_800AE190                        = 0;
                        s_fourClassPhaseFrames = 0;
                        s_fourClassExaminePhase = 4;
                        SH_DBG("[T21_EXAMINE] exit issued item=%u step1=15", (unsigned)itemId);
                    }
                } else if (s_fourClassExaminePhase == 4) {
                    /* Wait for case 15 to unwind back to the ordinary carousel
                     * state before selecting the next proof item. */
                    s_fourClassPhaseFrames++;
                    if (g_GameWork.gameStateSteps[1] == 1 || s_fourClassPhaseFrames > 180) {
                        s_fourClassExamineIndex++;
                        s_fourClassPhaseFrames = 0;
                        if (s_fourClassExamineIndex >= itemCount) {
                            s_fourClassExaminePhase = 5;
                            SH_DBG("[T21_EXAMINE] sequence complete items=%d shots=%d path=real-look",
                                   itemCount, itemCount * 2);
                        } else {
                            s_fourClassExaminePhase = 0;
                        }
                    }
                }

                /* One capture counter across the whole sequence. Every frame of
                 * both the carousel and the examine view is filmed from here —
                 * there is no second draw hook any more, because there is no
                 * second draw path. */
                if (s_fourClassExaminePhase != 5 &&
                    s_film > 0 && (s_frame % s_film) == 0)
                    film_capture();
            }

            /* Kick the unique-item (UNQ*.TMD / FS_BUFFER_5) load so the
             * oversized loose-file path in pc_big_tmd.c is actually exercised
             * for the selected item, then splice the resulting model into the
             * carousel's centre slot so it is drawn by the inventory's own
             * item camera. Off unless SH_AUTOPILOT_UNQ is set.
             *
             * Why the splice: the inventory carousel normally draws the
             * per-map item pack from FS_BUFFER_8 (Gfx_Items_Display), NOT
             * ITEM/UNQ21.TMD. UNQ21 is only used by the pickup/examine path,
             * whose camera (ItemScreen_CamSet) is overwritten every frame by
             * the world camera in a driven session. The inventory screen's
             * camera works, so link the UNQ21 object into the slot the
             * inventory is already drawing. */
            if (s_unqLoad && !s_fourClassExamine && held == 100) {
                extern void GameFs_UniqueItemModelLoad(u8 itemId);
                GameFs_UniqueItemModelLoad((u8)s_unqItemId);
                SH_DBG("[AUTOPILOT] GameFs_UniqueItemModelLoad(item=%d) issued", s_unqItemId);
            }
            if (s_unqLoad && !s_fourClassExamine && held > 130) {
                extern void  func_80054A04(u8 itemId);
                extern void* Pc_BigTmd_Resolve(void* nativeDest);

                if (held == 140) {
                    func_80054A04((u8)s_unqItemId);
                    SH_DBG("[AUTOPILOT] func_80054A04(item=%d) issued", s_unqItemId);
                }

                /* Re-splice every frame: Gfx_Items_DrawInit rebuilds the
                 * carousel slots (and NULLs them) on its own schedule. */
                if (held > 140) {
                    s_TmdFile* tf = (s_TmdFile*)Pc_BigTmd_Resolve(FS_BUFFER_5);
                    if (tf != NULL) {
                        unsigned long*     hdr = (unsigned long*)&tf->flags;
                        struct TMD_STRUCT* obj;
                        GsMapModelingData(hdr);
                        obj = GsGetTMDObject(hdr, 0);
                        if (obj != NULL) {
                            /* Slot 3 is the centre of the 7-wide carousel. */
                            GsLinkObject4_PC(obj, &g_Items_ItemsModelData[3]);
                            g_Items_ItemsModelData[3].coord2 = &g_Items_Coords[3];
                            if (!s_splicedLogged) {
                                s_splicedLogged = 1;
                                s_uniqueLinkTick = s_frame;
                                SH_DBG("[AUTOPILOT] spliced UNQ TMD (id=0x%x nobj=%u) into carousel slot 3",
                                       (unsigned)tf->id, (unsigned)tf->modelCount);
                            }
                        }
                    }
                }
            }

            /* Film mode: dump a contiguous run of frames so the carousel's own
             * per-frame spin (item_screens_3.c:2423, rotate.vy += 0.75deg)
             * becomes a real motion capture instead of 5 disjoint stills. */
            if (!s_fourClassExamine && s_film > 0 && held >= s_filmFrom && held <= s_filmTo &&
                ((held - s_filmFrom) % s_film) == 0)
            {
                film_capture();
            }

            if (held == 90)  capture("inv_a");
            if (held == 150) capture("inv_b");
            if (held == 210) capture("inv_c");
            if (held == 280) capture("inv_d");
            if (held == 340) capture("inv_e");
            break;
        }

        default:
            break;
    }

    if (s_quitFrame > 0 && s_frame >= s_quitFrame) {
        milestone_signature();
        SH_DBG("[AUTOPILOT] quit frame %ld reached", s_frame);
        if (g_ShDebugLog) fflush(g_ShDebugLog);
        exit(0);
    }
}

/* Draw-time half of the harness. Called from MainLoop immediately after
 * GsSortClear (game_main.c), inside #ifdef SH_PC_PORT, and a total no-op unless
 * SH_AUTOPILOT is set.
 *
 * Gfx_PickupItemAnimate is what actually submits the UNQ*.TMD model into OT0
 * (and re-arms the world-pause + g_PcPickupItemActive depth bracket each
 * frame). Calling it from the input tick is too early — GsSortClear later in
 * the same frame wipes the OT — so it has to run from here. */
void Pc_Autopilot_Draw(void)
{
    extern bool Gfx_PickupItemAnimate(u8 itemId);
    extern void func_80054A04(u8 itemId);
    extern void func_8004BFE8(void);
    extern void func_8004C040(void);

    if (s_enabled != 1 || !s_unqLoad || !s_unqReady)
        return;

    s_unqFrames++;

    /* ITEM-SCREEN PROJECTION BRACKET.
     *
     * Stock draws the examine model from Gfx_Items_Draw (item_screens_3.c:3749),
     * which opens with func_8004BFE8() -> PushMatrix + GsSetProjection(1000) and
     * closes with func_8004C040() -> PopMatrix + GsSetProjection(D_800C3954).
     * The item-preview FOV lives entirely in that bracket.
     *
     * This driver submits the same model from the post-GsSortClear hook, which
     * is OUTSIDE the bracket. Without it the item is projected with whatever
     * projection the world camera left set, so apparent zoom is wrong for every
     * item -- stock meshes included -- and the unbalanced PushMatrix in the
     * stock path leaks a matrix-stack entry per frame, which is what stacks up
     * into overlapping/garbled draws. Open it here and close it after the
     * submission so the harness matches the stock projection state exactly. */
    func_8004BFE8();

    /* Re-run the item setup every frame. func_80054A04 re-links the TMD
     * (through Pc_BigTmd_Resolve, so it parses the redirected oversized buffer)
     * AND re-applies the item camera via ItemScreen_CamSet. Once is not enough
     * in this driven scenario: the world camera update runs every frame and
     * otherwise leaves the item model outside the view frustum. */
    func_80054A04((u8)s_unqItemId);
    if (s_fourClassExamine)
        g_Items_Transforms[9].rotate.vy = (s16)(s_unqFrames * Q12_ANGLE(1.5f));

    /* Gfx_PickupItemAnimate grows the item scale from g_DeltaTimeRaw through its
     * own anim-state machine, which is normally driven by Event_ItemTake. We
     * call the animate function directly, so force the transform to full scale
     * here — otherwise item_screens_cam.c's guard rejects the draw with
     * "draw SKIPPED: reason=scale 0" and the model is silently invisible. */
    {
        extern q19_12 g_Items_PickupScale;
        extern s32    g_Items_PickupAnimState;
        g_Items_PickupScale     = Q12(0.5f);
        g_Items_PickupAnimState = 2;
    }
    g_Items_Transforms[9].scale.vx = Q12(1.0f);
    g_Items_Transforms[9].scale.vy = Q12(1.0f);
    g_Items_Transforms[9].scale.vz = Q12(1.0f);

    Gfx_PickupItemAnimate((u8)s_unqItemId);

    /* Re-assert after the animate call: its state-0 branch can write a scale
     * derived from the pickup scale >> 11, which is still tiny early on. */
    g_Items_Transforms[9].scale.vx = Q12(1.0f);
    g_Items_Transforms[9].scale.vy = Q12(1.0f);
    g_Items_Transforms[9].scale.vz = Q12(1.0f);

    if (s_fourClassExamine && s_film > 0 && (s_unqFrames % s_film) == 0)
        film_capture();

    /* Close the bracket opened above: PopMatrix + restore the saved projection.
     * Must run on every path that reached func_8004BFE8(), or the matrix stack
     * and GTE projection drift frame over frame. */
    func_8004C040();

    if ((s_unqFrames % 60) == 0) {
        SH_DBG("[AUTOPILOT] unq draw f=%ld scale=%d coordT=(%d,%d,%d)",
               s_unqFrames, (int)g_Items_Transforms[9].scale.vx,
               (int)g_Items_Coords[9].coord.t[0],
               (int)g_Items_Coords[9].coord.t[1],
               (int)g_Items_Coords[9].coord.t[2]);
    }

    if (s_unqFrames == 30)  capture("unq_a");
    if (s_unqFrames == 90)  capture("unq_b");
    if (s_unqFrames == 150) capture("unq_c");
    if (s_unqFrames == 210) capture("unq_d");
}
