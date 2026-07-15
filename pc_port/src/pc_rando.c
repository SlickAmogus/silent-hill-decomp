/* Randomizer gamemode. Config key `randomizer` (default 0); every entry point
 * is a no-op when it is off, so vanilla play is untouched.
 *
 * The mode never writes map-DLL data. It installs its own copy of the active
 * s_MapOverlayHdr (the same trick lang_text.c uses for translated messages) and
 * rewrites the event / event-func / message / chara-spawn tables inside that
 * copy. The DLL's tables therefore stay pristine and re-entering an area always
 * re-rolls from the original.
 *
 * The three facts the whole design rests on (all verified against the data, see
 * docs/Randomizer_Mode.md):
 *
 *  1. A door is just an s_EventData row whose sysState is SysState_LoadOverlay
 *     (change map) or SysState_LoadRoom (change room). Its pointOfInterestIdx
 *     names the doorway; its eventParam names the ARRIVAL mapPoint.
 *  2. That arrival mapPoint lives in the SOURCE map's mapPoints[] but holds
 *     coordinates in the DESTINATION map's space. So sending the player to an
 *     arbitrary map needs an arrival record authored by whichever map has a real
 *     door into it -- that is what pc_rando_data.h harvests.
 *  3. A locked/jammed door is a second row on the same doorway whose
 *     SysState_EventCallback selects the shared MapEvent_DoorLocked/Jammed.
 *     Those live at mapEventFuncs[0]/[1] -- but only in some maps, hence
 *     RANDO_DOORFN_MASK. We append our own locked-door handler instead, so
 *     locking works uniformly everywhere.
 */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/chara/chara.h"
#include "bodyprog/collision/collision.h"
#include "bodyprog/events/events_util.h"
#include "bodyprog/items.h"
#include "bodyprog/item_screens.h"
#include "bodyprog/map/map.h"
#include "bodyprog/math/math.h"
#include "bodyprog/savegame.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"
#include "bodyprog/events/map_msg.h"

#include "bodyprog/screen/screen_data.h" /* g_DeltaTimeRaw */

#include "map_registry.h"
#include "pc_config.h"
#include "pc_rando.h"
#include "pc_rando_data.h"
#include "sh_log.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ tuning */

#define RANDO_START_MAP     MapIdx_MAP2_S04
#define RANDO_BOSS_MAP      MapIdx_MAP7_S03
#define RANDO_AREAS_TO_BOSS 10   /* areas entered before the run ends at the boss */
#define RANDO_BOSS_PERMILLE 10   /* per-door final-boss chance, in 1/1000 (= 1%) */
#define RANDO_ENTRY_LOCK_S  10   /* seconds the door you came in through stays shut */

/* Extra handgun rounds on top of the 15 the custom-map loadout already grants. */
#define RANDO_EXTRA_HANDGUN_AMMO 30

/* Score. Kills and pickups earn; time and damage take a percentage back. */
#define SCORE_PER_KILL      100
#define SCORE_PER_ITEM      50
#define PENALTY_PER_MINUTE  1    /* % */
#define PENALTY_TIME_MAX    50   /* % */
#define PENALTY_PER_10HP    1    /* % per 10 HP taken */
#define PENALTY_DMG_MAX     50   /* % */

/* Score thresholds for the four endings (see rando_apply_ending). */
#define SCORE_GOOD_PLUS     3000
#define SCORE_GOOD          2000
#define SCORE_BAD_PLUS      1000

/* The two savegame flags the map7_s03 ending truth-table reads. */
#define ENDFLAG_CYBIL 449
#define ENDFLAG_GOOD  391

/* Door outcome weights, out of 100. Interior (room) doors mostly stay in-map so
 * a big area like the hospital is still worth exploring; exit doors mostly send
 * you somewhere new. LOCKED is drawn first, then the rest split the remainder. */
#define W_ROOM_LOCKED    25
#define W_ROOM_STAYS     70   /* of the unlocked remainder: another room here */
#define W_ROOM_MINIBOSS  2    /* of the unlocked remainder */

#define W_EXIT_LOCKED    15
#define W_EXIT_MINIBOSS  4    /* of the unlocked remainder */

/* Table sizes. map7_s02 has 251 events (the most); map7_s03 has 100 messages. */
#define RANDO_MAX_EVENTS 272
#define RANDO_MAX_FUNCS  80
#define RANDO_MAX_MSGS   144
#define RANDO_MAX_DOORS  96

/* --------------------------------------------------------------- area pools */

/* map6_s04 (Amusement Park) is intentionally NOT here: one of its rooms is the
 * Cybil boss arena, and a room transition into it softlocks the run. */
static const u8 RANDO_AREAS[] = {
    MapIdx_MAP3_S03, MapIdx_MAP5_S01, MapIdx_MAP6_S01,
    MapIdx_MAP2_S02, MapIdx_MAP2_S04,
};
#define N_AREAS ((int)ARRAY_SIZE(RANDO_AREAS))

static const u8 RANDO_MINIBOSSES[] = { MapIdx_MAP1_S05, MapIdx_MAP4_S05 };
#define N_MINIBOSSES ((int)ARRAY_SIZE(RANDO_MINIBOSSES))

/* Monster pool. stateStep is the AI entry state -- a wrong value leaves the
 * monster unposed and invisible (see project_monster_spawn_command); these are
 * the values the console SPAWN table uses. */
typedef struct { u8 charaId; u8 stateStep; } s_RandoMonster;
static const s_RandoMonster RANDO_MONSTERS[] = {
    { Chara_GreyChild,  3  },
    { Chara_PuppetNurse, 17 },
    { Chara_Romper,     5  },
    { Chara_Groaner,    5  },
    { Chara_AirScreamer, 12 },
};
#define N_MONSTERS ((int)ARRAY_SIZE(RANDO_MONSTERS))
#define RANDO_MONSTERS_MIN 1
#define RANDO_MONSTERS_MAX 5

/* Item pool. Chainsaw / HyperBlaster / RockDrill / GasolineTank are excluded by
 * request; Katana and Axe are Next-Fear unlockables and are excluded in the same
 * spirit. Weapons only drop if the player lacks them; ammo only for guns held. */
static const u8 RANDO_HEALTH[]  = {
    InvItemId_HealthDrink, InvItemId_FirstAidKit, InvItemId_Ampoule,
};
static const u8 RANDO_WEAPONS[] = {
    InvItemId_KitchenKnife, InvItemId_SteelPipe, InvItemId_Hammer,
    InvItemId_Handgun, InvItemId_HuntingRifle, InvItemId_Shotgun,
};
#define N_HEALTH  ((int)ARRAY_SIZE(RANDO_HEALTH))
#define N_WEAPONS ((int)ARRAY_SIZE(RANDO_WEAPONS))

