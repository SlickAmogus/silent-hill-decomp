#define SH_LIBCD_NO_DISC_GUARD
#include <libcd.h>

#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>
#include <PsyX/PsyX_public.h>

extern SDL_Window* g_window;
extern const char* PcPort_GetGameDataPath(void);
extern const char* PcPort_GetGameDiscPath(void);

int PcPort_CdInitChecked(void)
{
    const char* discPath = PcPort_GetGameDiscPath();

    if (!discPath || !discPath[0])
    {
        const char* dataPath = PcPort_GetGameDataPath();
        char message[768];

        snprintf(message, sizeof(message),
                 "Silent Hill disc image (.bin) not found.\n\n"
                 "Place a valid .bin file in:\n%s\n\n"
                 "Then restart the game.",
                 (dataPath && dataPath[0]) ? dataPath : "./gamedata");

        if (SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                     "Missing Silent Hill disc image",
                                     message,
                                     g_window) < 0)
        {
            fprintf(stderr, "%s\nSDL error: %s\n", message, SDL_GetError());
        }

        PsyX_Shutdown();
        exit(EXIT_FAILURE);
    }

    return CdInit();
}
