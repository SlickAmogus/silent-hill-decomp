#include "game.h"

#include "main/fsqueue.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/screen/background_draw.h"
#include "bodyprog/math/math.h"

// ========================================
// GLOBAL VARIABLES
// ========================================

q0_8 g_Screen_BackgroundImgGamma = Q8(0.5f);
s16  D_800A8E5A                  = 3; // @Unused.

// ========================================
// FUNCTIONS
// ========================================

void Screen_BackgroundImgDraw(s_FsImageDesc* image) // 0x800314EC
{
    s32       baseYOffset;
    s32       tileX;
    s32       tileIdxX;
    s32       x;
    s32       y;
    u32       texShift;
    s32       texPageX;
    u32       texOffsetY;
    GsOT*     ot;
    DR_TPAGE* tPage;
    SPRT*     sprt;
    PACKET*   packet;

    ot          = (GsOT*)&g_OtTags1[g_ActiveBufferIdx + 1][0];
    packet      = GsOUT_PACKET_P;
    baseYOffset = -120 << (g_GameWork.gsScreenHeight >> 8);
    texShift    = image->tPage[0];
    texPageX    = image->tPage[1];
    texOffsetY  = image->v;

    for (y = 0; (g_GameWork.gsScreenHeight >> 8) >= y; y++)
    {
        for (x = 0; ((g_GameWork.gsScreenWidth - 1) >> (8 - texShift)) >= x; x++)
        {
            sprt = (SPRT*)packet;

            addPrimFast(ot, sprt, 4);
            setRGBC0(sprt, g_Screen_BackgroundImgGamma, g_Screen_BackgroundImgGamma, g_Screen_BackgroundImgGamma, 100);

            if (y == 0)
            {
                setWH(sprt, 256, 256 - texOffsetY);
                *((u32*)&sprt->u0) = (texOffsetY << 8) + (getClut(image->clutX, image->clutY) << 16);
            }
            else
            {
                setWH(sprt, 256, 256);
                *((u32*)&sprt->u0) = getClut(image->clutX, image->clutY) << 16;
            }

            tileX = x << 8;

#ifdef SH_PC_PORT
            setXY0Fast(sprt,
                       (tileX - (g_GameWork.gsScreenWidth >> 1)) - (image->u << (2 - texShift)) - 1,
                       baseYOffset + ((256 - texOffsetY) * y));
#else
            setXY0Fast(sprt,
                       (tileX - (g_GameWork.gsScreenWidth >> 1)) - (image->u << (2 - texShift)),
                       baseYOffset + ((256 - texOffsetY) * y));
#endif

            packet += sizeof(SPRT);
            tPage   = (DR_TPAGE*)packet;

            tileIdxX = x << texShift;
            setDrawTPage(tPage, 0, 1, getTPage(texShift, 0, (texPageX + tileIdxX) << 6, ((texPageX << 4) & 0x100) + (y << 8)));

            AddPrim(ot, tPage);
            packet = (PACKET*)tPage + sizeof(DR_TPAGE);
        }
    }

    GsOUT_PACKET_P                  = packet;
    g_SysWork.bgmStatusFlags        |= BgmStatusFlag_Pause;
    g_Screen_BackgroundImgGamma = Q8(0.5f);
}

