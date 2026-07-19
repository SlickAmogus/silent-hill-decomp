#include <SDL2/SDL.h>
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/player.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/math/math.h"
#include "bodyprog/collision/ray.h"
#include "pc_combat.h"
#include "pc_config.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/sound/sound_system.h"

extern const unsigned char* g_sdlKeyboardState;
extern int PsyX_RawControllerButtonHeld(int sdlGameControllerButton);
extern int                  g_PcAimDevice; /* 0 = mouse, 1 = controller (game_main.c) */

/* Player bullet "fat hitbox" — single source of truth shared with ray.c
 * (func_8006EE0C) and bodyprog_combat_8008A058.c (func_8008A3E0). The enemy
 * bullet-collision cylinder ([box.top..box.height] radius cylinder.field_2) is
 * only a narrow neck/upper-torso band, so only high shots registered. While the
 * PLAYER's gun bullet is traced (g_PcBulletHitActive, set around the trace in
 * func_8008A3E0), func_8006EE0C inflates that cylinder by these radius-relative
 * amounts so the whole VISIBLE body is hittable — for BOTH free-aim and the
 * classic auto-aim. The damage ray hits at the aimed height, so blood/impact
 * lands where shot. Aim-assist clamps its aim point to the SAME expanded span.
 * Both default-tunable; expand-Y is each side of the band. */
s32    g_PcBulletHitActive = 0;
q19_12 g_PcBulletVertMul   = Q12(2.0f); /* vertical reach added each side = 2x collision radius */
q19_12 g_PcBulletRadMul    = Q12(1.0f); /* horizontal radius added = 1x collision radius */

/* PC-only: one-frame request that a Quick Turn (animated 180) start this frame.
 * Set by Pc_ExtraActionsUpdate on the bind edge; consumed + cleared by
 * Player_LogicUpdate (native state machine or the movement shim). */
int g_PcQuickTurnRequest = 0;

/* PC-only: 1 while the Rear Look bind is HELD in a TPS/OTS camera. Set every
 * frame by Pc_RearLookUpdate; consumed by Pc_TpsCamera_Apply (orbit +180) and
 * the head-look override in Player_Update. */
int g_PcRearLookActive = 0;

/* PC-only: Quick Heal green screen pulse — Q12 seconds remaining. Counted down and
 * drawn each frame by Pc_HealFlashUpdate (hooked in game_main.c). 0 = no flash. */
s32 g_PcHealFlashTimer = 0;

/* Monotonic per-frame counter for the edge caches below. Bumped once per frame
 * by Pc_ExtraActionsUpdate. This must NOT be g_VBlanks: that is a per-frame
 * vblank DELTA (game_main.c, clamped MIN(...,V_BLANKS_MAX=4)), not a counter,
 * so at a steady frame rate it holds the same small value every frame and the
 * "resample once per frame" test below never fires again after slot creation.
 * The cached edge then freezes — stuck false (bind does nothing) or stuck true
 * (reload re-triggers every frame with no press), which is the reported
 * accidental/double reload. */
s32 g_PcInputFrame = 0;

/* Returns true on the frame `sdlScancode` transitions 0→1.
 *
 * Frame-stable: prev-state is sampled at most once per frame, so multiple
 * callers in the same frame all see the same rising-edge result. Without
 * this, a press that opens the inventory in gameplay state would also fire
 * a "rising edge" again the first time the inventory state queries it
 * (since each fresh slot starts with prev=0), instantly closing it. */
bool PC_KeyboardKeyClicked(int sdlScancode)
{
    #define PC_KEY_CACHE_SIZE 16 /* both schemes' reload/reload2/cycle/heal/quick-turn keys share this never-evicting cache */
    static int  s_keys[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_prev[PC_KEY_CACHE_SIZE]   = {0};
    static bool s_edge[PC_KEY_CACHE_SIZE]   = {0};
    static s32  s_frame[PC_KEY_CACHE_SIZE]  = {0};
    static int  s_count                     = 0;
    static bool s_initFrames                = false;

    if (!g_sdlKeyboardState) return false;

    int slot = -1;
    for (int i = 0; i < s_count; i++) {
        if (s_keys[i] == sdlScancode) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_count >= PC_KEY_CACHE_SIZE) return false;
        slot = s_count++;
        s_keys[slot]  = sdlScancode;
        /* Seed prev with current held state — if the key is already down
         * when first queried, that's NOT a rising edge. */
        s_prev[slot]  = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot]  = false;
        s_frame[slot] = g_PcInputFrame;
        return false;
    }

    /* Resample only once per frame (per slot). Other call sites in the
     * same frame get the cached edge result. */
    if (s_frame[slot] != g_PcInputFrame) {
        bool nowHeld = g_sdlKeyboardState[sdlScancode] != 0;
        s_edge[slot] = nowHeld && !s_prev[slot];
        s_prev[slot] = nowHeld;
        s_frame[slot] = g_PcInputFrame;
    }
    return s_edge[slot];
}

