/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MOD_REGISTRY_H
#define PC_MOD_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

/* A mod console command: receives the argument string after the command word
 * (already trimmed of leading spaces; "" when there was none). Print output
 * with the exported Pc_Console_Print. */
typedef void (*Pc_ModCommandFn)(const char* arg);

/* Register a console command from a map DLL. Call it from the overlay's init
 * path. persistent=0 (normal map DLL): cleared when that map unloads.
 * persistent=1: only for a DLL that lives the whole session (chara_global.dll)
 * -- a transient DLL passing 1 would leave a dangling handler on unload.
 * Built-in commands always win, and a duplicate name is ignored. Returns 1 on
 * success. */
int Pc_ModConsole_Register(const char* name, Pc_ModCommandFn fn, const char* help, int persistent);

/* Dispatcher-internal: try a registered command. 1 = handled. */
int Pc_ModConsole_Dispatch(const char* cmd, const char* arg);

/* Drop every non-persistent command; called at the overlay unload choke point. */
void Pc_ModConsole_ClearTransient(void);

/* Enumerate registered commands for HELP. 1 while idx is valid. */
int Pc_ModConsole_List(const char** outName, const char** outHelp, int idx);

/* Config: the exe's parser stashes lines it does not recognize here; a mod
 * reads its own keys at init. Values are strings (parse with atoi/atof). */
void        Pc_ModConfig_Store(const char* key, const char* value);
const char* Pc_ModConfig_Value(const char* key); /* NULL if unset */

/* ASCII case-insensitive string equality (shared helper). */
int Pc_StrCaseEq(const char* a, const char* b);

/* Console output for a mod command (routes to the debug overlay). Defined in
 * pc_console_cmd.c. */
void Pc_Console_Print(const char* text);

#ifdef __cplusplus
}
#endif

#endif /* PC_MOD_REGISTRY_H */
