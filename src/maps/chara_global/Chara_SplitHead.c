/* Global chara pool wrapper (docs/Global_Chara_Pool.md): compiles the shared
 * split_head implementation without a host-map define. Include prefix mirrors
 * map1_s05.c, the retail host. Needs unk_draw.c (compiled as its own TU here)
 * and the sharedData_800D5xxx_1_s05 tables (chara_global_data.c). */
#include "bodyprog/bodyprog.h"
#include "bodyprog/math/math.h"
#include "bodyprog/sound/sound_system.h"
#include "main/rng.h"
#include "maps/map1/map1_s05.h"
#include "maps/particle.h"
#include "maps/characters/player.h"
#include "inline_no_dmpsx.h"

#include <psyq/gtemac.h>

#include "maps/characters/split_head.h"

#include "../src/maps/characters/split_head.c"