/* For a firearm, Inventory_AddSpecialItem reads the count as the LOADED round
 * count -- the same numbers the vanilla weapon pickups pass. */
static const u8 RANDO_WEAPON_CNT[] = {
    DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT, DEFAULT_PICKUP_ITEM_COUNT,
    HANDGUN_AMMO_PICKUP_ITEM_COUNT, RIFLE_AMMO_PICKUP_ITEM_COUNT, SHOTGUN_AMMO_PICKUP_ITEM_COUNT,
};

/* Prompts for the weapons. The six common pickups (health x3, ammo x3) already
 * have universal messages at MapMsgIdx 5..10; weapons do not -- each map that
 * ships one carries its own string at a map-local index. So we append ours past
 * the end of the map's table (RANDO_MSG_COUNT). Underscores render as spaces;
 * ~C2/~C7 are colour codes and ~S4 makes it a Yes/No prompt. */
static const char* const RANDO_WEAPON_MSGS[N_WEAPONS] = {
    "There_is_a_ ~C2 Kitchen_knife ~C7 . ~N Take_it? ~S4 ",
    "There_is_a_ ~C2 Steel_pipe ~C7 . ~N Take_it? ~S4 ",
    "There_is_a_ ~C2 Hammer ~C7 . ~N Take_it? ~S4 ",
    "There_is_a_ ~C2 Handgun ~C7 . ~N Take_it? ~S4 ",
    "There_is_a_ ~C2 Hunting_rifle ~C7 . ~N Take_it? ~S4 ",
    "There_is_a_ ~C2 Shotgun ~C7 . ~N Take_it? ~S4 ",
};

/* Minibosses. Both follow the same shape: a fight-arming flag, a "boss is dead"
 * flag the AI sets, and a terminal SysState_LoadOverlay row gated on an exit
 * flag that the post-death cutscene sets. We keep that chain intact -- the
 * cutscene supplies the "a few seconds after it dies" beat for free -- and only
 * rewrite where the terminal row sends you.
 *
 * map4_s05 is the awkward one: its fight is armed by EventFlag_347, which vanilla
 * sets via a room transition deep inside the map, and its cross-map entrance is
 * ~290 units from the Floatstinger. So we land the player on its own mapPoints
 * [13] -- the room-arrival the vanilla boss-room transition uses, 6 units from
 * the boss spawn -- and set 347 directly. */
typedef struct {
    u8  mapIdx;
    s16 armFlag;      /* set on entry to arm the fight (0 = fires on cold entry) */
    s16 deadFlag;     /* the boss AI sets this */
    s16 roomArrival;  /* mapPoints index to land on, or NO_VALUE = use the harvested cross-map arrival */
    s16 clearFlags[6];
    s16 sirenSfx[2];  /* looping SFX the death cutscene key-ons. Vanilla stops these
                       * in the specific story map you would exit to; the randomizer
                       * exits elsewhere, so it must stop them itself or they loop
                       * forever. 0 = unused slot. */
} s_RandoMiniboss;
static const s_RandoMiniboss RANDO_MINIBOSS_INFO[] = {
    { MapIdx_MAP1_S05, 0,   131, -1, { 130, 131, 132, 0, 0, 0 }, { Sfx_Unk1359, Sfx_Unk1478 } },
    { MapIdx_MAP4_S05, 347, 350, 13, { 346, 347, 349, 350, 351, 352 }, { Sfx_Unk1522, 0 } },
};

/* -------------------------------------------------------------------- state */

typedef enum {
    DOOR_LOCKED = 0,
    DOOR_AREA,      /* another map from RANDO_AREAS */
    DOOR_ROOM,      /* another room in this map */
    DOOR_MINIBOSS,
    DOOR_BOSS,
} e_RandoDoorKind;

typedef struct {
    u8 kind;
    u8 destMap;      /* AREA / MINIBOSS / BOSS */
    u8 arrivalIdx;   /* index into RANDO_ARRIVALS[destMap] */
    u8 poi;          /* doorway, for locating the entry door */
    u8 scripted;     /* a miniboss's post-death exit: registered only so the arrival
                      * override can find it. It has no doorway, its requiredEventFlag
                      * is load-bearing, and rewriting or locking it would fire it
                      * immediately -- so nothing else may touch it. */
    u8 origActivation;
    s16 eventIdx;    /* row in s_events this door owns */
} s_RandoDoor;

/* Everything the run carries. Reset wholesale on New Game (minus `enabled`/`rng`,
 * which come from boot), so no field can be forgotten as this grows. */
typedef struct {
    int  running;      /* a run is live (New Game taken in randomizer mode) */

    int  areasEntered;
    int  kills;
    int  itemsTaken;
    s32  damageTaken;  /* q19_12, accumulated */
    int  entryPending; /* a door was taken (or New Game): the next map load is a new area */

    /* per-area */
    s32  mapIdx;
    int  doorCount;
    int  entryDoor;        /* index into s_doors, or -1 */
    s32  entryLockTimer;   /* q19_12 seconds remaining */
    int  monstersWanted;
    int  monstersPlaced;
    int  monstersDone;     /* placement pass has run for this area */
    s32  arrivalX, arrivalZ;
    int  headerInstalled;

    /* one pickup is resolved at a time; the decision must not flicker between
     * the frame that shows the model and the frame that adds the item */
    s32  cacheFlag;
    s32  cacheItem;
    s32  cacheCount;
    s32  cacheMsg;
    int  cacheCounted;
} s_RandoRun;

static s_RandoRun s_run;
static int        s_enabled; /* config */
static u32        s_rng;

static s_MapOverlayHdr s_hdr;
static s_EventData     s_events[RANDO_MAX_EVENTS];
static void          (*s_eventFuncs[RANDO_MAX_FUNCS])();
static const char*     s_msgs[RANDO_MAX_MSGS];
static s_RandoDoor     s_doors[RANDO_MAX_DOORS];
static int             s_lockedFuncIdx; /* index of Pc_Rando_DoorLocked in s_eventFuncs */

/* ---------------------------------------------------------------------- rng */

/* Private stream: the game's Rng_* feeds enemy behaviour and demo playback, and
 * drawing from it would desync those. */
static u32 rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int rnd_below(int n) { return (n <= 0) ? 0 : (int)(rnd() % (u32)n); }

/* -------------------------------------------------------------------- utils */

int Pc_Rando_Active(void) { return s_enabled && s_run.running; }

static const s_RandoMiniboss* miniboss_info(int mapIdx);

static int map_has_arrival(int mapIdx)
{
    return mapIdx >= 0 && mapIdx < (int)ARRAY_SIZE(RANDO_ARRIVALS) &&
           RANDO_ARRIVALS[mapIdx].count > 0;
}