/* Frame-stable rising edge of a PHYSICAL controller button (SDL game-controller
 * button index). Mirrors PC_KeyboardKeyClicked: prev-state is sampled at most once
 * per frame so multiple callers in one frame see the same edge. Reads the physical
 * controller (not the kb-merged pad), so a keyboard key on the same PSX bit can't
 * trigger a controller-only action. sdlButton < 0 (unbound) never fires. */
bool PC_RawControllerButtonClicked(int sdlButton)
{
    #define PC_PAD_CACHE_SIZE 16 /* both schemes' reload/cycle/heal/quick-turn buttons share this never-evicting cache */
    static int  s_btn[PC_PAD_CACHE_SIZE]    = {0};
    static bool s_prevP[PC_PAD_CACHE_SIZE]  = {0};
    static bool s_edgeP[PC_PAD_CACHE_SIZE]  = {0};
    static s32  s_frameP[PC_PAD_CACHE_SIZE] = {0};
    static int  s_countP                    = 0;

    if (sdlButton < 0) return false;

    int slot = -1;
    for (int i = 0; i < s_countP; i++) {
        if (s_btn[i] == sdlButton) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_countP >= PC_PAD_CACHE_SIZE) return false;
        slot = s_countP++;
        s_btn[slot]    = sdlButton;
        s_prevP[slot]  = PsyX_RawControllerButtonHeld(sdlButton) != 0;
        s_edgeP[slot]  = false;
        s_frameP[slot] = g_PcInputFrame;
        return false;
    }
    if (s_frameP[slot] != g_PcInputFrame) {
        bool nowHeld = PsyX_RawControllerButtonHeld(sdlButton) != 0;
        s_edgeP[slot] = nowHeld && !s_prevP[slot];
        s_prevP[slot] = nowHeld;
        s_frameP[slot] = g_PcInputFrame;
    }
    return s_edgeP[slot];
}

/* Returns true on the rising edge of the manual-reload bind while a gun weapon is
 * equipped with reserve ammo available. Bound outside the PSX controller mapping so
 * every PSX button keeps its original semantics; now configurable on BOTH keyboard
 * (key_reload, default R) and controller (pad_reload, default unbound). The PSX game
 * had no manual reload — it fired automatically on empty; PC adds this convenience. */
static SDL_Scancode s_kbReload[2]  = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
static SDL_Scancode s_kbReload2[2] = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN }; /* keyboard secondary */
static int          s_padReload[2] = { -2, -2 }; /* [scheme]; -2 = unresolved */

static void Pc_ReloadBindsResolve(void)
{
    const ControlScheme* sc[2];
    int i;
    if (s_padReload[0] != -2) return;
    sc[0] = &g_PcConfig.classic;
    sc[1] = &g_PcConfig.altcam;
    for (i = 0; i < 2; i++) {
        s_kbReload[i]  = SDL_GetScancodeFromName(sc[i]->keyReload);
        s_kbReload2[i] = SDL_GetScancodeFromName(sc[i]->keyReload2);
        s_padReload[i] = (sc[i]->padReload[0] != '\0')
                          ? (int)SDL_GameControllerGetButtonFromString(sc[i]->padReload)
                          : SDL_CONTROLLER_BUTTON_INVALID;
    }
}

