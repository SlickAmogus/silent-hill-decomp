/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_input_glyphs_apply.c - registers the Kenney input glyph atlas as a pinned
 * hi-res override, resolves the Auto glyph set from the connected controller,
 * and steps the gamepad_icons setting on the Controls screen.
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

#include "hires_override.h"
#include "pc_config.h"
#include "pc_input_glyphs.h"
#include "sh_log.h"

#define ATLAS_PATH   "gamedata/icons/input_glyphs.png"
#define ATLAS_W      256
#define ATLAS_H      512
#define ATLAS_VRAM_X INPUT_GLYPHS_VRAM_X
#define ATLAS_VRAM_Y 0
#define ATLAS_VRAM_W 32
#define ATLAS_VRAM_H 256

static int s_tried;
static int s_ready;

static unsigned char* read_file(const char* path, unsigned int* outSize)
{
    FILE*          f = fopen(path, "rb");
    long           n;
    unsigned char* buf;

    if (f == NULL)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (n > 0) ? (unsigned char*)malloc((size_t)n) : NULL;
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *outSize = (unsigned int)n;
    return buf;
}

void InputGlyphs_Load(void)
{
    unsigned char* file;
    unsigned char* rgba   = NULL;
    unsigned int   size   = 0;
    int            w = 0, h = 0;
    const char*    reason = NULL;

    if (s_tried)
        return;
    s_tried = 1;

    file = read_file(ATLAS_PATH, &size);
    if (file == NULL)
    {
        reason = "atlas " ATLAS_PATH " missing";
    }
    else if (HiresOverride_DecodeToRGBA(file, size, &rgba, &w, &h) != 0 || rgba == NULL)
    {
        reason = "atlas does not decode";
    }
    else if (w != ATLAS_W || h != ATLAS_H)
    {
        reason = "atlas is not 256x512";
    }
    else
    {
        int ok;

        HiresOverride_SetForceNearestUpload(1);
        ok = HiresOverride_RegisterRGBAKeyed("input glyphs", rgba, ATLAS_W, ATLAS_H,
                                             ATLAS_VRAM_X, ATLAS_VRAM_Y, ATLAS_VRAM_W, ATLAS_VRAM_H,
                                             INPUT_GLYPHS_CLUT_X, INPUT_GLYPHS_CLUT_Y, 4, 0) == 0 &&
             HiresOverride_PinVramRect(ATLAS_VRAM_X, ATLAS_VRAM_Y, ATLAS_VRAM_W, ATLAS_VRAM_H,
                                       INPUT_GLYPHS_CLUT_X, INPUT_GLYPHS_CLUT_Y, 4, 1) == 0;
        HiresOverride_SetForceNearestUpload(0);
        if (ok)
            s_ready = 1;
        else
            reason = "atlas registration failed";
    }
    free(file);
    free(rgba);

    if (s_ready)
        SH_DBG("[GLYPHS] atlas registered");
    else
        SH_DBG("[GLYPHS] %s, drawing text", reason);
}

int InputGlyphs_Ready(void)
{
    return s_ready;
}

int InputGlyphs_ActiveSet(void)
{
    static int s_lastAuto = -1;
    int        set        = InputGlyphs_ClampSet(g_PcConfig.gamepadIcons);
    int        i, n, type = -1, vendor = 0, product = 0;

    if (set != GLYPH_SET_AUTO)
        return set;

    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++)
    {
        if (SDL_IsGameController(i))
        {
            type    = (int)SDL_GameControllerTypeForIndex(i);
            vendor  = SDL_JoystickGetDeviceVendor(i);
            product = SDL_JoystickGetDeviceProduct(i);
            break;
        }
    }
    {
        const char* deck = getenv("SteamDeck");

        set = InputGlyphs_ResolveAuto(type, vendor, product, deck != NULL && deck[0] == '1' && deck[1] == '\0');
    }
    if (set != s_lastAuto)
    {
        s_lastAuto = set;
        SH_DBG("[GLYPHS] auto: %s", InputGlyphs_SetLabel(set));
    }
    return set;
}

void InputGlyphs_Step(int dir)
{
    char value[8];

    g_PcConfig.gamepadIcons =
        ((g_PcConfig.gamepadIcons + (dir < 0 ? -1 : 1)) % GLYPH_SET_COUNT + GLYPH_SET_COUNT) %
        GLYPH_SET_COUNT;
    snprintf(value, sizeof(value), "%d", g_PcConfig.gamepadIcons);
    PcConfig_SaveKeyValue("gamepad_icons", value);
}
