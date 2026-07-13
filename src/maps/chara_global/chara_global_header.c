/* Global chara pool pseudo-map header (docs/Global_Chara_Pool.md).
 *
 * Not a real map: never routed through MapOverlay_Load. pc_chara_pool.c opens
 * this DLL once at boot with its own handle (never freed) and, on every map
 * load, copies each charaUpdateFuncs entry into slots the active map's header
 * left NULL. Only charaUpdateFuncs is meaningful here — everything else stays
 * zero and must never be read.
 *
 * Variant note: this DLL compiles WITHOUT any MAPx_Sxx define, so every
 * per-map #if inside the character sources picks the generic/"wild" branch
 * (stalker full variant, air screamer wild attack set, groaner standard
 * tuning). Native maps never see these copies — NULL-only backfill.
 *
 * Excluded (map-bound implementations): Twinfeeler (live code inline in
 * map4_s03.c with a runtime hot-swap), Incubus/Unknown23 (map7_s03 boss-FX
 * function family), LockerDeadBody (map1_s03-local), Chicken (no AI exists
 * on disc), cutscene actors (per-map cutscene state). They spawn as posed
 * statues via the asset pool; AI restoration is a separate feature. */

#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/map/map.h"
#include "maps/characters/air_screamer.h"
#include "maps/characters/bloodsucker.h"
#include "maps/characters/creeper.h"
#include "maps/characters/floatstinger.h"
#include "maps/characters/groaner.h"
#include "maps/characters/hanged_scratcher.h"
#include "maps/characters/larval_stalker.h"
#include "maps/characters/monster_cybil.h"
#include "maps/characters/puppet_nurse.h"
#include "maps/characters/romper.h"
#include "maps/characters/split_head.h"
#include "maps/characters/stalker.h"

SH_MAP_OVERLAY_HEADER = {
    .charaUpdateFuncs = {
        [Chara_AirScreamer]     = AirScreamer_Update,
        [Chara_NightFlutter]    = AirScreamer_Update,
        [Chara_Groaner]         = Groaner_Update,
        [Chara_Wormhead]        = Groaner_Update,
        [Chara_LarvalStalker]   = LarvalStalker_Update,
        [Chara_Stalker]         = Stalker_Update,
        [Chara_GreyChild]       = Stalker_Update,
        [Chara_Mumbler]         = Stalker_Update,
        [Chara_HangedScratcher] = HangedScratcher_Update,
        [Chara_Creeper]         = Creeper_Update,
        [Chara_Romper]          = Romper_Update,
        [Chara_SplitHead]       = SplitHead_Update,
        [Chara_Floatstinger]    = Floatstinger_Update,
        [Chara_PuppetNurse]     = PuppetNurse_Update,
        [Chara_DummyNurse]      = PuppetNurse_Update,
        [Chara_PuppetDoctor]    = PuppetDoctor_Update,
        [Chara_DummyDoctor]     = PuppetDoctor_Update,
        [Chara_Bloodsucker]     = Bloodsucker_Update,
        [Chara_MonsterCybil]    = MonsterCybil_Update,
    },
};