/* Weapon/ammo conditions for a manual reload, evaluated at PRESS time only. */
static bool Pc_ManualReloadConditions(void)
{
    extern u8 g_Items_GunsMaxLoadAmmo[36]; /* clip capacity by weaponAttack (item_screens_3.c) */
    return g_SysWork.playerCombat.weaponAttack >= WEAPON_ATTACK(EquippedWeaponId_Handgun, AttackInputType_Tap) &&
           g_SysWork.playerCombat.totalWeaponAmmo != 0 &&
           INV_ITEM_GROUP(g_SavegamePtr->equippedWeapon) == InvItemGroup_GunWeapons &&
           /* Don't reload an already-full clip (PSX only auto-reloaded on empty):
            * without this a press on a full clip plays a pointless reload+SFX for
            * zero rounds, and a stray 2nd controller edge right after a reload tops
            * the clip re-triggers a second reload — the "double up". Same index the
            * reload itself uses: Items_AmmoReloadCalculation(gunIdx = weaponAttack). */
           g_SysWork.playerCombat.currentWeaponAmmo <
               g_Items_GunsMaxLoadAmmo[g_SysWork.playerCombat.weaponAttack] &&
           /* Not already mid-reload — rejecting the press outright (rather than
            * latching it) is what stops a second press queueing a double reload. */
           g_SysWork.playerWork.extra.upperBodyState != PlayerUpperBodyState_Reload;
}

/* Latched manual-reload request, sampled every frame in Pc_ExtraActionsUpdate and
 * consumed exactly once where the reload actually starts.
 *
 * The bind used to be sampled only from the two aim-gated call sites, so its
 * prev-state went stale whenever the gun was lowered: holding the key while
 * walking and then aiming produced a rising edge that never happened (phantom
 * reload on aim-entry), and releasing it mid-reload swallowed the next press.
 * Sampling unconditionally keeps prev current; the short latch carries a press
 * that lands on a frame the FSM can't act on (recoil, the dead frame after a
 * reload completes) instead of destroying it. */
int g_PcReloadRequest = 0;
static q19_12 s_reloadLatchT = 0;

/* Kept as the FSM-facing query — now just reads the latch. */
bool PC_PlayerManualReloadRequested(void)
{
    return g_PcReloadRequest != 0;
}

static void Pc_ManualReloadSample(int sch)
{
    int e1, e2, e3;

    Pc_ReloadBindsResolve();

    /* Evaluate all three into separate ints: `||` would short-circuit and skip
     * sampling the secondary/pad slots, leaving their prev-state stale. */
    e1 = (s_kbReload[sch]  != SDL_SCANCODE_UNKNOWN) && PC_KeyboardKeyClicked(s_kbReload[sch]);
    e2 = (s_kbReload2[sch] != SDL_SCANCODE_UNKNOWN) && PC_KeyboardKeyClicked(s_kbReload2[sch]);
    e3 = (s_padReload[sch] >= 0) && PC_RawControllerButtonClicked(s_padReload[sch]);

    if ((e1 || e2 || e3) && Pc_ManualReloadConditions()) {
        g_PcReloadRequest = 1;
        s_reloadLatchT    = Q12(0.30f);
    } else if (g_PcReloadRequest) {
        /* Keep the window short — a stale press resurfacing later IS the
         * spurious-reload symptom this is meant to remove. */
        s_reloadLatchT -= g_DeltaTime;
        if (s_reloadLatchT <= 0) g_PcReloadRequest = 0;
    }
}

/* ---- Cycle Weapons + Quick Heal (bound, dispatched once per frame) ---------- */

extern void GameFs_WeaponInfoUpdate(void); /* player.h — loads the equipped weapon model/anim */

static s32 Pc_FindItemSlot(u8 id)
{
    s32 i;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
        if (g_SavegamePtr->items[i].id_0 == id) return i;
    return NO_VALUE;
}

/* Safe to swap weapon / heal outside the menu: in gameplay, control enabled, and
 * the upper body idle/moving (not aiming/mid-swing/reloading — GameFs_WeaponInfoUpdate
 * does a blocking model load, so it must not fire mid-combat). */
static int Pc_ActionSafe(void)
{
    return g_GameWork.gameState == GameState_InGame &&
           g_SysWork.sysState   == SysState_Gameplay &&
           !g_Player_DisableControl &&
           g_SysWork.playerWork.extra.upperBodyState <= PlayerUpperBodyState_SidestepRightStumble;
}

/* Equip one owned weapon — mirrors the load-time re-equip block
 * (player_control.c:10312-10343) + the model reload. */
