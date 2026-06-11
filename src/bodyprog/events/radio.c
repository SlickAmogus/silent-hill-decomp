#include "game.h"

#include <psyq/libetc.h>
#include <psyq/libpad.h>
#include <psyq/strings.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/events/radio.h"
#include "bodyprog/sound/sound_system.h"

#ifdef SH_PC_PORT
#include "sh_log.h"
#endif

void func_80037154(void) // 0x80037154
{
    s32 i;

    for (i = 0; i < ARRAY_SIZE(D_800BCDA8); i++)
    {
        D_800BCDA8[i].field_2 = NO_VALUE;
        D_800BCDA8[i].field_1 = NO_VALUE;
        D_800BCDA8[i].field_3 = 0;
#ifdef SH_PC_PORT
        /* Also reset field_0 so the radio keyon path in Game_NpcUpdate
         * (`if (field_0 == NO_VALUE)`) fires on the first frame an enemy
         * enters detection range. Vanilla relies on Game_RadioSoundStop
         * being called on every overlay transition to reset field_0, but
         * LoadSave (ProcessFlag 0x8) and NewGame paths skip that — so on
         * a fresh load with an AS already spawned at radio range, field_0
         * stays BSS-zero and the volume update branch runs forever instead
         * of the keyon branch. Fixes silent radio near wild Air Screamer
         * after loading a savegame in the streets (map2_s00).
         *
         * MUST also stop the loop voice: clearing field_0 alone skips the
         * vanilla stop branch in Game_NpcUpdate ("was playing, no enemy
         * now" -> Sd_SfxStop), so walking through a door with enemies in
         * radio range left the static playing FOREVER in the next room
         * (alley gate, Levin St house reports). On a fresh load nothing
         * is playing, so the extra stop is a no-op there. */
        D_800BCDA8[i].field_0 = NO_VALUE;
        Sd_SfxStop(Sfx_RadioInterferenceLoop + i);
#endif
    }
}

void Game_RadioSoundStop(void) // 0x80037188
{
    s32 i;

    for (i = 0; i < ARRAY_SIZE(D_800BCDA8); i++)
    {
        D_800BCDA8[i].field_0 = NO_VALUE;
    }

    for (i = 0; i < ARRAY_SIZE(D_800BCDA8); i++)
    {
        Sd_SfxStop(Sfx_RadioInterferenceLoop + i);
    }
}
