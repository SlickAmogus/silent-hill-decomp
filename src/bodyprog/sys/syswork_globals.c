#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "game.h"

s_SysWork  g_SysWork;
s_GameWork g_GameWork;

#ifdef SH_XBOX_PORT
/* Torn-layout tripwire. This TU allocates g_SysWork/g_GameWork; if it is ever
 * linked stale against newer headers (the .c.d-missing dep hole — a Jul 2026
 * build shipped 98 pre-merge objects and every post-npcs g_SysWork access
 * landed in foreign globals), other TUs' sizeof will disagree with the
 * allocation. game_main.c compares at MainLoop entry and logs [FATAL-BUILD]. */
unsigned SysWorkGlobals_SizeofSysWork(void)  { return (unsigned)sizeof(s_SysWork); }
unsigned SysWorkGlobals_SizeofGameWork(void) { return (unsigned)sizeof(s_GameWork); }
#endif

s_GameWork* const       g_GameWorkConst = &g_GameWork;
s_Savegame* const       g_SavegamePtr   = &g_GameWork.savegame;
s_ControllerData* const g_Controller0   = &g_GameWork.controllers[0];
s_ControllerData* const g_Controller1   = &g_GameWork.controllers[1];
s_GameWork* const       g_GameWorkPtr   = &g_GameWork;

const u32 D_80024D58 = 0; // Nothing references it. Might be just padding.