static void Pc_EquipWeapon(u8 invItemId, s32 slot)
{
    s32 groupId = INV_ITEM_GROUP(invItemId);
    g_Inventory_EquippedItem                  = invItemId;
    g_SavegamePtr->equippedWeapon             = invItemId;
    g_SysWork.playerCombat.weaponAttack       = invItemId + InvItemId_KitchenKnife; /* +0x80; s8 truncates to EquippedWeaponId */
    g_SysWork.playerCombat.weaponInventoryIdx = slot;
    g_SysWork.playerCombat.currentWeaponAmmo  = g_SavegamePtr->items[slot].count_1;
    if (groupId == InvItemGroup_GunWeapons) {
        s32 a = Pc_FindItemSlot(invItemId + InvItemId_HealthDrink); /* ammo id = weapon + 32 */
        g_SysWork.playerCombat.totalWeaponAmmo = (a == NO_VALUE) ? 0 : (s8)g_SavegamePtr->items[a].count_1;
    } else {
        g_SysWork.playerCombat.totalWeaponAmmo = 0;
    }
    GameFs_WeaponInfoUpdate();

    /* Swap the VISIBLE held-weapon model too — GameFs_WeaponInfoUpdate only reloads
     * the info/anim/stat tables, NOT the geometry Harry holds (g_WorldGfxWork.heldItem).
     * Mirror the inventory-exit sequence: reattach the grip bones for the new weapon
     * class, point heldItem->itemId at the new weapon + queue its async model/texture
     * read, and un-hide it. WorldGfx_HeldItemDraw polls the async load every frame and
     * binds the new model when the read lands (a few frames later; no blocking wait, so
     * no stutter — a brief empty hand until it binds). */
    Gfx_PlayerHeldItemAttach(g_SysWork.playerCombat.weaponAttack);
    WorldGfx_PlayerPrevHeldItem(&g_SysWork.playerCombat);
    func_8003D01C();
}

/* Cycle to the next OWNED weapon in acquisition/enum order (wraps). */
void Pc_CycleWeapons(void)
{
    static const u8 order[] = {
        InvItemId_KitchenKnife, InvItemId_SteelPipe, InvItemId_RockDrill,
        InvItemId_Hammer, InvItemId_Chainsaw, InvItemId_Katana, InvItemId_Axe,
        InvItemId_Handgun, InvItemId_HuntingRifle, InvItemId_Shotgun, InvItemId_HyperBlaster,
    };
    s32 n = (s32)(sizeof(order) / sizeof(order[0]));
    s32 cur = -1, i, k;
    for (i = 0; i < n; i++)
        if (order[i] == g_SavegamePtr->equippedWeapon) { cur = i; break; }
    for (k = 1; k <= n; k++) {
        s32 idx = cur + k;
        idx %= n; if (idx < 0) idx += n;
        s32 slot = Pc_FindItemSlot(order[idx]);
        if (slot != NO_VALUE) { Pc_EquipWeapon(order[idx], slot); return; }
    }
}

/* Auto-use the most sensible OWNED healing item for the current health. */
void Pc_QuickHeal(void)
{
    q19_12 health  = g_SysWork.playerWork.player.health;
    s32    hp      = health >> Q12_SHIFT;
    s32    deficit = 100 - hp;
    s32    drink, kit, amp;
    u8     chosen  = InvItemId_Empty;

    if (hp <= 0 || hp >= 100) return; /* dead or already full */

    drink = Pc_FindItemSlot(InvItemId_HealthDrink);
    kit   = Pc_FindItemSlot(InvItemId_FirstAidKit);
    amp   = Pc_FindItemSlot(InvItemId_Ampoule);

    if (hp < 10) {
        /* critical: strongest available (ampoule also refills the regen buffer) */
        if      (amp   != NO_VALUE) chosen = InvItemId_Ampoule;
        else if (kit   != NO_VALUE) chosen = InvItemId_FirstAidKit;
        else if (drink != NO_VALUE) chosen = InvItemId_HealthDrink;
    } else if (deficit <= 40) {
        /* light: a drink fills it with no waste; reserve stronger items */
        if      (drink != NO_VALUE) chosen = InvItemId_HealthDrink;
        else if (kit   != NO_VALUE) chosen = InvItemId_FirstAidKit;
        else if (amp   != NO_VALUE) chosen = InvItemId_Ampoule;
    } else {
        /* moderate: first-aid is the tight fit; keep the ampoule for emergencies */
        if      (kit   != NO_VALUE) chosen = InvItemId_FirstAidKit;
        else if (drink != NO_VALUE) chosen = InvItemId_HealthDrink;
        else if (amp   != NO_VALUE) chosen = InvItemId_Ampoule;
    }
    if (chosen == InvItemId_Empty) return; /* nothing owned */

    switch (chosen) {
        case InvItemId_FirstAidKit: health += Q12(80.0f);  break;
        case InvItemId_HealthDrink: health += Q12(40.0f);  break;
        case InvItemId_Ampoule:     health += Q12(100.0f); g_SavegamePtr->healthSaturation = Q12(300.0f); break;
    }
    g_SysWork.playerWork.player.health = CLAMP(health, Q12(0.0f), Q12(100.0f));
    Sd_PlaySfx(Sfx_Unk1325, -0x40, 0x40); /* same feedback SFX as the inventory heal */
    Player_ItemRemove(chosen, 1);
    g_PcHealFlashTimer = Q12(0.35f); /* brief green heal pulse (drawn by Pc_HealFlashUpdate) */
}

