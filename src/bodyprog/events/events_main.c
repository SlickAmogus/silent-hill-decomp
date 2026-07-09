#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/bodyprog_data_800A99B4.h"
#include "bodyprog/events/events_main.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/math/math.h"
#include "sh_log.h"

#ifdef SH_XBOX_PORT
#include "bodyprog/screen/screen_fade.h" /* recovery fade-in when a transition is refused */
/* map_xbox.c. Only map0_s00 is static-linked into the xbe; a SysState_LoadOverlay
 * event for any other map must be refused HERE, before it dispatches. On PSX the
 * overlay swap replaces the events table, which is what stops TriggerType_None
 * overlay events from re-firing; with the fixed map0_s00 header they would fire
 * every frame forever (the end-of-intro GameState 10<->11 loop). */
extern int  MapXbox_OverlayIsLinked(int mapIdx);
extern void MapXbox_LogUnlinkedOverlay(int mapIdx, const char* where);
#endif

// 0x80037388 — un-nested from Event_Update: clang/nxdk does not support GCC's
// nested-function extension. A file-scope static is behavior-identical (no parent
// locals were captured) and still compiles fine under the PC port's gcc.
static void Event_ItemTriggersClear(void)
{
    s32 i;

    for (i = 0; i < 5; i++)
    {
        g_ItemTriggerItemIds[i] = NO_VALUE;
        g_ItemTriggerEvents[i]  = NULL;
    }
}

