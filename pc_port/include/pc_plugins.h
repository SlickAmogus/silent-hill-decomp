#ifndef PC_PLUGINS_H
#define PC_PLUGINS_H

#include "game.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Silent Hill PC Port - Standard Plugin Lifecycle Signatures
 * Plugins can export any of these standard engine lifecycle hooks.
 */
typedef void        (*SH_Plugin_InitFunc)(void);
typedef void        (*SH_Plugin_ShutdownFunc)(void);
typedef void        (*SH_Plugin_NewGameFunc)(void);
typedef void        (*SH_Plugin_MapLoadFunc)(s32 mapIdx);
typedef void        (*SH_Plugin_UpdateFunc)(void);
typedef void        (*SH_Plugin_RenderFunc)(void);
typedef const char* (*SH_Plugin_GetNameFunc)(void);
typedef s32         (*SH_Plugin_GetApiVersionFunc)(void);

/*
 * Core Engine Plugin Manager Interface
 */
void Pc_Plugins_Init(void);
void Pc_Plugins_Shutdown(void);
void Pc_Plugins_OnNewGame(void);
void Pc_Plugins_OnMapLoad(s32 mapIdx);
void Pc_Plugins_OnUpdate(void);
void Pc_Plugins_OnRender(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_PLUGINS_H */