/* Per-frame draw for the Quick Heal green pulse: an additive full-screen green TILE
 * that eases out over ~0.35s. Mirrors the screen-fade full-screen-tile path (static
 * double-buffered prims into OT2 bucket 4). Called from game_main.c after the fade
 * update. Self-gates on the timer, so it is byte-identical output when not healing. */
void Pc_HealFlashUpdate(void)
{
    static TILE     s_tile[2];
    static DR_TPAGE s_tp[2];
    int buf;
    s32 g;

    if (g_PcHealFlashTimer <= 0)
        return;

    g_PcHealFlashTimer -= g_DeltaTime; /* fps-independent countdown (Q12 seconds) */
    if (g_PcHealFlashTimer < 0)
        g_PcHealFlashTimer = 0;

    buf = g_ActiveBufferIdx;
    g   = (g_PcHealFlashTimer * 96) / Q12(0.35f); /* peak additive green ~96, eases to 0 */

    setTile(&s_tile[buf]);
    setSemiTrans(&s_tile[buf], 1);
    setRGB0(&s_tile[buf], 0, (u8)g, 0);
    setWH(&s_tile[buf], SCREEN_WIDTH * 4, SCREEN_HEIGHT * 2);
    setXY0(&s_tile[buf], -SCREEN_WIDTH, -SCREEN_HEIGHT); /* cover the full Hor+ width */

    setDrawTPage(&s_tp[buf], 0, 1, getTPageN(0, 1, 0, 0)); /* abr=1 = additive */

    AddPrim(&g_OtTags0[buf][4], &s_tile[buf]);
    AddPrim(&g_OtTags0[buf][4], &s_tp[buf]);
}

/* Per-frame dispatch for the bound Cycle Weapons + Quick Heal actions (reload is
 * pulled by the combat FSM via PC_PlayerManualReloadRequested). Keyboard + physical
 * controller, edge-detected; gated to safe gameplay. Called from game_main.c. */