void Event_Update(bool disableButtonEvents) // 0x800373CC
{
    s_MapPoint2d* mapPoint;
    s_EventData*  mapEvent;
    q19_12        pointPosX;
    q19_12        pointPosZ;
    q19_12        pointRadiusX;
    q19_12        pointRadiusZ;
    s32           i;

    // `lastUsedItem` is set by `Inventory_ItemUse` when player uses an item that matches one of the item trigger events.
    // If it's set, find its index in `g_ItemTriggerItemIds` and use that to get the corresponding `s_EventData` from `g_ItemTriggerEvents`.
    // After processing, the field is cleared and item trigger IDs are reset.
    // (Multi-item events likely repopulate the trigger IDs below based on whichever events are still active?)
    if (g_SysWork.playerWork.extra.lastUsedItem != InvItemId_Unequipped)
    {
#ifdef SH_PC_PORT
        /* On PSX the item ID is guaranteed to be in g_ItemTriggerItemIds[] (it
         * was set by Inventory_ItemUse only when a matching trigger existed).
         * On PC, stale state can leave lastUsedItem set after the trigger
         * slots were cleared, causing an unbounded scan into adjacent memory.
         * The bounds check must be in the loop CONDITION to prevent the OOB
         * read from happening in the first place. */
        for (i = 0; i < 5 && g_SysWork.playerWork.extra.lastUsedItem != g_ItemTriggerItemIds[i]; i++);
        if (i >= 5) {
            g_SysWork.playerWork.extra.lastUsedItem = InvItemId_Unequipped;
            Event_ItemTriggersClear();
            return;
        }
#else
        for (i = 0; g_SysWork.playerWork.extra.lastUsedItem != g_ItemTriggerItemIds[i]; i++);
#endif

        g_MapEventData         = g_ItemTriggerEvents[i];
        g_MapEventLastUsedItem = g_SysWork.playerWork.extra.lastUsedItem;
        g_MapEventSysState     = g_MapEventData->sysState;
        g_MapEventParam        = g_MapEventData->eventParam;

#ifdef SH_XBOX_PORT
        /* Item-activated inter-map door (key on a map-boundary door) whose
         * target overlay is not linked: swallow the event instead of
         * dispatching a transition that cannot complete. */
        if (g_MapEventData->sysState == SysState_LoadOverlay &&
            !MapXbox_OverlayIsLinked(g_MapEventData->mapIdx))
        {
            MapXbox_LogUnlinkedOverlay(g_MapEventData->mapIdx, "Event_Update(item)");
            g_MapEventData     = NULL;
            g_MapEventSysState = SysState_Invalid;
            g_MapEventParam    = 0;
        }
#endif

        g_SysWork.playerWork.extra.lastUsedItem = InvItemId_Unequipped;
        Event_ItemTriggersClear();
        return;
    }

    Event_ItemTriggersClear();

    g_MapEventLastUsedItem = InvItemId_Unequipped;

    mapEvent = &g_MapOverlayHdr.mapEvents[-1];

#ifdef SH_PC_PORT
    /* Hard iteration cap. Sentinel triggerType=NO_VALUE (-1) stored in
     * the s8:4 bitfield should stop iteration after the last entry, but
     * recent traces show iteration walking to mapEventIdx=60+ on map0_s01
     * (which only has 23 entries). The OOB read produces garbage event
     * data with eventParam=14 — game then dispatches into garbage memory
     * and triggers spurious item pickups (KeyOfWoodman dog-head freeze).
     * Likely a bitfield sign-extension or struct alignment difference
     * vs PSX. Cap iteration to MAX_MAP_EVENTS so we don't walk wild. */
    const int MAX_MAP_EVENTS = 256; /* map7_s02 has 251 events, the largest across all maps */
    int _eventIdx = -1;
#endif

    while (true)
    {
        s32 disabledEventFlag_temp;
        s16 disabledEventFlag;
        s16 requiredEventFlag;

        mapEvent++;
#ifdef SH_PC_PORT
        _eventIdx++;
        if (_eventIdx >= MAX_MAP_EVENTS) {
            break;
        }
#endif

        if (mapEvent->triggerType == NO_VALUE)
        {
            break;
        }

        // `requiredEventFlag`: if set, EventFlag that must be set for event to trigger?
        // `disabledEventFlag`: if set, EventFlag that must not be set for event to trigger?
        // TODO: Can this s32 temp be removed? Trying to set `disabledEventFlag` directly results in `lhu` instead?
        requiredEventFlag      = mapEvent->requiredEventFlag;
        disabledEventFlag_temp = mapEvent->disabledEventFlag;
        disabledEventFlag      = disabledEventFlag_temp;

        if (requiredEventFlag != EventFlag_None && !Savegame_EventFlagGet(requiredEventFlag))
        {
            continue;
        }

        if (disabledEventFlag != EventFlag_None && Savegame_EventFlagGet(disabledEventFlag) &&
            (disabledEventFlag < 867 || mapEvent->activationType == TriggerActivationType_Exclusive ||
             mapEvent->sysState == SysState_EventSetFlag))
        {
            continue;
        }

        // `TriggerType_None` skips any trigger/activation check and always executes.
        // Maybe used for map-load events, and events that should run every frame?
        // Returns before processing other events until flag checks above disable it.
        if (mapEvent->triggerType == TriggerType_None)
        {
#ifdef SH_XBOX_PORT
            /* THE end-of-intro case: map0_s00's SysState_LoadOverlay event for
             * MAP0_S01 (requiredEventFlag=EventFlag_26, no disable flag) fires
             * every frame once the death cutscene sets the flag. On PSX the
             * overlay swap removes it; here the target map is not linked, so
             * skip it (and keep scanning other events) instead of looping the
             * transition forever. One-shot: the cutscene that armed this event
             * usually ends faded out, expecting the map switch to fade back
             * in — do that ourselves or the game looks hung on a black screen. */
            if (mapEvent->sysState == SysState_LoadOverlay &&
                !MapXbox_OverlayIsLinked(mapEvent->mapIdx))
            {
                static s_EventData* s_lastRefusedEvent = NULL;

                MapXbox_LogUnlinkedOverlay(mapEvent->mapIdx, "Event_Update(TriggerType_None)");

                if (s_lastRefusedEvent != mapEvent)
                {
                    s_lastRefusedEvent = mapEvent;
                    if (!ScreenFade_IsNone())
                    {
                        ScreenFade_Start(true, true, IS_SCREEN_FADE_WHITE(g_Screen_FadeStatus));
                        SH_DBG("[MAP-GUARD] kicked recovery fade-in (fadeStatus was 0x%x)",
                               (unsigned)g_Screen_FadeStatus);
                    }
                }
                continue;
            }
#endif
            g_MapEventData     = mapEvent;
            g_MapEventSysState = mapEvent->sysState;
            g_MapEventParam    = mapEvent->eventParam;
            return;
        }

        // `TriggerActivationType_Button`: Only continue processing event when action button is pressed and
        // `Player_IsBusy` returns `false`.
        if (mapEvent->activationType == TriggerActivationType_Button &&
            (!(g_Controller0->clickedBtnFlags & g_GameWorkPtr->config.controllerConfig.action) ||
            disableButtonEvents || Player_IsBusy()))
        {
            continue;
        }

        mapPoint = &g_MapOverlayHdr.mapPoints[mapEvent->pointOfInterestIdx];

        switch (mapEvent->triggerType)
        {
            case TriggerType_TouchAabb:
                pointPosX    = mapPoint->positionX;
                pointPosZ    = mapPoint->positionZ;
                pointRadiusX = mapPoint->triggerParam0 * Q12(0.25f);
                pointRadiusZ = mapPoint->triggerParam1 * Q12(0.25f);

                if (ABS(g_SysWork.playerWork.player.position.vx - pointPosX) > pointRadiusX)
                {
                    continue;
                }

                if (ABS(g_SysWork.playerWork.player.position.vz - pointPosZ) > pointRadiusZ)
                {
                    continue;
                }
                break;

            case TriggerType_TouchFacing:
                if (!Event_CollideFacingCheck(mapPoint))
                {
                    continue;
                }
                break;

            case TriggerType_TouchObbFacing:
                if (!Event_CollideObbFacingCheck(mapPoint))
                {
                    continue;
                }
                break;

            case TriggerType_TouchObb:
                if (!Event_CollideObbCheck(mapPoint))
                {
                    continue;
                }
                break;
        }

        // Trigger checks have passed. Check activation type.

        // `TriggerActivationType_Exclusive`: Skip processing any other events if this event is active.
        if (mapEvent->activationType == TriggerActivationType_Exclusive && mapEvent == g_MapEventData)
        {
            g_MapEventSysState = SysState_Invalid;
            return;
        }

        // `TriggerActivationType_Item`: When trigger check has passed (player is in the trigger area).
        // Required item ID for event is stored into `g_ItemTriggerItemIds` and event pointer at `g_ItemTriggerEvents`
        // Once player uses an item in the inventory screen, it compares the ID against the ones stored at `g_ItemTriggerItemIds`.
        // If used item ID matches one that event has requested, `extra.lastUsedItem` gets set to the item ID.
        // At the start of this function, if `extra.lastUsedItem` is set, it will locate the `s_EventData` for it from `g_ItemTriggerEvents` and run the event.
        if (mapEvent->activationType == TriggerActivationType_Item)
        {
            for (i = 0; g_ItemTriggerItemIds[i] != NO_VALUE; i++);

            g_ItemTriggerEvents[i]  = mapEvent;
            g_ItemTriggerItemIds[i] = mapEvent->requiredItemId;
            continue;
        }

        // `TriggerActivationType_Button`: Only allow button activated events when area is lit up?
        if (mapEvent->activationType == TriggerActivationType_Button)
        {
            if ((g_SysWork.field_2388.field_154.effectsInfo_0.field_0.s_field_0.field_0 & 2) && !g_SysWork.field_2388.isFlashlightOn_15 &&
                ((g_SysWork.field_2388.field_1C[0].effectsInfo_0.field_0.s_field_0.field_0 & 1) || (g_SysWork.field_2388.field_1C[1].effectsInfo_0.field_0.s_field_0.field_0 & 1)))
            {
                if (mapEvent->sysState != SysState_LoadOverlay &&
                    (mapEvent->sysState != SysState_LoadRoom && mapEvent->eventParam > 1))
                {
                    continue;
                }
            }
        }

        // Trigger and activation checks passed. Run event.

        // If this is `EventSetFlag`, handle setting the flag here and skip running it.
        // (Same as `SysState_EventSetFlag_Update`.)
        if (mapEvent->sysState == SysState_EventSetFlag)
        {
            Savegame_EventFlagSetAlt(mapEvent->disabledEventFlag);
            break;
        }

        // Set `g_MapEventSysState` to the SysState needed for the event to be ran on next tick (`SysState_ReadMessage`/`SaveMenu`/`EventCallFunc`/etc.).
#ifdef SH_XBOX_PORT
        /* Touched/button-activated inter-map door whose target overlay is not
         * linked (e.g. walking back up the street to a map0_s02 boundary):
         * refuse the transition, keep the player in the current map. */
        if (mapEvent->sysState == SysState_LoadOverlay &&
            !MapXbox_OverlayIsLinked(mapEvent->mapIdx))
        {
            MapXbox_LogUnlinkedOverlay(mapEvent->mapIdx, "Event_Update(trigger)");
            continue;
        }
#endif
        g_MapEventData     = mapEvent;
        g_MapEventSysState = mapEvent->sysState;
        g_MapEventParam    = mapEvent->eventParam;
        return;
    }

    g_MapEventData     = NULL;
    g_MapEventSysState = SysState_Invalid;
    g_MapEventParam    = 0;
}