#if 0  /* Disabled — broken on g_PsxSkipFramebufferStore=1 ortho */
void Screen_BackgroundImgDraw_PcHorStretch(s_FsImageDesc* image)
{
    GsOT*     ot       = (GsOT*)&g_OtTags1[g_ActiveBufferIdx + 1][0];
    PACKET*   packet   = GsOUT_PACKET_P;
    POLY_FT4* poly;
    s32 texShift   = image->tPage[0];
    s32 texPageX   = image->tPage[1];
    s32 texOffsetY = image->v;
    s32 numTilesX  = ((g_GameWork.gsScreenWidth - 1) >> (8 - texShift)) + 1;
    s32 yMax       = (g_GameWork.gsScreenHeight >> 8); /* same loop bound as PSX path */
    s32 y;
    s32 x;
    /* Half-width and half-height of the on-screen quad target (16:9 from
     * 240 height). Using 213/120 PSX units mirrors the Z-key map fix's
     * "fit vertically with X stretched" semantics. */
    const s32 SCREEN_HALF_W = 213;
    const s32 SCREEN_HALF_H = 120;
    s32 tileScreenWidth = (SCREEN_HALF_W * 2) / numTilesX;

    for (y = 0; y <= yMax; y++)
    {
        for (x = 0; x < numTilesX; x++)
        {
            s32 quadL = -SCREEN_HALF_W + x * tileScreenWidth;
            s32 quadR = quadL + tileScreenWidth;
            s32 quadT, quadB;
            s32 vTop, vBot;

            if (y == 0)
            {
                quadT = -SCREEN_HALF_H;
                quadB = quadT + (256 - texOffsetY) * (SCREEN_HALF_H * 2 + numTilesX) /
                                ((256 - texOffsetY) + 256 * yMax);
                /* Actually for typical paper-map atlases yMax==0 and we just
                 * fill the full vertical span. Keep the formula simple. */
                quadT = -SCREEN_HALF_H;
                quadB = SCREEN_HALF_H;
                vTop  = texOffsetY;
                vBot  = 255;
            }
            else
            {
                /* Multi-row atlases (rare for cutscene map). Fall back to
                 * proportional vertical stride matching the PSX path. */
                quadT = -SCREEN_HALF_H + (256 - texOffsetY) * y;
                quadB = quadT + 256;
                vTop  = 0;
                vBot  = 255;
            }

            poly = (POLY_FT4*)packet;
            setPolyFT4(poly);
            setRGB0Fast(poly, g_Screen_BackgroundImgGamma,
                              g_Screen_BackgroundImgGamma,
                              g_Screen_BackgroundImgGamma);

            setXY0Fast(poly, quadL, quadT);
            setXY1Fast(poly, quadR, quadT);
            setXY2Fast(poly, quadL, quadB);
            setXY3Fast(poly, quadR, quadB);

            setUV0AndClutSum(poly, 0,   vTop, getClut(image->clutX, image->clutY));
            setUV1AndTPageSum(poly, 255, vTop,
                getTPage(texShift, 0,
                         (texPageX + (x << texShift)) << 6,
                         ((texPageX << 4) & 0x100) + (y << 8)));
            setUV2Sum(poly, 0,   vBot);
            setUV3Sum(poly, 255, vBot);

            setSemiTrans(poly, 0);
            addPrim(ot, poly);
            packet = (PACKET*)(poly + 1);
        }
    }

    GsOUT_PACKET_P                  = packet;
    g_SysWork.bgmStatusFlags        |= BgmStatusFlag_Pause;
    g_Screen_BackgroundImgGamma     = Q8(0.5f);
}
#endif

void Screen_BackgroundImgTransition(s_FsImageDesc* image0, s_FsImageDesc* image1, q3_12 alpha) // 0x800317CC
{
    volatile int   pad;
    s32            i;
    s32            j;
    u32            color;
    u8             tPageY;
    s32            offsetX;
    s_FsImageDesc* image;
    POLY_FT4*      poly;

    poly = (POLY_FT4*)GsOUT_PACKET_P;

    for (i = 0; i < 3; i++)
    {
        image = (i > 0) ? image0 : image1;
        color = (i < 2) ? Q12_MULT_PRECISE(alpha, 128) : 128;

        for (j = 0; j < 3; j++)
        {
            setPolyFT4(poly);

            setXY0Fast(poly, -160 + (128 * j), -120);
            setXY1Fast(poly, (128 * j) + ((j != 2) ? -32 : -96), -120);

            offsetX = 128 * j;

            setXY2Fast(poly, -160 + offsetX, 120);
            setXY3Fast(poly, offsetX + ((j != 2) ? -32 : -96), 120);

            *((u32*)&poly->u0) = (image->v << 8) + (getClut(image->clutX, image->clutY) << 16);

            tPageY = image->tPage[1];

            *((u32*)&poly->u1) = (image->v << 8) + ((j != 2) ? 128 : 64) +
                                 (getTPage(image->tPage[0], i + 1, (image->tPage[1] + j) << 6, (tPageY << 4) & 0x100) << 16);

            *((u16*)&poly->u2) = (image->v + 239) << 8;
            *((u16*)&poly->u3) = ((image->v + 239) << 8) + ((j != 2) ? 128 : 64);

            setSemiTrans(poly, i < 2);

            *((u16*)&poly->r0) = color + (color << 8);
            poly->b0           = color;

            addPrim(g_OrderingTable0[g_ActiveBufferIdx].org, poly);
            poly++;
        }
    }

    g_SysWork.bgmStatusFlags |= BgmStatusFlag_Pause;
    GsOUT_PACKET_P = (PACKET*)poly;
}

