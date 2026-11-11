#ifndef PC_GAME_OPTIONS_H
#define PC_GAME_OPTIONS_H

/* config.cfg persistence for the original game's own options (volumes,
 * vibration, brightness level, the Extra menu...). See pc_game_options.c. */

/* Returns 1 when the key was one of ours. */
int  Pc_GameOptions_ConfigKey(const char* key, const char* value);
/* Writes the values read from config.cfg into g_GameWork.config. */
void Pc_GameOptions_Apply(void);
/* Saves every option that changed since the last Apply/Persist. */
void Pc_GameOptions_Persist(void);

#endif
