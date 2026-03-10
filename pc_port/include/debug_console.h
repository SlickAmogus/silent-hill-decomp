#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

#ifdef SH_PC_PORT

void DebugConsole_Update(void);
void DebugConsole_Render(void);
int  DebugConsole_IsOpen(void);

#endif /* SH_PC_PORT */
#endif /* DEBUG_CONSOLE_H */