bool Event_CollideFacingCheck(s_MapPoint2d* mapPoint) // 0x800378D4
{
    q19_12     deltaX;
    q19_12     deltaZ;
    q3_12      rotY;
    q19_12     deltaRotY;
    static s32 D_800A9A20 = 0;
    static s32 D_800A9A24 = 0;
    static s32 D_800A9A28 = 0;

    if (g_TickCount > D_800A9A20)
    {
        rotY       = g_SysWork.playerWork.player.rotation.vy;
        D_800A9A24 = g_SysWork.playerWork.player.position.vx - (Math_Sin(rotY) >> 3); // `/ 8`.
        D_800A9A28 = g_SysWork.playerWork.player.position.vz - (Math_Cos(rotY) >> 3); // `/ 8`.
        D_800A9A20 = g_TickCount;
    }

    deltaX = mapPoint->positionX - D_800A9A24;
    if (ABS(deltaX) > Q12(0.8f))
    {
        return false;
    }

    deltaZ = mapPoint->positionZ - D_800A9A28;
    if (ABS(deltaZ) > Q12(0.8f))
    {
        return false;
    }

    if ((SQUARE(deltaX) + SQUARE(deltaZ)) > SQUARE(Q12(0.8f)))
    {
        return false;
    }

    deltaRotY = g_SysWork.playerWork.player.rotation.vy - ratan2(deltaX, deltaZ);
    if (deltaRotY >= Q12_ANGLE(180.0f))
    {
        deltaRotY -= Q12_ANGLE(360.0f);
    }

    if (Q12_ANGLE(30.0f) < ABS(deltaRotY))
    {
        return false;
    }
    else
    {
        return true;
    }
}

