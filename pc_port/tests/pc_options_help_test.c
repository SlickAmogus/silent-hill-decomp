/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pc_options_help.h"

int main(void)
{
    const char* keys[] = { "width", "fullscreen", "vsync", "texture_filter", "use_pgxp",
        "msaa", "post_process", "tonemap", "map", "flashlight_mode", "flashlight_intensity",
        "flashlight_size", "fog_strength", "disable_culling", "fps_cap", "fmv_volume",
        "weather_sim_hz", "control_2d", "mouse_sensitivity", "controller_sensitivity",
        "invert_mouse_y", "invert_controller_y", "bullet_decals", "aim_assist", "fps_fov",
        "tps_fov", "ots_fov", "tps_aim_zoom_amount", "ots_aim_zoom_amount", "tps_ots_aim",
        "tps_camera_collision", "minimap", "minimap_scale", "minimap_corner", "minimap_opacity",
        "minimap_require_map", "low_health_glow", "crosshair", "crosshair_size", "text_size" };
    const float sizes[][2] = { {640,480}, {1280,720}, {1920,1080}, {2560,1440},
        {3440,1440}, {5120,1440}, {1280,1024}, {600,900}, {3840,2160} };
    unsigned i;
    for(i=0;i<sizeof(keys)/sizeof(keys[0]);i++) {
        const PcOptionHelp* h=Pc_OptionsHelp(keys[i]);
        assert(h && h->name[0][0] && h->name[1][0]);
        assert(h->description[0][0] && h->description[1][0]);
        assert(strcmp(h->description[0],h->description[1]));
    }
    assert(!Pc_OptionsHelp(NULL));
    assert(!Pc_OptionsHelp("unknown-setting"));
    assert(Pc_OptionsPreviewScene("fog_strength")==PC_SCENE_STREET);
    assert(Pc_OptionsPreviewScene("weather_sim_hz")==PC_SCENE_WEATHER);
    assert(Pc_OptionsPreviewScene("fps_fov")==PC_SCENE_SCHOOL);
    assert(Pc_OptionsPreviewScene("flashlight_intensity")==PC_SCENE_SCHOOL);
    assert(Pc_OptionsPreviewScene("control_2d")==PC_SCENE_NONE);
    assert(Pc_OptionsPreviewScene("msaa")==PC_SCENE_NONE);
    assert(Pc_OptionsPreviewScene(NULL)==PC_SCENE_NONE);
    for(i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        PcOptionsLayout l=Pc_OptionsLayout(sizes[i][0],sizes[i][1]);
        assert(l.left>=0 && l.top>=0 && l.width>0 && l.height>0);
        assert(l.left+l.width<=sizes[i][0] && l.top+l.height<=sizes[i][1]);
        assert(l.font>=10 && l.listWidth<=l.width);
        if (!l.stacked) assert(l.width-l.listWidth-l.gap>0);
    }
    assert(strstr(Pc_OptionsHelp("disable_culling")->name[1],"Culling"));
    assert(strstr(Pc_OptionsHelp("control_2d")->name[1],"2D"));
    assert(strstr(Pc_OptionsHelp("tps_fov")->name[1],"FOV"));
    puts("English/Russian help coverage and responsive layouts OK");
    return 0;
}