void Pc_ExtraActionsUpdate(void)
{
    static SDL_Scancode s_kbCycle[2] = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
    static SDL_Scancode s_kbHeal[2]  = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
    static SDL_Scancode s_kbQt[2]    = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
    static int          s_padCycle[2] = { -2, -2 }, s_padHeal[2] = { -2, -2 };
    static int          s_padQt[2] = { -2, -2 };
    extern int          g_DebugThirdPersonCam;
    int cycleClicked, healClicked, qtClicked, sch;

    g_PcInputFrame++; /* frame identity for the edge caches — see g_PcInputFrame */

    if (s_padCycle[0] == -2) {
        const ControlScheme* sc[2];
        int i;
        sc[0] = &g_PcConfig.classic;
        sc[1] = &g_PcConfig.altcam;
        for (i = 0; i < 2; i++) {
            s_kbCycle[i]  = SDL_GetScancodeFromName(sc[i]->keyCycleWeapons);
            s_kbHeal[i]   = SDL_GetScancodeFromName(sc[i]->keyQuickHeal);
            s_kbQt[i]     = SDL_GetScancodeFromName(sc[i]->keyQuickTurn);
            s_padCycle[i] = (sc[i]->padCycleWeapons[0] != '\0') ? (int)SDL_GameControllerGetButtonFromString(sc[i]->padCycleWeapons) : SDL_CONTROLLER_BUTTON_INVALID;
            s_padHeal[i]  = (sc[i]->padQuickHeal[0]    != '\0') ? (int)SDL_GameControllerGetButtonFromString(sc[i]->padQuickHeal)    : SDL_CONTROLLER_BUTTON_INVALID;
            s_padQt[i]    = (sc[i]->padQuickTurn[0]    != '\0') ? (int)SDL_GameControllerGetButtonFromString(sc[i]->padQuickTurn)    : SDL_CONTROLLER_BUTTON_INVALID;
        }
    }
    sch = g_DebugThirdPersonCam ? 1 : 0;

    /* Sample the edges every frame (keeps prev-state current), act only in gameplay. */
    cycleClicked = (s_kbCycle[sch] != SDL_SCANCODE_UNKNOWN && PC_KeyboardKeyClicked(s_kbCycle[sch])) ||
                   (s_padCycle[sch] >= 0 && PC_RawControllerButtonClicked(s_padCycle[sch]));
    healClicked  = (s_kbHeal[sch]  != SDL_SCANCODE_UNKNOWN && PC_KeyboardKeyClicked(s_kbHeal[sch]))  ||
                   (s_padHeal[sch]  >= 0 && PC_RawControllerButtonClicked(s_padHeal[sch]));
    qtClicked    = (s_kbQt[sch]    != SDL_SCANCODE_UNKNOWN && PC_KeyboardKeyClicked(s_kbQt[sch]))    ||
                   (s_padQt[sch]    >= 0 && PC_RawControllerButtonClicked(s_padQt[sch]));

    Pc_ManualReloadSample(sch);

    if (!Pc_ActionSafe())
    {
        g_PcQuickTurnRequest = 0; /* not safe gameplay -> drop any pending turn */
        g_PcReloadRequest    = 0; /* ditto any pending reload */
        s_reloadLatchT       = 0;
        return;
    }
    if (cycleClicked) Pc_CycleWeapons();
    if (healClicked)  Pc_QuickHeal();
    /* Quick Turn: (re)assign the one-frame request every safe frame — never latch —
     * so a press the current player sub-state ignores (e.g. the AFK look-around idle)
     * can't queue a delayed 180 on the next movement input. Consumed + cleared in
     * Player_LogicUpdate (native state entry or shim latch). */
    g_PcQuickTurnRequest = qtClicked ? 1 : 0;
}

/* Per-frame HELD read for Rear Look: sets g_PcRearLookActive while the bind is
 * held during TPS/OTS gameplay (never FPS or classic). Consumed by the camera
 * (orbit +180) and the head-look override. Called from game_main.c. */
void Pc_RearLookUpdate(void)
{
    static SDL_Scancode s_kb[2]  = { SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN };
    static int          s_pad[2] = { -2, -2 };
    extern int          g_DebugThirdPersonCam;
    extern int          g_PcFpsCam;
    extern int          g_PcConsoleInputActive;
    const Uint8*        keys;
    int                 sch, held;

    if (s_pad[0] == -2) {
        const ControlScheme* sc[2];
        int i;
        sc[0] = &g_PcConfig.classic;
        sc[1] = &g_PcConfig.altcam;
        for (i = 0; i < 2; i++) {
            s_kb[i]  = SDL_GetScancodeFromName(sc[i]->keyRearLook);
            s_pad[i] = (sc[i]->padRearLook[0] != '\0') ? (int)SDL_GameControllerGetButtonFromString(sc[i]->padRearLook) : SDL_CONTROLLER_BUTTON_INVALID;
        }
    }
    sch  = g_DebugThirdPersonCam ? 1 : 0;
    keys = SDL_GetKeyboardState(NULL);
    held = ((keys && s_kb[sch] != SDL_SCANCODE_UNKNOWN && keys[s_kb[sch]]) ||
            (s_pad[sch] >= 0 && PsyX_RawControllerButtonHeld(s_pad[sch])));

    /* TPS/OTS only (not FPS, not classic), gameplay only; cleared otherwise so it
     * can never latch on. */
    g_PcRearLookActive = (held && g_DebugThirdPersonCam && !g_PcFpsCam &&
                          g_GameWork.gameState == GameState_InGame &&
                          g_SysWork.sysState   == SysState_Gameplay &&
                          !g_PcConsoleInputActive) ? 1 : 0;
}