bool Event_CollideObbFacingCheck(s_MapPoint2d* mapPoint) // 0x80037A4C
{
    s32    temp_a0_2;
    s32    temp_a2;
    q19_12 halfCosPlayerRotY;
    s32    temp_s2;
    s32    halfSinRotY;
    s32    temp_s4;
    q19_12 deltaX;
    q19_12 deltaZ;
    s32    temp_v1;
    q19_12 clampedHalfCosPlayerRotY;
    bool   cond;
    s32    scaledSinPlayerRotY;
    s32    scaledCosRotY;

    halfSinRotY   = Math_Sin(g_SysWork.playerWork.player.rotation.vy) >> 1; // `/ 2`.
    scaledCosRotY = -Math_Cos(Q12_ANGLE_FROM_Q8(mapPoint->triggerParam0)) * mapPoint->triggerParam1;

    clampedHalfCosPlayerRotY = halfSinRotY;

    temp_a0_2 = scaledCosRotY >> 4; // `/ 16`.
    deltaX    = mapPoint->positionX - g_SysWork.playerWork.player.position.vx;
    temp_s2   = deltaX - temp_a0_2;
    temp_s4   = deltaX + temp_a0_2;

    clampedHalfCosPlayerRotY = MAX(halfSinRotY, 0);

    if (temp_s4 >= temp_s2)
    {
        cond = clampedHalfCosPlayerRotY < temp_s2;
    }
    else
    {
        cond = clampedHalfCosPlayerRotY < temp_s4;
    }

    if (!cond)
    {
        if (MIN(halfSinRotY, 0) <= MAX(temp_s2, temp_s4))
        {
            halfCosPlayerRotY   = Math_Cos(g_SysWork.playerWork.player.rotation.vy) >> 1; // `/ 2`.
            scaledSinPlayerRotY = Math_Sin(Q12_ANGLE_FROM_Q8(mapPoint->triggerParam0)) *
                                  mapPoint->triggerParam1;

            clampedHalfCosPlayerRotY = halfCosPlayerRotY;

            temp_a0_2 = scaledSinPlayerRotY >> 4; // `/ 16`.
            deltaZ    = mapPoint->positionZ - g_SysWork.playerWork.player.position.vz;
            temp_v1   = deltaZ - temp_a0_2;
            temp_a2   = deltaZ + temp_a0_2;

            clampedHalfCosPlayerRotY = MAX(halfCosPlayerRotY, Q12(0.0f));

            if (temp_a2 >= temp_v1)
            {
                cond = clampedHalfCosPlayerRotY < temp_v1;
            }
            else
            {
                cond = clampedHalfCosPlayerRotY < temp_a2;
            }

            if (!cond)
            {
                if (MIN(halfCosPlayerRotY, 0) <= MAX(temp_v1, temp_a2) &&
                   (((temp_s2 * halfCosPlayerRotY) - (halfSinRotY * temp_v1)) <= 0) && ((temp_s4 * halfCosPlayerRotY) - (halfSinRotY * temp_a2)) >= 0 &&
                   ((-temp_s2 * (temp_a2 - temp_v1)) + ((temp_s4 - temp_s2) * temp_v1)) >= 0)
                {
                    return (((halfSinRotY - temp_s2) * (temp_a2 - temp_v1)) -
                            ((temp_s4 - temp_s2) * (halfCosPlayerRotY - temp_v1))) < 1;
                }
            }
        }
    }

    return false;
}