void Screen_BackgroundImgDrawAlt(s_FsImageDesc* image) // 0x80031AAC
{
    volatile s32 pad; // TODO: Is there a better solution?
    s32          i;
    s32          xOffset;
    u8           tPageY;
    POLY_FT4*    poly;

    poly = (POLY_FT4*)GsOUT_PACKET_P;

    for (i = 0; i < 3; i++)
    {
        setPolyFT4(poly);
        setXY0Fast(poly, -160 + (128 * i), -120);
        setXY1Fast(poly, (128 * i) + ((i == 2) ? -96 : -32), -120);

        xOffset = 128 * i;

        setXY2Fast(poly, -160 + xOffset, 120);
        setXY3Fast(poly, xOffset + (i == 2 ? -96 : -32), 120);

        *((u32*)&poly->u0) = (image->v << 8) + (getClut(image->clutX, image->clutY) << 16);

        tPageY = image->tPage[1];

        *((u32*)&poly->u1) = (image->v << 8) + ((i == 2) ? 64 : 128) +
                             (getTPage(image->tPage[0], 0, (image->tPage[1] + i) << 6, (tPageY << 4) & 0x100) << 16);

        *((u16*)&poly->u2) = (image->v + 239) << 8;
        *((u16*)&poly->u3) = ((image->v + 239) << 8) + ((i == 2) ? 64 : 128);

        setSemiTrans(poly, false);

        *((u16*)&poly->r0) = g_Screen_BackgroundImgGamma + (g_Screen_BackgroundImgGamma << 8);
        poly->b0           = g_Screen_BackgroundImgGamma;

        addPrim(&g_OrderingTable0[g_ActiveBufferIdx].org[2], poly);
        poly++;
    }

    GsOUT_PACKET_P                  = (PACKET*)poly;
    g_SysWork.bgmStatusFlags        |= BgmStatusFlag_Pause;
    g_Screen_BackgroundImgGamma = Q8(0.5f);
}

bool Screen_BackgroundMotionBlur(s32 vBlanks) // 0x80031CCC
{
    s32       interlacingEnabled;
    s32       i;
    s32       offsetY;
    s32       texOffsetY;
    s32       tPageOffsetY;
    s32       j;
    GsOT*     ot;
    SPRT*     sprt;
    DR_TPAGE* tPage;

    ot                 = (GsOT*)&g_OtTags1[g_ActiveBufferIdx + 1][0];
    sprt               = (SPRT*)GsOUT_PACKET_P;
    interlacingEnabled = GsDISPENV.isinter;

    for (i = 0; (i == 0 || (interlacingEnabled && i == 1)); i++)
    {
        for (j = 0; j < 3; j++)
        {
            if (interlacingEnabled)
            {
                texOffsetY   = (-(i == 0)) & 0x20;
                offsetY      = (i == 0) ? -224 : 0;
                tPageOffsetY = i << 8;
            }
            else
            {
                offsetY      = -112;
                texOffsetY   = (g_ActiveBufferIdx == 0) << 5;
                tPageOffsetY = g_ActiveBufferIdx << 8;
            }

            addPrimFast(ot, sprt, 4);

            if ((VSync(SyncMode_Count) % vBlanks) == 0)
            {
                setRGBC0(sprt, 127, 127, 127, PRIM_RECT | RECT_TEXTURE);
            }
            else
            {
                setRGBC0(sprt, 128, 128, 128, PRIM_RECT | RECT_TEXTURE);
            }

            setWH(sprt, 256, 224);
            *((u32*)&sprt->u0) = texOffsetY << 8;

            setXY0Fast(sprt, (-g_GameWork.gsScreenWidth / 2) + (j << 8), offsetY);

            sprt++;
            tPage = (DR_TPAGE*)sprt;

            setDrawTPage(tPage, 0, 1, getTPage(2, 0, (j << 8), tPageOffsetY));
            AddPrim(ot, tPage);

            sprt = (PACKET*)sprt + sizeof(DR_TPAGE);
        }
    }

    GsOUT_PACKET_P = sprt;
    return false;
}