/* OTS/TPS free-aim aim assist.
 *
 * Free-aim casts a single screen-center bullet ray; an enemy only takes a hit
 * when that ray threads its tight char-collision cylinder AND the separate
 * hand->target damage ray (cast from Harry's hand, offset from the camera eye)
 * threads that same cylinder. The parallax between the two origins meant you had
 * to land the reticle on a narrow strip near the enemy's central axis to score a
 * hit — the "tiny hitbox" feel.
 *
 * Fix: when the reticle is over an enemy's body (mouse) or near it (controller
 * auto-aim), return a point ON the enemy's vertical axis at the aimed height.
 * Pointing the gun at the axis guarantees the hand->target damage ray threads
 * the cylinder, so a hit registers anywhere on the body. The body is modeled as
 * a vertical capsule [box.top..box.height] of radius cylinder.field_2 — the same
 * extents the engine's character trace (func_8006EE0C box path) uses for bullets.
 *
 * camFwd is the unit (Q12) view forward; the ray is camPos + camFwd*t. Returns
 * the chosen NPC index in g_SysWork.npcs[] (or NO_VALUE), writing the world-space
 * aim point to *outAimPoint on success. Nearest qualifying enemy wins.
 */
#define AA_MOUSE_RADIUS_MUL  Q12(2.5f)  /* mouse: hittable radius = 1.8x collision radius (covers visible body) */
#define AA_CTRL_RADIUS_MUL   Q12(5.0f)  /* controller: close-range floor radius */
#define AA_CTRL_CONE_TAN     Q12(0.20f) /* controller: ~9deg magnetic auto-aim cone, scaled by distance */
#define AA_VERT_REACH_MUL    Q12(8.0f)  /* vertical window reach = 4x radius (the box collision span is a narrow neck/torso band, far shorter than the visible body) */
/* Absolute floor on the mouse activation window: several enemies animate their
 * collision radius down to ~0.05m (bloodsucker, twinfeeler, larval stalker),
 * making 2.5x radius a few pixels wide — reticle visibly on the body but no
 * snap, and the unsnapped hand-ray misses the tight cylinder by OTS parallax
 * (the "crosshair on them but no hit" report). 0.35m of reticle slop cannot
 * create false hits: the snap aims at the collision axis, so the bullet still
 * has to thread the real (inflated) cylinder. */
#define AA_MOUSE_RADIUS_FLOOR Q12(0.35f)

