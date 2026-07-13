/* Global chara pool wrapper (docs/Global_Chara_Pool.md): compiles the shared
 * floatstinger implementation without a host-map define. Include prefix
 * mirrors map4_s05.c, the retail host. Needs particle_acid.c (own TU here)
 * and the D_800D780C floor-box family (chara_global_data.c). */
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>

#include "bodyprog/bodyprog.h"
#include "bodyprog/dms.h"
#include "bodyprog/gfx/map_effects.h"
#include "bodyprog/math/math.h"
#include "bodyprog/player.h"
#include "bodyprog/sound/sound_system.h"
#include "main/rng.h"
#include "maps/map4/map4_s05.h"
#include "maps/particle.h"
#include "maps/characters/floatstinger.h"
#include "maps/characters/player.h"

#include "../src/maps/characters/floatstinger.c"
