#ifndef PC_PLUGINS_H
#define PC_PLUGINS_H

#include "game.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Plugin API callback signatures */
typedef void           (*SH_Plugin_InitFunc)(void);
typedef void           (*SH_Plugin_NewGameFunc)(void);
typedef void           (*SH_Plugin_MapLoadFunc)(s32 mapIdx);
typedef void           (*SH_Plugin_UpdateFunc)(void);
typedef void           (*SH_Plugin_PlayerDamageFunc)(s32* damage);
typedef int            (*SH_Plugin_HideHealthFunc)(void);
typedef int            (*SH_Plugin_WeatherFunc)(s32* ambient, s32* rain);
typedef void           (*SH_Plugin_ScreenFadeFunc)(void);
typedef int            (*SH_Plugin_NpcSpawnFunc)(e_CharaId* charaId);
typedef void           (*SH_Plugin_RadioVolumeFunc)(s32* volume);
typedef void           (*SH_Plugin_RadioAttributesFunc)(s32* volume, s32* pitch);
typedef int            (*SH_Plugin_LiveInventoryFunc)(void);

/* Core Engine Plugin Manager API */
void           Pc_Plugins_Init(void);
void           Pc_Plugins_Shutdown(void);
void           Pc_Plugins_OnNewGame(void);
void           Pc_Plugins_OnMapLoad(s32 mapIdx);
void           Pc_Plugins_OnUpdate(void);
void           Pc_Plugins_OnPlayerDamage(s32* damage);
int            Pc_Plugins_ShouldHideHealth(void);
int            Pc_Plugins_OverrideWeather(s32* ambient, s32* rain);
void           Pc_Plugins_OnScreenFadeDraw(void);
int            Pc_Plugins_OverrideNpcSpawn(e_CharaId* charaId);
void           Pc_Plugins_ModifyRadioVolume(s32* volume);
void           Pc_Plugins_ModifyRadioAttributes(s32* volume, s32* pitch);
int            Pc_Plugins_IsLiveInventoryEnabled(void);
int            Pc_Plugins_HasNightmarePlugin(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_PLUGINS_H */