s32 Pc_AimAssistFind(const VECTOR3* camPos, const VECTOR3* camFwd, s32 aimRange, VECTOR3* outAimPoint)
{
    s32     bestIdx  = NO_VALUE;
    s32     bestK    = 0x7FFFFFFF;
    s32     bestPerp = 0x7FFFFFFF;
    VECTOR3 bestPt   = { 0, 0, 0 };
    int     isPad    = (g_PcAimDevice == 1);
    s64     fxz2;
    s32     i;

    /* |camFwd_xz|^2 (Q24). Camera pitch is clamped well short of vertical, so
     * this is never near zero, but guard the actual divisor (fxz2 >> 12) so a
     * near-vertical chest-anchored forward can't trigger an integer div-by-zero
     * (x86 idiv SIGFPE) below. */
    fxz2 = (s64)camFwd->vx * camFwd->vx + (s64)camFwd->vz * camFwd->vz;
    if ((fxz2 >> 12) <= 0)
        return NO_VALUE;

    for (i = 0; i < (s32)ARRAY_SIZE(g_SysWork.npcs); i++)
    {
        s_SubCharacter* npc = &g_SysWork.npcs[i];
        s32 cx, cz, dx, dz, radius;
        s32 yA, yB, yLo, yHi, vSlack, rayY, bodyH;
        s32 kmult, qx, qz, perpH, maxPerp;

        if (npc->model.charaId < Chara_AirScreamer || npc->model.charaId >= Chara_LockerDeadBody)
            continue; /* only damageable enemies (mirrors func_8008A3E0) */
        if (npc->collision.state == CharaCollisionState_Ignore)
            continue; /* skip only state 0, like the bullet trace itself — downed-
                       * but-alive enemies (state 1) still deserve the snap */
        if (npc->health <= Q12(0.0f))
            continue;

        radius = npc->collision.cylinder.field_2;
        if (radius <= 0)
            continue;

        cx = npc->position.vx + npc->collision.shapeOffsets.box.vx;
        cz = npc->position.vz + npc->collision.shapeOffsets.box.vz;

        dx = cx - camPos->vx;
        dz = cz - camPos->vz;

        /* kmult (Q12) = the multiplier on camFwd putting the ray at its closest
         * horizontal approach to the enemy axis. Since |camFwd| == Q12(1.0),
         * kmult is also the world distance along the ray to that point. */
        kmult = (s32)(((s64)dx * camFwd->vx + (s64)dz * camFwd->vz) / (fxz2 >> 12));
        if (kmult <= 0 || kmult > aimRange)
            continue; /* behind the camera or past aim range */

        qx = camPos->vx + (s32)(((s64)camFwd->vx * kmult) >> 12);
        qz = camPos->vz + (s32)(((s64)camFwd->vz * kmult) >> 12);
        perpH = Math_Vector2MagCalc(cx - qx, cz - qz);

        /* Vertical body span (sign-convention agnostic) — this is the actual
         * bullet collision cylinder, but it's only a narrow neck/upper-torso
         * band (so the engine's auto-aim, which always aimed at it, registered
         * hits while manual free-aim only hit near the neck). */
        yA = npc->position.vy + npc->collision.box.top;
        yB = npc->position.vy + npc->collision.box.height;
        yLo = (yA < yB) ? yA : yB;
        yHi = (yA < yB) ? yB : yA;
        bodyH  = yHi - yLo;
        /* ACTIVATION window: cover the full VISIBLE body (radius-based, since the
         * collision span underestimates it), so aiming at head/chest/legs all
         * trigger the assist. The aim point is then clamped back into the real
         * cylinder span [yLo,yHi] below, so the damage ray still threads it. */
        vSlack = (s32)(((s64)radius * AA_VERT_REACH_MUL) >> 12);
        if (bodyH > vSlack)
            vSlack = bodyH;

        rayY = camPos->vy + (s32)(((s64)camFwd->vy * kmult) >> 12);
        if (rayY < yLo - vSlack || rayY > yHi + vSlack)
            continue; /* reticle above / below the body */

        if (isPad)
        {
            s32 cone  = (s32)(((s64)kmult * AA_CTRL_CONE_TAN) >> 12);
            s32 floor = (s32)(((s64)radius * AA_CTRL_RADIUS_MUL) >> 12);
            maxPerp = (cone > floor) ? cone : floor;
        }
        else
        {
            maxPerp = (s32)(((s64)radius * AA_MOUSE_RADIUS_MUL) >> 12);
            if (maxPerp < AA_MOUSE_RADIUS_FLOOR)
                maxPerp = AA_MOUSE_RADIUS_FLOOR;
        }
        if (perpH > maxPerp)
            continue; /* reticle not over / near the body */

        /* Nearest qualifying enemy wins (tie-break: most on-target). */
        if (kmult < bestK || (kmult == bestK && perpH < bestPerp))
        {
            VECTOR3 aim;
            /* Clamp the aim point into the EXPANDED bullet cylinder span (the
             * same inflation func_8006EE0C applies during the player's bullet
             * trace) — not the narrow collision band — so a leg/torso shot aims
             * at that height and the impact (blood) lands where you aimed. */
            s32 ev  = (s32)(((s64)radius * g_PcBulletVertMul) >> 12);
            s32 eLo = yLo - ev;
            s32 eHi = yHi + ev;
            aim.vx = cx;
            aim.vy = (rayY < eLo) ? eLo : (rayY > eHi) ? eHi : rayY;
            aim.vz = cz;

            /* Don't aim through walls: reject if level geometry blocks the eye
             * from the aim point. (Char trace excluded — we want the enemy.) */
            if (dx != 0 || dz != 0)
            {
                s_RayTrace occ;
                s32        distToAim = Math_Vector2MagCalc(aim.vx - camPos->vx, aim.vz - camPos->vz);
                if (Ray_TraceQuery(&occ, camPos, &aim) &&
                    occ.hitDistance < distToAim - radius)
                    continue; /* occluded */
            }

            bestIdx  = i;
            bestK    = kmult;
            bestPerp = perpH;
            bestPt   = aim;
        }
    }

    if (bestIdx != NO_VALUE)
        *outAimPoint = bestPt;

    return bestIdx;
}
