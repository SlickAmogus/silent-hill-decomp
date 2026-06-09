#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/player_pos_update.h"
#include "bodyprog/math/math.h"

void Chara_PositionSet(s_MapPoint2d* mapPoint) // 0x800371E8
{
    q19_12 rotY;

    #define playerChara g_SysWork.playerWork.player

    rotY = Q12_ANGLE_FROM_Q8(mapPoint->triggerParam0);
    Math_SVectorSet(&playerChara.rotation, Q12_ANGLE(0.0f), rotY, Q12_ANGLE(0.0f));

    playerChara.position.vy = Q12(0.0f);
    playerChara.position.vx = mapPoint->positionX;
    playerChara.position.vz = mapPoint->positionZ;

    if (mapPoint->triggerParam1 >= 2)
    {
        playerChara.position.vx += Q12_MULT_FLOAT_PRECISE(Math_Sin(rotY), 0.4f);
        playerChara.position.vz += Q12_MULT_FLOAT_PRECISE(Math_Cos(rotY), 0.4f);
    }

    g_SysWork.loadingScreenIdx = mapPoint->loadingScreenId;

    if (mapPoint->paperMapIdx == 24) // TODO: Demagic 24.
    {
        g_SavegamePtr->paperMapIdx = PaperMapIdx_OtherPlaces;
    }
    else if (mapPoint->paperMapIdx != PaperMapIdx_OtherPlaces)
    {
        g_SavegamePtr->paperMapIdx = mapPoint->paperMapIdx;
    }

    g_SysWork.cameraAngleY = rotY;

    func_8007E9C4();
    Game_MapRoomIdxUpdate();

    #undef playerChara
}

void Game_PlayerHeightUpdate(void) // 0x80037334
{
    s_CollisionSurface coll;

    if (g_MapOverlayHdr.updateWorldObjects != NULL)
    {
        g_MapOverlayHdr.updateWorldObjects();
    }

    Collision_SurfaceGet(&coll, g_SysWork.playerWork.player.position.vx, g_SysWork.playerWork.player.position.vz);
#ifdef SH_PC_PORT
    /* Only apply if collision data is valid (sentinel = Q12(8.0f) means no chunk loaded) */
    if (coll.groundHeight != Q12(8.0f)) {
        g_SysWork.playerWork.player.position.vy = coll.groundHeight;
        /* Also initialize positionY -- the floor collision system uses this as
         * the "previous ground target" on frames where collision data is missing
         * (field_14 == 0). Without this, the first movement tick can wrongly snap
         * Harry to Y=0 if positionY was never set (stays at 0 from bzero). */
        g_SysWork.playerWork.player.properties.player.groundHeight = coll.groundHeight;
    }
#else
    g_SysWork.playerWork.player.position.vy = coll.groundHeight;
#endif
}