static int inventory_has(s32 itemId)
{
    int i;
    for (i = 0; i < INV_ITEM_COUNT_MAX; i++)
    {
        if (g_SavegamePtr->items[i].id_0 == itemId)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------- events */

/* The doorway's own trigger row survives; only where it leads changes. */
static void door_write_event(s_EventData* e, const s_RandoDoor* d)
{
    /* A randomized door is never gated: key doors, story-flag doors and the
     * "it's locked" rows all become plain press-to-use doors. */
    e->requiredEventFlag = EventFlag_None;
    e->disabledEventFlag = EventFlag_None;
    e->requiredItemId    = 0;

    /* A locked door MUST be button-activated. Event_Update only demands a fresh
     * button edge for TriggerActivationType_Button; a touch-activated row re-matches
     * every frame the player stands in its volume, and the locked handler freezes
     * player control -- an unbreakable "It's locked." loop. Many vanilla door rows
     * are touch-activated (they open on contact), so this cannot be assumed. */
    if (d->kind == DOOR_LOCKED || d->origActivation == TriggerActivationType_Item)
        e->activationType = TriggerActivationType_Button;
    else
        e->activationType = d->origActivation;

    switch (d->kind)
    {
        case DOOR_LOCKED:
            e->sysState   = SysState_EventCallback;
            e->eventParam = (u32)s_lockedFuncIdx;
            break;

        case DOOR_ROOM:
        {
            const s_RandoRoomSet* rs = &RANDO_ROOMS[s_run.mapIdx];
            const s_RandoRoomPoint* rp = &rs->points[d->arrivalIdx];
            e->sysState   = SysState_LoadRoom;
            e->eventParam = rp->pointIdx; /* a real mapPoint of THIS map */
            e->mapIdx     = rp->bgmIdx;   /* on a room transition this field is the BGM track */
            break;
        }

        case DOOR_AREA:
        case DOOR_MINIBOSS:
        case DOOR_BOSS:
            e->sysState   = SysState_LoadOverlay;
            e->mapIdx     = d->destMap;
            /* eventParam would name an arrival mapPoint of THIS map, which cannot
             * describe a landing spot in another one. Pc_Rando_ArrivalOverride
             * supplies the real record; leave the index in range so nothing reads
             * out of bounds before it does. */
            e->eventParam = 0;
            break;
    }
}

static void door_roll(s_RandoDoor* d, int isExitDoor)
{
    int locked  = isExitDoor ? W_EXIT_LOCKED   : W_ROOM_LOCKED;
    int miniW   = isExitDoor ? W_EXIT_MINIBOSS : W_ROOM_MINIBOSS;
    int staysW  = isExitDoor ? 0               : W_ROOM_STAYS;
    int r;

    /* Past the area budget every door is the way out. */
    if (s_run.areasEntered >= RANDO_AREAS_TO_BOSS)
    {
        d->kind    = DOOR_BOSS;
        d->destMap = RANDO_BOSS_MAP;
        d->arrivalIdx = 0;
        return;
    }

    if ((int)(rnd() % 1000u) < RANDO_BOSS_PERMILLE)
    {
        d->kind    = DOOR_BOSS;
        d->destMap = RANDO_BOSS_MAP;
        d->arrivalIdx = 0;
        return;
    }

    if (rnd_below(100) < locked)
    {
        d->kind = DOOR_LOCKED;
        return;
    }

    r = rnd_below(100);

    /* "Another room in this map" needs the map to actually have room-arrival
     * points; the boss arenas and small maps have none. */
    if (r < staysW && RANDO_ROOMS[s_run.mapIdx].count > 0)
    {
        d->kind       = DOOR_ROOM;
        d->arrivalIdx = (u8)rnd_below(RANDO_ROOMS[s_run.mapIdx].count);
        return;
    }

    if (r >= 100 - miniW)
    {
        int k = rnd_below(N_MINIBOSSES);
        if (map_has_arrival(RANDO_MINIBOSSES[k]))
        {
            d->kind    = DOOR_MINIBOSS;
            d->destMap = RANDO_MINIBOSSES[k];
            d->arrivalIdx = 0;
            return;
        }
    }

    /* Default: somewhere else in the area pool. Prefer not to loop back into the
     * area the player is already standing in. */
    {
        int tries;
        int pick = RANDO_AREAS[rnd_below(N_AREAS)];
        for (tries = 0; tries < 4 && pick == s_run.mapIdx; tries++)
            pick = RANDO_AREAS[rnd_below(N_AREAS)];

        d->kind    = DOOR_AREA;
        d->destMap = (u8)pick;
        d->arrivalIdx = (u8)rnd_below(RANDO_ARRIVALS[pick].count);
    }
}

/* Is this row one of the doorway rows we own?
 *
 * A door is always TOUCH-triggered. A TriggerType_None row fires the instant its
 * flags allow, with no doorway at all -- those are scripted transitions (the
 * post-boss exits in map1_s05/map4_s05, map6_s04's post-story exit). Treating one
 * as a door would strip its requiredEventFlag and teleport the player out of a
 * boss arena the moment they arrived. */
static int event_is_door(const s_EventData* e, int mapIdx)
{
    if (e->triggerType == TriggerType_None)
        return 0;

    if (e->sysState == SysState_LoadOverlay || e->sysState == SysState_LoadRoom)
        return 1;

    /* A vanilla locked or jammed door: an EventCallback row selecting the shared
     * MapEvent_DoorLocked / MapEvent_DoorJammed. Only trust eventParam 0/1 in the
     * maps that actually put those funcs there. */
    if (e->sysState == SysState_EventCallback)
    {
        u8 mask = RANDO_DOORFN_MASK[mapIdx];
        if (e->eventParam == 0 && (mask & RANDO_DOORFN_JAMMED))
            return 1;
        if (e->eventParam == 1 && (mask & RANDO_DOORFN_LOCKED))
            return 1;
    }
    return 0;
}

/* Our universal "It's locked." handler. The shared MapEvent_DoorLocked is a
 * map-DLL symbol and 18 of 43 maps do not even have one, so the mode appends
 * this instead. Mirrors the shared handler exactly. */
static void Pc_Rando_DoorLocked(void)
{
    const s_MapPoint2d* p = &g_MapOverlayHdr.mapPoints[g_MapEventData->pointOfInterestIdx];
    VECTOR3 sfxPos = { p->positionX, Q12(-1.2f), p->positionZ };
    Event_DisplayMapMsgWithSfx(MapMsgIdx_DoorLocked, Sfx_DoorLocked, &sfxPos);
}

/* Rebuild the event table into s_events, replacing the first row of every
 * doorway with a randomized one and dropping that doorway's other rows.
 * Everything else (item pickups, save points, examine text, cutscene chains) is
 * copied verbatim, in order -- Event_Update returns on the first match, so the
 * original relative order has to survive. */
static void build_events(const s_EventData* src, int mapIdx)
{
    u8  poiSeen[256];
    int n = 0, i;
    int openCount = 0;
    int isMiniboss = (miniboss_info(mapIdx) != NULL);

    memset(poiSeen, 0, sizeof(poiSeen));
    s_run.doorCount = 0;

    for (i = 0; i < RANDO_MAX_EVENTS - 1; i++)
    {
        const s_EventData* e = &src[i];
        if (e->triggerType == TriggerType_EndOfArray)
            break;

        /* No saving in a randomizer run: the world save points are simply dropped
         * from the table, so the red square is inert. Quick save / quick load are
         * gated separately, in pc_quicksave.c. */
        if (e->sysState == SysState_SaveMenu0 || e->sysState == SysState_SaveMenu1)
            continue;

        if (event_is_door(e, mapIdx) && s_run.doorCount < RANDO_MAX_DOORS)
        {
            u8 poi = e->pointOfInterestIdx;

            if (poiSeen[poi])
                continue; /* this doorway already has its randomized row */

            poiSeen[poi] = 1;

            {
                s_RandoDoor* d = &s_doors[s_run.doorCount];
                d->poi            = poi;
                d->eventIdx       = (s16)n;
                d->scripted       = 0;
                d->origActivation = e->activationType;
                door_roll(d, e->sysState == SysState_LoadOverlay);

                s_events[n] = *e;
                door_write_event(&s_events[n], d);

                if (d->kind != DOOR_LOCKED)
                    openCount++;

                s_run.doorCount++;
                n++;
            }
            continue;
        }

        s_events[n] = *e;

        /* The miniboss's way out. Vanilla: boss dies -> AI sets its dead flag ->
         * the post-death cutscene runs and sets the exit flag -> this row loads the
         * story map. We keep that whole chain (the cutscene is the "a few seconds
         * after it dies" beat) and only change where it lands. Its requiredEventFlag
         * MUST survive, or it fires on arrival. */
        if (isMiniboss && e->triggerType == TriggerType_None &&
            e->sysState == SysState_LoadOverlay && s_run.doorCount < RANDO_MAX_DOORS)
        {
            s_RandoDoor* d = &s_doors[s_run.doorCount];
            int pick = RANDO_AREAS[rnd_below(N_AREAS)];

            d->poi            = e->pointOfInterestIdx;
            d->eventIdx       = (s16)n;
            d->kind           = DOOR_AREA;
            d->destMap        = (u8)pick;
            d->arrivalIdx     = (u8)rnd_below(RANDO_ARRIVALS[pick].count);
            d->scripted       = 1;
            d->origActivation = e->activationType;

            s_events[n].mapIdx     = d->destMap;
            s_events[n].eventParam = 0; /* Pc_Rando_ArrivalOverride supplies the landing */
            s_run.doorCount++;
        }

        n++;
    }

    /* At least one way out, always -- but never by rewriting a scripted exit. A
     * miniboss arena legitimately has no open door: the boss IS the door. */
    if (openCount == 0)
    {
        int candidates[RANDO_MAX_DOORS];
        int nc = 0;
        for (i = 0; i < s_run.doorCount; i++)
            if (!s_doors[i].scripted)
                candidates[nc++] = i;

        if (nc > 0)
        {
            s_RandoDoor* d = &s_doors[candidates[rnd_below(nc)]];
            d->kind       = DOOR_AREA;
            d->destMap    = RANDO_AREAS[rnd_below(N_AREAS)];
            d->arrivalIdx = (u8)rnd_below(RANDO_ARRIVALS[d->destMap].count);
            door_write_event(&s_events[d->eventIdx], d);
        }
    }

    s_events[n].triggerType = TriggerType_EndOfArray;
}

/* Copy the map's event funcs and append ours. mapEventFuncs has no length, so
 * derive one from the largest index any EventCallback row actually uses. */
static void build_event_funcs(const s_EventData* src, void (**origFuncs)())
{
    int maxIdx = -1, i;

    for (i = 0; i < RANDO_MAX_EVENTS; i++)
    {
        if (src[i].triggerType == TriggerType_EndOfArray)
            break;
        if (src[i].sysState == SysState_EventCallback && (int)src[i].eventParam > maxIdx)
            maxIdx = (int)src[i].eventParam;
    }

    if (maxIdx >= RANDO_MAX_FUNCS - 2)
        maxIdx = RANDO_MAX_FUNCS - 3;

    for (i = 0; i <= maxIdx; i++)
        s_eventFuncs[i] = origFuncs ? origFuncs[i] : NULL;

    s_lockedFuncIdx = maxIdx + 1;
    s_eventFuncs[s_lockedFuncIdx] = Pc_Rando_DoorLocked;
}

static void build_messages(int mapIdx, const char** origMsgs)
{
    int count = RANDO_MSG_COUNT[mapIdx];
    int i;

    if (count > RANDO_MAX_MSGS - N_WEAPONS)
        count = RANDO_MAX_MSGS - N_WEAPONS;

    for (i = 0; i < count; i++)
        s_msgs[i] = origMsgs ? origMsgs[i] : "";

    for (i = 0; i < N_WEAPONS; i++)
        s_msgs[count + i] = RANDO_WEAPON_MSGS[i];
}

/* Message index of the prompt for a randomized weapon drop. */
static int weapon_msg_idx(int mapIdx, int weaponSlot)
{
    int count = RANDO_MSG_COUNT[mapIdx];
    if (count > RANDO_MAX_MSGS - N_WEAPONS)
        count = RANDO_MAX_MSGS - N_WEAPONS;
    return count + weaponSlot;
}

/* ------------------------------------------------------------------ spawning */

/* Walkable-ground test: nonzero groundType means the collision data has floor
 * here. mapPoints are a good scatter of in-map positions, but a map's table also
 * holds arrival records expressed in OTHER maps' coordinate spaces -- this is
 * what filters those out. */
static int position_is_walkable(q19_12 x, q19_12 z)
{
    return func_800808AC(x, z) != 0;
}

static int far_enough(q19_12 x, q19_12 z, q19_12 fx, q19_12 fz, q19_12 minDist)
{
    s64 dx = (s64)x - fx;
    s64 dz = (s64)z - fz;
    s64 d2 = dx * dx + dz * dz;
    return d2 >= (s64)minDist * minDist;
}

/* The map's authored spawn positions, taken at load time -- clear_native_spawns
 * wipes the rows themselves, so they cannot be read back on the placement frame. */
static q19_12 s_nativeX[32];
static q19_12 s_nativeZ[32];
static int    s_nativeCount;

/* Rewrite the map's chara spawn table with 1..5 monsters from the pool, placed
 * on authored, walkable positions away from where the player just came in. Runs
 * on the first gameplay frame, when collision data is up. */
static void place_monsters(void)
{
    s_MapPoint2d*   pts   = g_MapOverlayHdr.mapPoints;
    s_SpawnInfo*    rows  = &g_MapOverlayHdr.charaSpawnInfos[0][0];
    const int       nRows = 32;
    q19_12          candX[64];
    q19_12          candZ[64];
    int             nCand = 0;
    int             i, placed;

    /* A miniboss arena keeps its own single authored row (the boss). Nothing to
     * place, and nothing here may touch that row or its spawn bookkeeping. */
    if (s_run.monstersWanted <= 0)
    {
        s_run.monstersPlaced = 0;
        s_run.monstersDone   = 1;
        return;
    }

    /* Candidates: the map's own authored spawn positions first (guaranteed sane),
     * then its mapPoints as a whole-map scatter. */
    for (i = 0; i < s_nativeCount && nCand < (int)ARRAY_SIZE(candX); i++)
    {
        if (!position_is_walkable(s_nativeX[i], s_nativeZ[i]))
            continue;
        if (!far_enough(s_nativeX[i], s_nativeZ[i],
                        s_run.arrivalX, s_run.arrivalZ, Q12(10.0f)))
            continue;
        candX[nCand] = s_nativeX[i];
        candZ[nCand] = s_nativeZ[i];
        nCand++;
    }

    if (pts != NULL)
    {
        int nPts = (int)RANDO_MAPPOINT_COUNT[s_run.mapIdx];
        for (i = 0; i < nPts && nCand < (int)ARRAY_SIZE(candX); i++)
        {
            if (!position_is_walkable(pts[i].positionX, pts[i].positionZ))
                continue;
            if (!far_enough(pts[i].positionX, pts[i].positionZ,
                            s_run.arrivalX, s_run.arrivalZ, Q12(10.0f)))
                continue;
            candX[nCand] = pts[i].positionX;
            candZ[nCand] = pts[i].positionZ;
            nCand++;
        }
    }

    /* Fallback for a map whose points are all foreign-space or unwalkable: probe
     * rings around the player. */
    if (nCand < s_run.monstersWanted)
    {
        static const q19_12 RADII[] = { Q12(14.0f), Q12(22.0f), Q12(30.0f) };
        int r, a;
        for (r = 0; r < (int)ARRAY_SIZE(RADII) && nCand < (int)ARRAY_SIZE(candX); r++)
        {
            for (a = 0; a < 12 && nCand < (int)ARRAY_SIZE(candX); a++)
            {
                q3_12  ang = (q3_12)((a * 4096) / 12);
                q19_12 x   = s_run.arrivalX + (q19_12)(((s64)RADII[r] * Math_Sin(ang)) >> 12);
                q19_12 z   = s_run.arrivalZ + (q19_12)(((s64)RADII[r] * Math_Cos(ang)) >> 12);
                if (!position_is_walkable(x, z))
                    continue;
                candX[nCand] = x;
                candZ[nCand] = z;
                nCand++;
            }
        }
    }

    placed = 0;
    for (i = 0; i < s_run.monstersWanted && nCand > 0 && placed < nRows; i++)
    {
        int c = rnd_below(nCand);
        const s_RandoMonster* m = &RANDO_MONSTERS[rnd_below(N_MONSTERS)];

        rows[placed].positionX        = candX[c];
        rows[placed].positionZ        = candZ[c];
        rows[placed].charaId          = (s8)m->charaId;
        rows[placed].rotationY        = (q0_8)(rnd() & 0xFF);
        rows[placed].flags            = (s8)m->stateStep;
        rows[placed].gameDifficultyMin = GameDifficulty_Easy;
        placed++;

        /* consume the candidate so two monsters don't stack */
        candX[c] = candX[nCand - 1];
        candZ[c] = candZ[nCand - 1];
        nCand--;
    }

    s_run.monstersPlaced = placed;
    s_run.monstersDone   = 1;
    SH_LOG("[RANDO] %s: placed %d/%d monsters (%d candidates left)",
           MapRegistry_GetName((e_MapIdx)s_run.mapIdx), placed, s_run.monstersWanted, nCand);
}

/* Wipe the map's authored spawn rows at load time, keeping their positions --
 * they are the best monster placements the map has, and placement itself has to
 * wait for the first gameplay frame (it needs collision data to test for floor).
 * The wipe is needed because GameBoot_InGameInit runs Game_NpcRoomInitSpawn before
 * then, so without it the area would come up with its NATIVE monsters and then get
 * ours on top.
 *
 * NEVER call this on a miniboss map: map1_s05 and map4_s05 each have exactly one
 * spawn row, and it is the boss. */
static void clear_native_spawns(void)
{
    s_SpawnInfo* rows = &s_hdr.charaSpawnInfos[0][0];
    int i;

    s_nativeCount = 0;
    for (i = 0; i < 32; i++)
    {
        if (rows[i].flags != 0)
        {
            s_nativeX[s_nativeCount] = rows[i].positionX;
            s_nativeZ[s_nativeCount] = rows[i].positionZ;
            s_nativeCount++;
        }
        rows[i].charaId = Chara_None;
        rows[i].flags   = 0;
    }
}

/* ---------------------------------------------------------------- item remap */

/* Taking a pickup latches its event flag in the savegame, which retires the
 * trigger for good -- so on a second visit the area would come back stripped.
 * Re-arming needs to know WHICH flags are pickups, and nothing in the event table
 * says so. So learn them: every flag Event_ItemTake is actually invoked with is a
 * pickup by definition. Record (map, flag) here, clear them on re-entry.
 *
 * Flags never seen still work on a first visit (they start clear), so the table
 * only ever needs to grow as the run does. */
#define RANDO_MAX_PICKUPS 128
static struct { u8 mapIdx; s16 flag; } s_pickups[RANDO_MAX_PICKUPS];
static int s_pickupCount;

static void pickup_remember(s32 eventFlagIdx)
{
    int i;
    if (eventFlagIdx <= 0)
        return;
    for (i = 0; i < s_pickupCount; i++)
        if (s_pickups[i].mapIdx == (u8)s_run.mapIdx && s_pickups[i].flag == (s16)eventFlagIdx)
            return;
    if (s_pickupCount < RANDO_MAX_PICKUPS)
    {
        s_pickups[s_pickupCount].mapIdx = (u8)s_run.mapIdx;
        s_pickups[s_pickupCount].flag   = (s16)eventFlagIdx;
        s_pickupCount++;
    }
}

static void pickups_rearm(s32 mapIdx)
{
    int i;
    for (i = 0; i < s_pickupCount; i++)
        if (s_pickups[i].mapIdx == (u8)mapIdx)
            Savegame_EventFlagClear(s_pickups[i].flag);
}

static void resolve_pickup(s32 eventFlagIdx)
{
    int haveGun[3];
    int nGun = 0, gunSlot[3];
    int lack[N_WEAPONS];
    int nLack = 0;
    int roll, i;

    haveGun[0] = inventory_has(InvItemId_Handgun);
    haveGun[1] = inventory_has(InvItemId_HuntingRifle);
    haveGun[2] = inventory_has(InvItemId_Shotgun);
    for (i = 0; i < 3; i++)
        if (haveGun[i])
            gunSlot[nGun++] = i;

    for (i = 0; i < N_WEAPONS; i++)
        if (!inventory_has(RANDO_WEAPONS[i]))
            lack[nLack++] = i;

    s_run.cacheFlag    = eventFlagIdx;
    s_run.cacheCounted = 0;

    roll = rnd_below(100);

    /* 45% healing / 25% a weapon you lack / 30% ammo for a gun you carry, with
     * the empty categories folded back into healing. */
    if (roll >= 45 && roll < 70 && nLack > 0)
    {
        int w = lack[rnd_below(nLack)];
        s_run.cacheItem  = RANDO_WEAPONS[w];
        s_run.cacheCount = RANDO_WEAPON_CNT[w];
        s_run.cacheMsg   = weapon_msg_idx(s_run.mapIdx, w);
        return;
    }

    if (roll >= 70 && nGun > 0)
    {
        static const u8  AMMO_ID[3]  = { InvItemId_HandgunBullets, InvItemId_RifleShells, InvItemId_ShotgunShells };
        static const u8  AMMO_CNT[3] = { HANDGUN_AMMO_PICKUP_ITEM_COUNT, RIFLE_AMMO_PICKUP_ITEM_COUNT, SHOTGUN_AMMO_PICKUP_ITEM_COUNT };
        static const u8  AMMO_MSG[3] = { MapMsgIdx_HandgunAmmoSelect, MapMsgIdx_RifleAmmoSelect, MapMsgIdx_ShotgunAmmoSelect };
        int g = gunSlot[rnd_below(nGun)];
        s_run.cacheItem  = AMMO_ID[g];
        s_run.cacheCount = AMMO_CNT[g];
        s_run.cacheMsg   = AMMO_MSG[g];
        return;
    }

    {
        static const u8 HEALTH_MSG[N_HEALTH] = {
            MapMsgIdx_HealthDrinkSelect, MapMsgIdx_FirstAidSelect, MapMsgIdx_AmpouleSelect,
        };
        int h = rnd_below(N_HEALTH);
        s_run.cacheItem  = RANDO_HEALTH[h];
        s_run.cacheCount = DEFAULT_PICKUP_ITEM_COUNT;
        s_run.cacheMsg   = HEALTH_MSG[h];
    }
}

void Pc_Rando_RemapItemTake(s32* itemId, s32* itemCount, s32 eventFlagIdx, s32* mapMsgIdx)
{
    /* headerInstalled: the appended weapon prompts only exist in the message table
     * we built for THIS map. On any map the mode left alone, a remapped weapon's
     * message index would point past the end of the map's real table. */
    if (!Pc_Rando_Active() || !s_run.headerInstalled)
        return;

    /* Event_ItemTake is a state machine re-entered every frame. The item must not
     * change between the frame that spins up its model and the frame that adds
     * it, so the roll is cached per pickup. */
    if (s_run.cacheFlag != eventFlagIdx)
    {
        resolve_pickup(eventFlagIdx);
        pickup_remember(eventFlagIdx);
    }

    /* EventState_TakeItem == 3: the player said yes. Count it once. */
    if (!s_run.cacheCounted && g_SysWork.sysStateSteps[1] == 3)
    {
        s_run.cacheCounted = 1;
        s_run.itemsTaken++;
    }

    *itemId    = s_run.cacheItem;
    *itemCount = s_run.cacheCount;
    *mapMsgIdx = s_run.cacheMsg;
}

/* -------------------------------------------------------------------- score */

static int run_minutes(void)
{
    return (int)((u32)g_SavegamePtr->gameplayTimer >> 12) / 60;
}

static int rando_score(void)
{
    int base = s_run.kills * SCORE_PER_KILL + s_run.itemsTaken * SCORE_PER_ITEM;
    int timePen = run_minutes() * PENALTY_PER_MINUTE;
    int dmgPen  = (int)((u32)s_run.damageTaken >> 12) / 10 * PENALTY_PER_10HP;
    int keep;

    if (timePen > PENALTY_TIME_MAX) timePen = PENALTY_TIME_MAX;
    if (dmgPen  > PENALTY_DMG_MAX)  dmgPen  = PENALTY_DMG_MAX;

    keep = 100 - timePen - dmgPen;
    if (keep < 0) keep = 0;

    return base * keep / 100;
}

void Pc_Rando_OnEnemyKilled(void)
{
    if (!Pc_Rando_Active())
        return;
    s_run.kills++;
}

void Pc_Rando_OnDamageTaken(s32 amount)
{
    if (!Pc_Rando_Active() || amount <= 0)
        return;
    s_run.damageTaken += amount;
}

int Pc_Rando_ScoreLine(char* buf, int cap)
{
    /* Gameplay only -- it is a HUD, not a debug panel, so it should not paint over
     * the inventory, the map screen or the menus. */
    if (!Pc_Rando_Active() || g_GameWork.gameState != GameState_InGame)
        return 0;
    snprintf(buf, cap, "Score: %d", rando_score());
    return 1;
}

/* The ending is a 2-bit truth table over these two flags, read once by
 * map7_s03's Map_WorldObjectsInit -- which runs during the map's own init, well
 * after our map-load hook. Setting them here therefore lands in time, and it
 * also picks the right final boss (391 selects Incubus vs Incubator). */
static void apply_ending(void)
{
    int score = rando_score();
    int cybil, good;
    const char* name;

    if      (score >= SCORE_GOOD_PLUS) { cybil = 1; good = 1; name = "Good+"; }
    else if (score >= SCORE_GOOD)      { cybil = 0; good = 1; name = "Good";  }
    else if (score >= SCORE_BAD_PLUS)  { cybil = 1; good = 0; name = "Bad+";  }
    else                               { cybil = 0; good = 0; name = "Bad";   }

    if (cybil) Savegame_EventFlagSet(ENDFLAG_CYBIL); else Savegame_EventFlagClear(ENDFLAG_CYBIL);
    if (good)  Savegame_EventFlagSet(ENDFLAG_GOOD);  else Savegame_EventFlagClear(ENDFLAG_GOOD);

    SH_LOG("[RANDO] final boss: score %d -> %s ending (kills %d, items %d, %d min, %d HP taken)",
           score, name, s_run.kills, s_run.itemsTaken, run_minutes(),
           (int)((u32)s_run.damageTaken >> 12));
}

/* --------------------------------------------------------------- map loading */

static const s_RandoMiniboss* miniboss_info(int mapIdx)
{
    int i;
    for (i = 0; i < (int)ARRAY_SIZE(RANDO_MINIBOSS_INFO); i++)
        if (RANDO_MINIBOSS_INFO[i].mapIdx == mapIdx)
            return &RANDO_MINIBOSS_INFO[i];
    return NULL;
}

/* Silence a miniboss's death-cutscene siren loop. A key-off on a voice that is not
 * sounding is harmless, so this is safe to call on any exit from a miniboss map. */
static void stop_miniboss_siren(int mapIdx)
{
    const s_RandoMiniboss* mb = miniboss_info(mapIdx);
    int i;
    if (mb == NULL)
        return;
    for (i = 0; i < (int)ARRAY_SIZE(mb->sirenSfx); i++)
        if (mb->sirenSfx[i] != 0)
            Sd_SfxStop((u16)mb->sirenSfx[i]);
}

void Pc_Rando_ArrivalOverride(s_MapPoint2d* arrival, const s_EventData* evt)
{
    int i;

    if (!Pc_Rando_Active() || !s_run.headerInstalled || evt == NULL || arrival == NULL)
        return;

    /* Only rows we rewrote: g_MapEventData points into s_events. */
    if (evt < &s_events[0] || evt >= &s_events[RANDO_MAX_EVENTS])
        return;

    for (i = 0; i < s_run.doorCount; i++)
    {
        const s_RandoDoor* d = &s_doors[i];
        if (&s_events[d->eventIdx] != evt)
            continue;
        if (d->kind != DOOR_AREA && d->kind != DOOR_MINIBOSS && d->kind != DOOR_BOSS)
            return; /* room jumps use a real mapPoint of this map -- vanilla path */

        /* This door is taking us to a new area; the map load that follows counts. */
        s_run.entryPending = 1;

        {
            const s_RandoMiniboss* mb = miniboss_info(d->destMap);

            /* map4_s05's boss arena is only reachable by one of its own room
             * transitions; its cross-map entrance is ~290 units away. Land on the
             * arena's mapPoint instead so the fight can actually start. */
            if (mb != NULL && mb->roomArrival >= 0)
            {
                const s_RandoRoomSet* rs = &RANDO_ROOMS[d->destMap];
                int k;
                for (k = 0; k < rs->count; k++)
                {
                    if (rs->points[k].pointIdx == (u8)mb->roomArrival)
                    {
                        *arrival = rs->points[k].point;
                        return;
                    }
                }
            }

            if (map_has_arrival(d->destMap))
            {
                const s_RandoArrivalSet* as = &RANDO_ARRIVALS[d->destMap];
                u8 k = d->arrivalIdx < as->count ? d->arrivalIdx : 0;
                *arrival = as->points[k];
            }
        }
        return;
    }
}

/* An area, a miniboss arena or the boss map -- the maps the mode owns. Anything
 * else (an ending map, a story map reached by some path we did not author) is left
 * completely alone. */
static int map_is_owned(int mapIdx)
{
    int i;
    if (mapIdx == RANDO_BOSS_MAP)
        return 1;
    for (i = 0; i < N_AREAS; i++)
        if (RANDO_AREAS[i] == mapIdx)
            return 1;
    return miniboss_info(mapIdx) != NULL;
}

void Pc_Rando_OnMapLoad(s32 mapIdx)
{
    const s_EventData* srcEvents;
    void (**srcFuncs)();
    const char** srcMsgs;
    int isNewEntry;

    if (!Pc_Rando_Active())
        return;

    /* Cleared before every early return below: a stale 1 would let Pc_Rando_Update
     * keep placing monsters and locking doors using the PREVIOUS area's tables
     * while the new map's own header is live. */
    s_run.headerInstalled = 0;

    /* Leaving a miniboss map: kill its death-cutscene siren, which vanilla only
     * stops in the story map you would normally exit to. s_run.mapIdx is still the
     * map we are coming FROM at this point. */
    if (mapIdx != s_run.mapIdx)
        stop_miniboss_siren(s_run.mapIdx);

    /* Only a door (or New Game) starts a new area. GameBoot_MapLoad also runs on a
     * post-death Continue, which reloads the SAME area -- counting that would burn
     * a slot off the run and re-arm every pickup in it. */
    isNewEntry         = s_run.entryPending;
    s_run.entryPending = 0;

    if (mapIdx < 0 || mapIdx >= (int)ARRAY_SIZE(RANDO_MSG_COUNT) || !map_is_owned(mapIdx))
        return;

    /* The final boss map runs the real ending chain; do not touch its events. All
     * we do is decide which of the four endings it will play. */
    if (mapIdx == RANDO_BOSS_MAP)
    {
        apply_ending();
        s_run.mapIdx = mapIdx;
        return;
    }

    s_run.mapIdx         = mapIdx;
    s_run.monstersPlaced = 0;
    s_run.monstersDone   = 0;
    s_run.entryDoor      = -1;
    s_run.entryLockTimer = 0;
    s_run.cacheFlag      = NO_VALUE;
    s_nativeCount        = 0;

    /* A miniboss arena IS its content: leave its single authored spawn row (the
     * boss) alone and add nothing to it. */
    s_run.monstersWanted = (miniboss_info(mapIdx) != NULL)
                         ? 0
                         : RANDO_MONSTERS_MIN + rnd_below(RANDO_MONSTERS_MAX - RANDO_MONSTERS_MIN + 1);

    if (isNewEntry)
    {
        /* Counted before the doors are rolled, so that the tenth area's own doors
         * are the ones that lead to the boss. */
        s_run.areasEntered++;

        /* Fresh roll on every entry, so the pickups this area already gave up come
         * back (see pickups_rearm). */
        pickups_rearm(mapIdx);
    }

    /* Game_NpcRoomInitSpawn only considers a spawn row whose per-map "still alive"
     * bit is set and whose session bit is clear. Both persist across visits, so a
     * re-entered area would otherwise come back empty. This has to happen HERE and
     * not on the placement frame: GameBoot_InGameInit runs its own spawn pass in
     * between, and on a miniboss map that pass spawns the boss and sets its session
     * bit -- clearing it afterwards would re-arm the row and spawn a second boss. */
    g_SavegamePtr->ovlEnemyStates[mapIdx] = NO_VALUE;
    g_SysWork.field_228C[0] = 0;

    /* Copy from whatever header is live: on a localized build lang_text.c has
     * already swapped in its own copy with translated messages, and we must
     * inherit those, not the DLL's English ones. */
    srcEvents = g_pMapOverlayHeader->mapEvents;
    srcFuncs  = g_pMapOverlayHeader->mapEventFuncs;
    srcMsgs   = (const char**)g_pMapOverlayHeader->mapMessages;

    if (srcEvents == NULL)
        return;

    /* Where the player is about to be put. On a randomizer jump D_800BCDB0 was
     * already overridden by Pc_Rando_ArrivalOverride; on the first area (New Game)
     * it is still zeroed and the player goes to mapPoints[0]. */
    if (D_800BCDB0.positionX != 0 || D_800BCDB0.positionZ != 0)
    {
        s_run.arrivalX = D_800BCDB0.positionX;
        s_run.arrivalZ = D_800BCDB0.positionZ;
    }
    else if (g_pMapOverlayHeader->mapPoints != NULL)
    {
        s_run.arrivalX = g_pMapOverlayHeader->mapPoints[0].positionX;
        s_run.arrivalZ = g_pMapOverlayHeader->mapPoints[0].positionZ;
    }

    build_event_funcs(srcEvents, srcFuncs);
    build_messages(mapIdx, srcMsgs);
    build_events(srcEvents, mapIdx);

    /* Re-arm a miniboss so it can be fought again on a later visit. */
    {
        const s_RandoMiniboss* mb = miniboss_info(mapIdx);
        if (mb != NULL)
        {
            int i;
            for (i = 0; i < (int)ARRAY_SIZE(mb->clearFlags); i++)
                if (mb->clearFlags[i] != 0)
                    Savegame_EventFlagClear(mb->clearFlags[i]);
            if (mb->armFlag != 0)
                Savegame_EventFlagSet(mb->armFlag);
        }
    }

    s_hdr                = *g_pMapOverlayHeader;
    s_hdr.mapEvents      = s_events;
    s_hdr.mapEventFuncs  = s_eventFuncs;
    s_hdr.mapMessages    = s_msgs;
    if (s_run.monstersWanted > 0)
        clear_native_spawns();
    g_pMapOverlayHeader  = &s_hdr;
    s_run.headerInstalled = 1;

    SH_LOG("[RANDO] area %d/%d: %s -- %d doors, %d monsters queued, score %d",
           s_run.areasEntered, RANDO_AREAS_TO_BOSS, MapRegistry_GetName((e_MapIdx)mapIdx),
           s_run.doorCount, s_run.monstersWanted, rando_score());
}

/* ------------------------------------------------------------------- per-frame */

/* Shut the door the player just walked through, for RANDO_ENTRY_LOCK_S. */
static void lock_entry_door(void)
{
    s_MapPoint2d* pts = g_MapOverlayHdr.mapPoints;
    s64 best = -1;
    int bestI = -1;
    int i;

    if (pts == NULL || s_run.doorCount <= 0)
        return;

    for (i = 0; i < s_run.doorCount; i++)
    {
        s64 dx, dz, d2;

        if (s_doors[i].scripted)
            continue; /* a miniboss exit has no doorway; locking it would fire it */

        dx = (s64)pts[s_doors[i].poi].positionX - s_run.arrivalX;
        dz = (s64)pts[s_doors[i].poi].positionZ - s_run.arrivalZ;
        d2 = dx * dx + dz * dz;
        if (best < 0 || d2 < best)
        {
            best  = d2;
            bestI = i;
        }
    }

    /* Only if it really is the doorway we came out of, not just the nearest one
     * on a big open map. */
    if (bestI < 0 || best > (s64)Q12(8.0f) * Q12(8.0f))
        return;

    s_run.entryDoor      = bestI;
    s_run.entryLockTimer = Q12((float)RANDO_ENTRY_LOCK_S);

    /* Through door_write_event, so the row is forced button-activated like any
     * other locked door -- a touch-activated one would trap the player in the
     * "It's locked." freeze loop for the whole lock window. */
    {
        s_RandoDoor shut = s_doors[bestI];
        shut.kind = DOOR_LOCKED;
        door_write_event(&s_events[s_doors[bestI].eventIdx], &shut);
    }
}

void Pc_Rando_Update(void)
{
    if (!Pc_Rando_Active() || !s_run.headerInstalled)
        return;

    if (g_GameWork.gameState != GameState_InGame)
        return;

    /* First gameplay frame in this area: collision is up, so monsters can be
     * placed on real floor, and the arrival point is final. */
    if (!s_run.monstersDone)
    {
        place_monsters();
        lock_entry_door();
    }

    /* The map's own room code resets the concurrent-NPC cap; keep it high enough
     * for everything we placed. */
    if (g_SysWork.npcFlagsId < s_run.monstersPlaced)
        g_SysWork.npcFlagsId = (u8)s_run.monstersPlaced;

    if (s_run.entryLockTimer > 0)
    {
        s_run.entryLockTimer -= g_DeltaTimeRaw;
        if (s_run.entryLockTimer <= 0 && s_run.entryDoor >= 0)
        {
            s_RandoDoor* d = &s_doors[s_run.entryDoor];
            door_write_event(&s_events[d->eventIdx], d); /* restore its real destination */
            s_run.entryDoor = -1;
        }
    }
}

/* ------------------------------------------------------------------ run start */

/* Gated on a live run, not merely on the config: loading an OLD save while the
 * mode happens to be enabled must not top up its ammo. */
int Pc_Rando_ExtraHandgunAmmo(void)
{
    return Pc_Rando_Active() ? RANDO_EXTRA_HANDGUN_AMMO : 0;
}

void Pc_Rando_OnNewGame(void)
{
    if (!s_enabled)
        return;

    memset(&s_run, 0, sizeof(s_run));
    s_run.running      = 1;
    s_run.cacheFlag    = NO_VALUE;
    s_run.entryDoor    = -1;
    s_run.mapIdx       = RANDO_START_MAP;
    s_run.entryPending = 1; /* the map load that follows is area 1 */
    s_pickupCount      = 0;

    SH_LOG("[RANDO] run started (seed %u)", s_rng);
}

void Pc_Rando_Init(void)
{
    s_enabled = g_PcConfig.randomizer != 0;
    if (!s_enabled)
        return;

    /* Seeded from the wall clock so a run differs every time. */
    s_rng = (u32)time(NULL) * 2654435761u;
    if (s_rng == 0)
        s_rng = 0x9E3779B9u;

    /* The mode always opens in map2_s04, so the launcher's Level dropdown is
     * greyed out and this simply overrides whatever the config carried. */
    strncpy(g_PcConfig.mapName, "map2_s04", sizeof(g_PcConfig.mapName) - 1);
    g_PcConfig.mapName[sizeof(g_PcConfig.mapName) - 1] = '\0';

    /* Every monster in the pool has to be spawnable in every area. */
    g_PcConfig.globalCharaPool = 1;

    SH_LOG("[RANDO] enabled -- start map forced to map2_s04, global chara pool on");
}
