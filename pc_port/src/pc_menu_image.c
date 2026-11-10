/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Menu images; see pc_menu_image.h. */
#include "game.h"

#include <stdio.h>
#include <stdlib.h>

#include "bodyprog/bodyprog.h"

#include "hires_override.h"
#include "pc_menu_image.h"
#include "sh_log.h"
#include "stb_image.h"

/* Slots past the play-as body in the range that survives map loads; one row
 * each, so their spill slots (+64, +128) stay unused. */
#define MI_SLOT_BASE (HIRES_POOL_PLAYAS_SLOT + 1)

typedef struct
{
    const char* path;
    int         nativeW, nativeH; /* the UV space: 8-bit, so under 256 */
    int         state;            /* 0 untried, 1 ready, -1 missing */
} MiImage;

static MiImage s_images[PC_MENU_IMAGE_COUNT] = {
    { "gamedata/image/buttonWide.png", 240, 84, 0 },
    { "gamedata/image/warning.png", 110, 116, 0 },
};

static int mi_ready(int id)
{
    MiImage*       im = &s_images[id];
    unsigned char* rgba;
    int            w = 0, h = 0, comp = 0;

    if (im->state != 0)
        return im->state > 0;
    im->state = -1;
    {
        FILE*          f = fopen(im->path, "rb");
        long           size;
        unsigned char* buf;

        if (f == NULL)
        {
            SH_DBG("[MENUIMG] %s not found", im->path);
            return 0;
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = (size > 0) ? (unsigned char*)malloc((size_t)size) : NULL;
        if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size)
        {
            fclose(f);
            free(buf);
            return 0;
        }
        fclose(f);
        rgba = stbi_load_from_memory(buf, (int)size, &w, &h, &comp, 4);
        free(buf);
    }
    if (rgba == NULL)
    {
        SH_DBG("[MENUIMG] %s: %s", im->path, stbi_failure_reason());
        return 0;
    }
    if (HiresOverride_PoolSlotRegisterRGBA(MI_SLOT_BASE + id, 0, rgba, w, h, im->nativeW, im->nativeH) == 0)
        im->state = 1;
    stbi_image_free(rgba);
    return im->state > 0;
}

int Pc_MenuImage_Draw(int id, int x0, int y0, int x1, int y1, int bucket, int shade)
{
    POLY_FT4* poly = (POLY_FT4*)GsOUT_PACKET_P;
    int       slot = MI_SLOT_BASE + id;
    int       u, v;

    if (id < 0 || id >= PC_MENU_IMAGE_COUNT || !mi_ready(id))
        return 0;
    u = s_images[id].nativeW - 1;
    v = s_images[id].nativeH - 1;
    setPolyFT4(poly);
    setRGB0(poly, (u8)shade, (u8)shade, (u8)shade);
    /* The bit-15 clut alone keys the slot's texture; no VRAM page is read. */
    poly->tpage = 0;
    poly->clut  = (u16)(((HIRES_POOL_CLUT_ROW_BASE + (slot / 64) * HIRES_POOL_MAX_ROWS) << 6) | (slot % 64));
    setUV4(poly, 0, 0, u, 0, 0, v, u, v);
    setXY4(poly, x0, y0, x1, y0, x0, y1, x1, y1);
    addPrim(&g_OrderingTable2[g_ActiveBufferIdx].org[bucket], poly);
    GsOUT_PACKET_P = (PACKET*)(poly + 1);
    return 1;
}