bool Event_CollideObbCheck(s_MapPoint2d* mapPoint) // 0x80037C5C
{
    q19_12 sinAngle;
    q19_12 cosAngle;
    q19_12 angle;
    q19_12 deltaX;
    q19_12 deltaZ;
    s32    shift8Field_7;
    s32    temp_v0;
    s32    scale;
    u32    temp;

    shift8Field_7 = mapPoint->triggerParam1 << 8;

    deltaX = g_SysWork.playerWork.player.position.vx - mapPoint->positionX;
    if (mapPoint->triggerParam1 << 9 < ABS(deltaX))
    {
        return false;
    }

    deltaZ = g_SysWork.playerWork.player.position.vz - mapPoint->positionZ;
    scale  = 2;
    if ((shift8Field_7 * scale) < ABS(deltaZ))
    {
        return false;
    }

    // TODO: Odd Q8 angle conversion method. `Q12_ANGLE_FROM_Q8` doesn't match here.
    angle    = -(mapPoint->triggerParam0 << 20) >> 16;
    sinAngle = Math_Sin(angle);

    temp = FP_FROM((-deltaX * sinAngle) + (deltaZ * Math_Cos(angle)), Q12_SHIFT);
    if (temp > Q12(4.0f))
    {
        return false;
    }

    cosAngle = Math_Cos(angle);
    temp_v0  = FP_FROM((deltaX * cosAngle) + (deltaZ * Math_Sin(angle)), Q12_SHIFT);
    if (shift8Field_7 < ABS(temp_v0))
    {
        return false;
    }
    else
    {
        return true;
    }
}
