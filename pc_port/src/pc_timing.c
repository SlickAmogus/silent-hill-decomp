#include "game.h"
#include "bodyprog/math/fixed_point.h"
#include "bodyprog/screen/screen_data.h"
#include "pc_timing.h"

bool PC_Tick30HzReady(int* accumulator_q12)
{
    *accumulator_q12 += g_DeltaTime;
    if (*accumulator_q12 >= TIMESTEP_30_FPS)
    {
        *accumulator_q12 -= TIMESTEP_30_FPS;
        /* Clamp on huge stalls (alt-tab, paused breakpoint) so we don't
         * fire a burst of ticks afterwards. */
        if (*accumulator_q12 > TIMESTEP_30_FPS)
        {
            *accumulator_q12 = 0;
        }
        return true;
    }
    return false;
}
