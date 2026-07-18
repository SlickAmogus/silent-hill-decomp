#include "game.h"

#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_data.h"
#include "bodyprog/screen/screen_draw.h"
#include "bodyprog/text/text_draw.h"
#include "bodyprog/math/math.h"

#ifdef SH_PC_PORT
#include "font_region.h"   /* Region font layout: US strip vs PAL 21x6 grid. */
#include "lang_text.h"     /* Pc_LangMenuText — port-written menu translations. */
#include "pc_kanji.h"      /* NTSC-J SJIS glyph atlas. */
#include "main/fileinfo.h" /* g_GameRegion */
#ifdef SH_PC_PORT
#include "pc_config.h"     /* g_PcConfig — texture-mod gate for HD-atlas glyph clipping */
#endif
#endif

#ifndef PAD_HACK_IGNORE
    const s32 pad_rodata_80025D68 = 0;
    s8 __pad_bss_800C38B2[2];
    s32 __pad_bss_800C38B8[4];
    s16 __pad_bss_800C391E;
    s32 __pad_bss_800C3924;
#endif

// ========================================
// STATIC VARIABLES
// ========================================

/** @brief Glyph widths for the 12x16 font. Used for kerning. */
static const u8 FONT_12X16_GLYPH_WIDTHS[FONT_12X16_GLYPH_COUNT] = {
    3,  7,  7,  11, 11, 4,  10, 4,  6,  10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 4,  4,
    10, 11, 10, 8,  13, 12, 12, 12, 13, 11, 11, 13, 12, 9,  9,  12, 12, 13, 12, 13, 11,
    13, 12, 10, 11, 13, 12, 12, 12, 11, 12, 6,  4,  6,  8,  0,  3,  9,  10,  9, 9,  9,
    7,  11, 11, 6,  6,  10, 6,  13, 11, 10, 11, 10, 8,  8,  7,  10, 10, 12, 10, 10, 9
};

#ifdef SH_PC_PORT
    /* Mutable on PC: retail EUR dims LightGrey — patched at region init. */
    #define STRING_COLORS_CONST
#else
    #define STRING_COLORS_CONST const
#endif

/** @brief See `e_StringColorId`. */
static STRING_COLORS_CONST u32 STRING_COLORS[StringColorId_Count] = {
    COLOR_RGBC(160, 128, 64,  PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(32,  32,  32,  PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(24,  128, 40,  PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(8,   184, 96,  PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(128, 0,   0,   PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(24,  128, 40,  PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(100, 100, 100, PRIM_RECT | RECT_TEXTURE),
    COLOR_RGBC(128, 128, 128, PRIM_RECT | RECT_TEXTURE)
};

/** `e_ColorId` */
static s16 g_StringColorId = StringColorId_White;

// 2 bytes of padding.

/** Text index 2D layer.
 * If modifying `Gfx_StringSetPosition`, when setting it to
 * a value lower than 6, text will not be affected by the fade effect.
 */
static s32 g_Strings2dLayerIdx = 6;


// ========================================
// GLOBAL VARIABLES
// ========================================

DVECTOR    g_StringPosition;
s32        g_StringPositionX1;
s_800C38B0 D_800C38B0;
s32        g_MapMsg_WidthIdx;
s32        g_MapMsg_Widths[12];
GsSPRITE   g_MapMsg_GlyphSprite;
s16        D_800C391C;
s32        D_800C3920;

#ifdef SH_PC_PORT
/* Region hook (font_region.c): retail EUR ships STRING_COLORS[LightGrey] as
 * (64,64,64) instead of the US (100,100,100). */
void Gfx_StringLightGreyColorPatch(unsigned char r, unsigned char g, unsigned char b)
{
    STRING_COLORS[StringColorId_LightGrey] = COLOR_RGBC(r, g, b, PRIM_RECT | RECT_TEXTURE);
}
#endif

void Gfx_StringSetPosition(s32 x, s32 y) // 0x8004A87C
{
    #define OFFSET_X SCREEN_POSITION_X(50.0f)
    #define OFFSET_Y SCREEN_POSITION_Y(47.0f)

    if (x != NO_VALUE)
    {
        g_StringPosition.vx = x - OFFSET_X;
        g_StringPositionX1  = (s16)(x - OFFSET_X);
    }

    if (y != NO_VALUE)
    {
        g_StringPosition.vy = y - OFFSET_Y;
    }

    g_Strings2dLayerIdx = 6;

    #undef OFFSET_X
    #undef OFFSET_Y
}

void Gfx_Strings2dLayerIdxSet(s32 idx) // 0x8004A8C0
{
    g_Strings2dLayerIdx = idx;
}

void Gfx_StringsReset2dLayerIdx(void) // 0x8004A8CC
{
    g_Strings2dLayerIdx = 6;
}

void Gfx_StringSetColor(s16 colorId) // 0x8004A8DC
{
    g_StringColorId = colorId;
}

bool Gfx_StringDraw(char* str, s32 strLength) // 0x8004A8E8
{
    #define WIDE_SPACE_SIZE 10
    #define ATLAS_BASE_Y    240

    // TODO: This only works for one case. There may originally have been some other generic macro.
    #define setSprtUvClut(glyphSprt, idx, clut)                                                                                                     \
    *((u32*)&(glyphSprt)->u0) = (((idx) % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X) + /* `u0`:   Column in atlas. */            \
                                (ATLAS_BASE_Y << 8)                                                 + /* `v0`:   Row 0 in atlas with offset. */ \
                                ((clut) << 16)                                                        /* `clut`: Packed magic value. */

    s32       posX;
    s32       posY;
    s32       u0;
    s32       sizeCpy;
    s32       glyphIdx;
    u32       glyphColor;
    u32       charCode;
    u8*       strCpy;
    bool      result;
    s32       glyphWidth;
    s32       posXCpy;
    GsOT*     ot;
    PACKET*   packet;
    DR_TPAGE* tPage;
    POLY_FT4* glyphPoly;
    SPRT*     glyphSprt;

    // Create local argument copies.
#ifdef SH_PC_PORT
    /* Menu-translation chokepoint: every menu/UI string funnels through this
     * function, so a single exact-match lookup localizes them all (misses —
     * item text, digits, US discs — return the input untouched). */
    strCpy  = (u8*)Pc_LangMenuText(str);
#else
    strCpy  = str;
#endif
    sizeCpy = strLength;

    packet = NULL;
    result = false;

    // Set base screen position.
    posX = g_StringPosition.vx;
    posY = g_StringPosition.vy;

    glyphColor = STRING_COLORS[g_StringColorId];
    ot         = &g_OtTags0[g_ActiveBufferIdx][g_Strings2dLayerIdx];

    if (!g_SysWork.enableHighResGlyphs)
    {
        packet = GsOUT_PACKET_P;
    }

    // Parse string.
    while (sizeCpy > 0)
    {
        charCode = *strCpy;

        // TODO: Try refactoring into switch.

        // Convert literal `!` and `&` into `char`s mappable to representative atlas glyphs.
        if (charCode == '!')
        {
            charCode = '\\';
        }
        else if (charCode == '&')
        {
            charCode = '^';
        }

        // Space.
        if (charCode == '_')
        {
            posX += FONT_12X16_SPACE_SIZE;
        }
        // Wide space.
        else if (charCode == '\v')
        {
            posX += WIDE_SPACE_SIZE;
        }
        // Start of header.
        else if (charCode == '\x01')
        {
            posX--;
        }
        // Regular character.
#ifdef SH_PC_PORT
        /* NTSC-J: SJIS pairs (the select-prompt options like Yes/No route the
         * JP map messages through this drawer) — one 12x16 kanji atlas cell. */
        else if (g_GameRegion == Region_JPN && Pc_KanjiIsLead(charCode) && strCpy[1] != '\0')
        {
            unsigned int   kPage;
            int            kU, kV;
            unsigned short kClut;

            sizeCpy--;

            if (Pc_KanjiCell((u16)(((u32)charCode << 8) | strCpy[1]), &kPage, &kU, &kV, &kClut))
            {
                if (g_SysWork.enableHighResGlyphs)
                {
                    glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                    setPolyFT4(glyphPoly);
                    setRGB0(glyphPoly, glyphColor, glyphColor >> 8, glyphColor >> 16);
                    setXY4(glyphPoly,
                           posX,                           posY * 2,
                           posX,                           (posY * 2) + g_FontLayout->hiResGlyphBottom,
                           posX + FONT_12X16_GLYPH_SIZE_X, posY * 2,
                           posX + FONT_12X16_GLYPH_SIZE_X, (posY * 2) + g_FontLayout->hiResGlyphBottom);

                    *((u32*)&glyphPoly->u0) = (u32)kU + ((u32)kV << 8) + ((u32)kClut << 16);
                    *((u32*)&glyphPoly->u1) = (u32)kU + (kPage << 16) + (((u32)kV + 15) << 8);
                    *((u16*)&glyphPoly->u2) = (u16)(kU + FONT_12X16_GLYPH_SIZE_X + (kV << 8));
                    *((u16*)&glyphPoly->u3) = (u16)(kU + FONT_12X16_GLYPH_SIZE_X + ((kV + 15) << 8));

                    addPrim(ot, glyphPoly);
                    GsOUT_PACKET_P = (u8*)glyphPoly + sizeof(POLY_FT4);
                }
                else
                {
                    posXCpy = (u16)posX;

                    glyphSprt              = (SPRT*)packet;
                    *((u32*)&glyphSprt->w) = 0x10000C;

                    addPrimFast(ot, glyphSprt, 4);
                    *((u32*)&glyphSprt->r0)   = glyphColor;
                    *((u32*)(&glyphSprt->x0)) = posXCpy + (posY << 16);
                    *((u32*)&glyphSprt->u0)   = (u32)kU + ((u32)kV << 8) + ((u32)kClut << 16);

                    packet += sizeof(SPRT);

                    tPage = (DR_TPAGE*)packet;
                    setDrawTPage(tPage, 0, 1, kPage);
                    addPrim(ot, tPage);

                    packet += sizeof(DR_TPAGE);
                }
            }

            posX += FONT_12X16_GLYPH_SIZE_X;
            strCpy++; /* trail byte; the loop tail consumes the lead */
        }
        /* Region font layout: same math as the original, with the layout
         * constants (v base, tpage, CLUT, widths) coming from g_FontLayout.
         * With the USA layout the emitted prim words are bit-identical to the
         * hardcoded originals kept in the #else. Bytes >= 0x80 are the PAL
         * accents (up to 2 emissions: combining mark + base letter). */
        else if ((charCode >= GLYPH_TABLE_ASCII_OFFSET && charCode <= 'z') || charCode >= 0x80)
        {
            s_GlyphEmit emits[2];
            s32         emitCount;
            s32         emitIdx;

            sizeCpy--;
            emitCount = Font_MapChar(charCode, emits);

            for (emitIdx = 0; emitIdx < emitCount; emitIdx++)
            {
                s32 row;
                s32 vTop;
                u32 page;
                s32 drawY;

                glyphIdx   = emits[emitIdx].cell;
                glyphWidth = emits[emitIdx].advance;
                drawY      = posY + emits[emitIdx].dy;
                row        = glyphIdx / FONT_12X16_ATLAS_COLUMN_COUNT;
                vTop       = g_FontLayout->vBase + ((row % g_FontLayout->rowsPerPage) * FONT_12X16_GLYPH_SIZE_Y);
                page       = g_FontLayout->tpageBase + (row / g_FontLayout->rowsPerPage);
                u0         = (glyphIdx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X;

                // Draw glyph sprite.
                if (g_SysWork.enableHighResGlyphs)
                {
                    s32 glyphDrawW = FONT_12X16_GLYPH_SIZE_X;
                    if ((g_PcConfig.texturePacks || g_PcConfig.allowLooseFiles) &&
                        glyphWidth > 0 && glyphWidth < (s32)FONT_12X16_GLYPH_SIZE_X)
                        glyphDrawW = glyphWidth; /* clip HD-atlas overlap ghost (see SPRT path) */

                    glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                    setPolyFT4(glyphPoly);
                    setRGB0(glyphPoly, glyphColor, glyphColor >> 8, glyphColor >> 16);
                    setXY4(glyphPoly,
                           posX,              drawY * 2,
                           posX,              (drawY * 2) + g_FontLayout->hiResGlyphBottom,
                           posX + glyphDrawW, drawY * 2,
                           posX + glyphDrawW, (drawY * 2) + g_FontLayout->hiResGlyphBottom);

                    *((u32*)&glyphPoly->u0) = u0 + (vTop << 8) + (g_FontLayout->packedClut << 16); // `u0`, `v0`, `clut`.
                    *((u32*)&glyphPoly->u1) = u0 + (page << 16) + ((vTop + 15) << 8);              // `u1`, `v1`, `page`.
                    *((u16*)&glyphPoly->u2) = (u16)(u0 + glyphDrawW + (vTop << 8));   // `u2`, `v2`.
                    *((u16*)&glyphPoly->u3) = (u16)(u0 + glyphDrawW + ((vTop + 15) << 8)); // `u3`, `v3`.

                    addPrim(ot, glyphPoly);
                    GsOUT_PACKET_P = (u8*)glyphPoly + sizeof(POLY_FT4);
                }
                else
                {
                    u32 spriteW = FONT_12X16_GLYPH_SIZE_X;
                    posXCpy = (u16)posX;

                    /* Clip to the glyph's advance when a texture mod is active: HD font
                     * atlases paint the gutterless 4bpp cells edge-to-edge, so a narrow
                     * glyph's fixed-12 sample overlaps the next glyph with the mod's edge
                     * pixels ("ghost text"). Original art has transparent padding past the
                     * advance, so this is visually identical there. */
                    if ((g_PcConfig.texturePacks || g_PcConfig.allowLooseFiles) &&
                        glyphWidth > 0 && glyphWidth < (s32)FONT_12X16_GLYPH_SIZE_X)
                        spriteW = (u32)glyphWidth;

                    glyphSprt              = (SPRT*)packet;
                    *((u32*)&glyphSprt->w) = ((u32)FONT_12X16_GLYPH_SIZE_Y << 16) | spriteW;

                    addPrimFast(ot, glyphSprt, 4);
                    *((u32*)&glyphSprt->r0)   = glyphColor;
                    *((u32*)(&glyphSprt->x0)) = posXCpy + (drawY << 16);
                    *((u32*)&glyphSprt->u0)   = u0 + (vTop << 8) + (g_FontLayout->packedClut << 16); // `u0`, `v0`, `clut`.

                    packet += sizeof(SPRT);

                    tPage = (DR_TPAGE*)packet;
                    setDrawTPage(tPage, 0, 1, page);
                    addPrim(ot, tPage);

                    packet += sizeof(DR_TPAGE);
                }

                posX += glyphWidth;
            }
        }
#else
        else if (charCode >= GLYPH_TABLE_ASCII_OFFSET && charCode <= 'z')
        {
            sizeCpy--;

            // Draw glyph sprite.
            if (g_SysWork.enableHighResGlyphs)
            {
                glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                glyphIdx   = charCode - GLYPH_TABLE_ASCII_OFFSET;
                glyphWidth = FONT_12X16_GLYPH_WIDTHS[glyphIdx];

                setPolyFT4(glyphPoly);
                setRGB0(glyphPoly, glyphColor, glyphColor >> 8, glyphColor >> 16);
                setXY4(glyphPoly,
                       posX,                             posY * 2,
                       posX,                             (posY * 2) + 30,
                       posX + FONT_12X16_GLYPH_SIZE_X, posY * 2,
                       posX + FONT_12X16_GLYPH_SIZE_X, (posY * 2) + 30);

                posX += glyphWidth;

                u0 = (glyphIdx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X;

                *((u32*)&glyphPoly->u0) = u0 + (0xF000 + (0x7FD3 << 16));                                                    // `u0`, `v0`, `clut`.
                *((u32*)&glyphPoly->u1) = u0 + (((((glyphIdx / FONT_12X16_ATLAS_COLUMN_COUNT) & 0xF) | 16) << 16) | 0xFF00); // `u1`, `v1`, `page`.
                *((u16*)&glyphPoly->u2) = u0 - 0xFF4;                                                                        // `u2`, `v2`.
                *((u16*)&glyphPoly->u3) = u0 - 0xF4;                                                                         // `u3`, `v3`.

                addPrim(ot, glyphPoly);
                GsOUT_PACKET_P = (u8*)glyphPoly + sizeof(POLY_FT4);
            }
            else
            {
                posXCpy = (u16)posX;

                glyphSprt              = (SPRT*)packet;
                *((u32*)&glyphSprt->w) = 0x10000C;

                glyphIdx = charCode - GLYPH_TABLE_ASCII_OFFSET;
                posX    += FONT_12X16_GLYPH_WIDTHS[glyphIdx];

                addPrimFast(ot, glyphSprt, 4);
                *((u32*)&glyphSprt->r0)   = glyphColor;
                *((u32*)(&glyphSprt->x0)) = posXCpy + (posY << 16);

                setSprtUvClut(glyphSprt, glyphIdx, 0x7FD3); // TODO: Demagic CLUT arg.
                //*((u32*)&glyphSprt->u0) = ((glyphIdx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X) + 0xF000 + (0x7FD3 << 16); // `u0`, `v0`, `clut`.

                packet += sizeof(SPRT);

                tPage = (DR_TPAGE*)packet;
                setDrawTPage(tPage, 0, 1, ((glyphIdx / FONT_12X16_ATLAS_COLUMN_COUNT) & 0xF) | 16);
                addPrim(ot, tPage);

                packet += sizeof(DR_TPAGE);
            }
        }
#endif
        // Newline.
        else if (charCode == '\n')
        {
            posX  = g_StringPositionX1;
            posY += FONT_12X16_GLYPH_SIZE_Y;
        }
        // New color.
        else if (charCode >= '\x01' && charCode < '\b')
        {
            glyphColor      = STRING_COLORS[charCode];
            g_StringColorId = charCode;
        }
        // Terminator.
        else if (charCode == '\0')
        {
            result = true;
            break;
        }

        strCpy++;
    }

    if (!g_SysWork.enableHighResGlyphs)
    {
        GsOUT_PACKET_P = packet;
    }

    // Reset base string position?
    Math_DVectorSetFast(&g_StringPosition, posX, posY);

    return result;

    #undef WIDE_SPACE_SIZE
    #undef ATLAS_BASE_Y
}

s32 Gfx_MapMsg_CalculateWidths(s32 mapMsgIdx) // 0x8004ACF4
{
    s32 i;
    s32 j;
    s32 charCode;
    u8  msgCode;
    s32 posIdx;
    u8* mapMsg;

    g_MapMsg_WidthIdx  = 1;
    g_MapMsg_AudioLoadBlock = 0;

    for (i = (FONT_12X16_LINE_COUNT_MAX - 1); i >= 0; i--)
    {
        g_MapMsg_Widths[i] = 0;
    }

    mapMsg = g_MapOverlayHdr.mapMessages[mapMsgIdx];

    for (j = 0; j < FONT_12X16_LINE_COUNT_MAX; )
    {
        charCode = *mapMsg;

        switch (charCode)
        {
            case '\t':
            case '\n':
            case ' ':
                mapMsg++;
                break;

            case '_':
                ++mapMsg;
                g_MapMsg_Widths[g_MapMsg_WidthIdx - 1] += FONT_12X16_SPACE_SIZE;
                break;

            case MAP_MSG_CODE_MARKER:
                msgCode = *++mapMsg;
                posIdx  = *++mapMsg - '0';

                switch (msgCode)
                {
                    case MAP_MSG_CODE_COLOR:
                    case MAP_MSG_CODE_SELECT:
                    case MAP_MSG_CODE_TAB:
                        break;

                    case MAP_MSG_CODE_NEWLINE:
                        j++;
                        g_MapMsg_WidthIdx++;
                        break;

                    case MAP_MSG_CODE_END:
                        j = FONT_12X16_LINE_COUNT_MAX;
                        break;

                    case MAP_MSG_CODE_LINE_POSITION:
                        D_800C38B0.positionIdx = posIdx;
                        break;

                    case MAP_MSG_CODE_JUMP:
                        if (posIdx == 2)
                        {
                            g_MapMsg_AudioLoadBlock = 3;
                        }

                        while (posIdx != ' ' && posIdx != '\t')
                        {
                            posIdx = *++mapMsg;
                        }

                        break;

                    case MAP_MSG_CODE_HIGH_RES:
                        g_SysWork.enableHighResGlyphs = true;
                        break;
                }

                mapMsg++;
                break;

            case 0:
                j = FONT_12X16_LINE_COUNT_MAX;
                break;

            default:
                // Convert literal `!` and `&` into `char`s mappable to representative atlas glyphs.
                if (charCode == '!')
                {
                    charCode = '\\';
                }
                else if (charCode == '&')
                {
                    charCode = '^';
                }

#ifdef SH_PC_PORT
                /* NTSC-J: an SJIS pair is one fixed-width kanji cell. */
                if (g_GameRegion == Region_JPN && Pc_KanjiIsLead((u8)charCode) && mapMsg[1] != '\0')
                {
                    g_MapMsg_Widths[g_MapMsg_WidthIdx - 1] += FONT_12X16_GLYPH_SIZE_X;
                    mapMsg += 2;
                    break;
                }

                /* Region font layout: identical widths for US bytes; PAL
                 * accent bytes sum their emissions (mark advance is 0). */
                {
                    s_GlyphEmit emits[2];
                    s32         emitCount;
                    s32         emitIdx;

                    emitCount = Font_MapChar((u8)charCode, emits);

                    for (emitIdx = 0; emitIdx < emitCount; emitIdx++)
                    {
                        g_MapMsg_Widths[g_MapMsg_WidthIdx - 1] += emits[emitIdx].advance;
                    }
                }
#else
                g_MapMsg_Widths[g_MapMsg_WidthIdx - 1] += FONT_12X16_GLYPH_WIDTHS[charCode - GLYPH_TABLE_ASCII_OFFSET];
#endif
                mapMsg++;
                break;
        }
    }

    // TODO: JAP0 includes extra code and returns a value here.
}

s32 Gfx_MapMsg_StringDraw(char* mapMsg, s32 strLength) // 0x8004AF18
{
    #define LINE_SPACE_SIZE 32
    #define CHARCODE_OFFSET '\''

    s32       glyphPosX;
    s32       glyphPosY;
    u32       temp_a0;
    s32       temp_a0_2;
    s32       digit;
    s32       i;
    s32       fractionDigits;
    s32       longestLineWidth;
    bool      isFraction;
    s32       lineIdx;
    u32       color;
    u8        codeTag;
    s32       codeArg;
    s32       result;
    s32       charCode;
    s32       idx;
    s32       charWidth;
    GsOT*     ot;
    PACKET*   packet;
    DR_TPAGE* tPage;
    POLY_FT4* glyphPoly;
    SPRT*     glyphSprt;

    packet = NULL;
    result = 0;

    ot                  = (GsOT*)&g_OtTags0[g_ActiveBufferIdx][6];
    color               = STRING_COLORS[g_StringColorId];
    g_StringPosition.vx = -(g_MapMsg_Widths[0] >> 1);

    if (!g_SysWork.enableHighResGlyphs)
    {
        packet = GsOUT_PACKET_P;
    }

    switch ((u8)D_800C38B0.positionIdx)
    {
        case 0:
            g_StringPosition.vy = -92;
            break;

        case 1:
            g_StringPosition.vy = 76 - ((g_MapMsg_WidthIdx - 1) * FONT_12X16_GLYPH_SIZE_Y);
            break;

        case 2:
            g_StringPosition.vy = -60;
            break;

        case 3:
            g_StringPosition.vy = 44 - ((g_MapMsg_WidthIdx - 1) * FONT_12X16_GLYPH_SIZE_Y);
            break;

        case 4:
            g_StringPosition.vy = ((FONT_12X16_LINE_COUNT_MAX - g_MapMsg_WidthIdx) * 8) - 76;
            break;
    }

#ifdef SH_PC_PORT
    /* The 3D-world vertical-FOV crop (g_PsxWorldVScale, top-anchored) clips the bottom of
     * the frame, where the bottom-anchored message boxes (positionIdx 1/3/4) sit — cutting
     * the lower lines off-screen on multi-line messages. Lift those boxes up by
     * g_PsxMsgVShift (console MSGSHIFT) to compensate. Top boxes (0/2) are above the clip. */
    {
        u8 pidx = (u8)D_800C38B0.positionIdx;
        extern int g_PsxCutsceneActive;
        /* Only during gameplay — cutscenes skip the crop (g_PsxCutsceneActive), so their
         * message boxes are already at their true positions and must NOT be shifted. */
        if ((pidx == 1 || pidx == 3 || pidx == 4) && !g_PsxCutsceneActive) {
            extern int g_PsxMsgVShift;
            g_StringPosition.vy -= (s16)g_PsxMsgVShift;
        }
    }
#endif

    longestLineWidth = g_MapMsg_Widths[0];
    for (i = 0; i < g_MapMsg_WidthIdx; i++)
    {
        if (longestLineWidth < g_MapMsg_Widths[i])
        {
            longestLineWidth = g_MapMsg_Widths[i];
        }
    }

    g_StringPosition.vx = -(longestLineWidth >> 1);
    g_StringPositionX1  = g_StringPosition.vx;
    glyphPosX           = g_StringPositionX1;
    glyphPosY           = g_StringPosition.vy;

    // Parse string.
    for (lineIdx = 0; lineIdx < FONT_12X16_LINE_COUNT_MAX;)
    {
        // Convert literal `!` and `&` into `char`s mappable to representative atlas glyphs.
#ifdef SH_PC_PORT
        /* Read unsigned: PAL accent bytes (0x80+) must not sign-extend into
         * negative glyph indices (mapMsg is char*, signed on this target). */
        charCode = *(u8*)mapMsg;
#else
        charCode = *mapMsg;
#endif
        if (charCode == '!')
        {
            charCode = '\\';
        }
        else if (charCode == '&')
        {
            charCode = '^';
        }

        // Process `char`.
        switch (charCode)
        {
            // Space.
            case '_':
                glyphPosX += FONT_12X16_SPACE_SIZE;
                mapMsg++;
                break;

            // Ignore spaces and tabs.
            case ' ':
            case '\t':
                mapMsg++;
                break;

            case MAP_MSG_CODE_MARKER:
                codeTag = *++mapMsg;
                codeArg = *++mapMsg - '0';

                switch (codeTag)
                {
                    default:
                        break;

                    case MAP_MSG_CODE_NEWLINE:
                        lineIdx++;

                        switch (result)
                        {
                            case MapMsgCode_AlignCenter:
                                glyphPosX = -(g_MapMsg_Widths[lineIdx] >> 1);
                                break;

                            case MapMsgCode_SetByT:
                                glyphPosX = g_StringPositionX1;
                                break;

                            default:
                                glyphPosX = -(longestLineWidth >> 1);
                                break;
                        }

                        glyphPosY += FONT_12X16_GLYPH_SIZE_Y;
                        break;

                    case MAP_MSG_CODE_JUMP:
                        fractionDigits = 0;
                        isFraction     = 0;
                        digit          = 0;

                        // Parse time value.
                        if (g_SysWork.mapMsgTimer == NO_VALUE)
                        {
                            s32 c;

                            mapMsg                  = mapMsg + 2;
                            c                       = *mapMsg;
                            g_MapMsg_AudioLoadBlock = codeArg + 1;

                            while (c != ')')
                            {
                                if (c == '.')
                                {
                                    isFraction = 1;
                                }
                                else
                                {
                                    if (isFraction != 0)
                                    {
                                        fractionDigits++;
                                    }

                                    digit *= 10;
                                    digit -= '0' - c;
                                }

                                mapMsg++;
                                c = *mapMsg;
                            }

                            digit <<= 12;
                            for (i = 0; i < fractionDigits; i++)
                            {
                                digit /= 10;
                            }

                            g_SysWork.mapMsgTimer = digit;
                            mapMsg                = mapMsg + 1;
                        }
                        else
                        {
                            while (codeArg != ' ' && codeArg != '\t')
                            {
                                codeArg = *++mapMsg;
                            }
                        }
                        break;

                    case MAP_MSG_CODE_MIDDLE:
                        result    = MapMsgCode_AlignCenter;
                        glyphPosX = -(g_MapMsg_Widths[lineIdx] >> 1);
                        break;

                    case MAP_MSG_CODE_TAB:
                        result             = MapMsgCode_SetByT;
                        g_StringPositionX1 = -120;
                        glyphPosX          = -120;
                        break;

                    case MAP_MSG_CODE_COLOR:
                        color           = STRING_COLORS[codeArg];
                        g_StringColorId = codeArg;
                        break;

                    case MAP_MSG_CODE_DISPLAY_ALL:
                        strLength = MAP_MESSAGE_DISPLAY_ALL_LENGTH;
                        break;

                    case MAP_MSG_CODE_END:
                        result  = NO_VALUE;
                        lineIdx = FONT_12X16_LINE_COUNT_MAX;
                        break;

                    case MAP_MSG_CODE_SELECT:
                        result  = codeArg;
                        lineIdx = FONT_12X16_LINE_COUNT_MAX;
                        break;

                    case MAP_MSG_CODE_HIGH_RES:
                        g_SysWork.enableHighResGlyphs = true;
                        break;
            }

            mapMsg++;
            break;

        // Terminator.
        case '\0':
            result  = 1;
            lineIdx = FONT_12X16_LINE_COUNT_MAX;
            break;

        // Draw glyph sprite.
        default:
            strLength--;

#ifdef SH_PC_PORT
            /* NTSC-J: an SJIS pair draws one 12x16 kanji cell from the PC
             * atlas — same SPRT/FT4 shape as the Latin glyphs, different
             * uv/page/clut. Absent glyphs advance silently. */
            if (g_GameRegion == Region_JPN && Pc_KanjiIsLead((u8)charCode) && ((u8*)mapMsg)[1] != '\0')
            {
                unsigned int   kPage;
                int            kU, kV;
                unsigned short kClut;

                if (Pc_KanjiCell((u16)(((u32)(u8)charCode << 8) | ((u8*)mapMsg)[1]),
                                 &kPage, &kU, &kV, &kClut))
                {
                    if (g_SysWork.enableHighResGlyphs)
                    {
                        glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                        setPolyFT4(glyphPoly);
                        setRGB0(glyphPoly, (s8)color, (s8)(color >> 8), (s8)(color >> 16));
                        setXY4(glyphPoly,
                               glyphPosX,                           glyphPosY * 2,
                               glyphPosX,                           (glyphPosY * 2) + g_FontLayout->hiResGlyphBottom,
                               glyphPosX + FONT_12X16_GLYPH_SIZE_X, glyphPosY * 2,
                               glyphPosX + FONT_12X16_GLYPH_SIZE_X, (glyphPosY * 2) + g_FontLayout->hiResGlyphBottom);

                        *((u32*)&glyphPoly->u0) = (u32)kU + ((u32)kV << 8) + ((u32)kClut << 16);
                        *((u32*)&glyphPoly->u1) = (u32)kU + (kPage << 16) + (((u32)kV + 15) << 8);
                        *((u16*)&glyphPoly->u2) = (u16)(kU + FONT_12X16_GLYPH_SIZE_X + (kV << 8));
                        *((u16*)&glyphPoly->u3) = (u16)(kU + FONT_12X16_GLYPH_SIZE_X + ((kV + 15) << 8));

                        addPrim(ot, glyphPoly);
                        GsOUT_PACKET_P = (PACKET*)glyphPoly + sizeof(POLY_FT4);
                    }
                    else
                    {
                        glyphSprt              = (SPRT*)packet;
                        *((u32*)&glyphSprt->w) = 0x10000C;

                        addPrimFast(ot, glyphSprt, 4);
                        *((u32*)&glyphSprt->r0)   = color;
                        *((u32*)(&glyphSprt->x0)) = (u16)glyphPosX + (glyphPosY << 16);
                        *((u32*)&glyphSprt->u0)   = (u32)kU + ((u32)kV << 8) + ((u32)kClut << 16);

                        packet += sizeof(SPRT);

                        tPage = (DR_TPAGE*)packet;
                        setDrawTPage(tPage, 0, 1, kPage);
                        addPrim(ot, tPage);

                        packet += sizeof(DR_TPAGE);
                    }
                }

                glyphPosX += FONT_12X16_GLYPH_SIZE_X;
                mapMsg    += 2;

                if (strLength <= 0)
                {
                    if (!g_SysWork.enableHighResGlyphs)
                    {
                        GsOUT_PACKET_P = packet;
                    }

                    return result;
                }
                break;
            }

            /* Region font layout (see Gfx_StringDraw): USA output is
             * bit-identical to the original constants kept in the #else;
             * PAL accent bytes emit up to 2 glyphs (combining mark + base). */
            {
                s_GlyphEmit emits[2];
                s32         emitCount;
                s32         emitIdx;

                emitCount = Font_MapChar(charCode, emits);

                for (emitIdx = 0; emitIdx < emitCount; emitIdx++)
                {
                    s32 row;
                    s32 vTop;
                    u32 page;
                    s32 drawY;

                    idx       = emits[emitIdx].cell;
                    charWidth = emits[emitIdx].advance;
                    drawY     = glyphPosY + emits[emitIdx].dy;
                    row       = idx / FONT_12X16_ATLAS_COLUMN_COUNT;
                    vTop      = g_FontLayout->vBase + ((row % g_FontLayout->rowsPerPage) * FONT_12X16_GLYPH_SIZE_Y);
                    page      = g_FontLayout->tpageBase + (row / g_FontLayout->rowsPerPage);
                    temp_a0   = (idx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X;

                    if (g_SysWork.enableHighResGlyphs)
                    {
                        glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                        setPolyFT4(glyphPoly);
                        setRGB0(glyphPoly, (s8)color, (s8)(color >> 8), (s8)(color >> 16));
                        setXY4(glyphPoly,
                               glyphPosX,                           drawY * 2,
                               glyphPosX,                           (drawY * 2) + g_FontLayout->hiResGlyphBottom,
                               glyphPosX + FONT_12X16_GLYPH_SIZE_X, drawY * 2,
                               glyphPosX + FONT_12X16_GLYPH_SIZE_X, (drawY * 2) + g_FontLayout->hiResGlyphBottom);

                        *((u32*)&glyphPoly->u0) = temp_a0 + (vTop << 8) + (g_FontLayout->packedClut << 16);      // `u0`, `v0`, `clut`.
                        *((u32*)&glyphPoly->u1) = temp_a0 + (page << 16) + ((vTop + 15) << 8);                   // `u1`, `v1`, `page`.
                        *((u16*)&glyphPoly->u2) = (u16)(temp_a0 + FONT_12X16_GLYPH_SIZE_X + (vTop << 8));        // `u2`, `v2`.
                        *((u16*)&glyphPoly->u3) = (u16)(temp_a0 + FONT_12X16_GLYPH_SIZE_X + ((vTop + 15) << 8)); // `u3`, `v3`.

                        addPrim(ot, glyphPoly);
                        GsOUT_PACKET_P = (PACKET*)glyphPoly + sizeof(POLY_FT4);
                    }
                    else
                    {
                        temp_a0_2 = (u16)glyphPosX;

                        glyphSprt              = (SPRT*)packet;
                        *((u32*)&glyphSprt->w) = 0x10000C;

                        addPrimFast(ot, glyphSprt, 4);
                        *((u32*)&glyphSprt->r0)   = color;
                        *((u32*)(&glyphSprt->x0)) = temp_a0_2 + (drawY << 16);
                        *((u32*)&glyphSprt->u0)   = temp_a0 + (vTop << 8) + (g_FontLayout->packedClut << 16); // `u0`, `v0`, `clut`.

                        packet += sizeof(SPRT);

                        tPage = (DR_TPAGE*)packet;
                        setDrawTPage(tPage, 0, 1, page);
                        addPrim(ot, tPage);

                        packet += sizeof(DR_TPAGE);
                    }

                    glyphPosX += charWidth;
                }
            }
#else
            if (g_SysWork.enableHighResGlyphs)
            {
                glyphPoly = (POLY_FT4*)GsOUT_PACKET_P;

                idx       = charCode - CHARCODE_OFFSET;
                charWidth = FONT_12X16_GLYPH_WIDTHS[charCode - CHARCODE_OFFSET];

                setPolyFT4(glyphPoly);
                setRGB0(glyphPoly, (s8)color, (s8)(color >> 8), (s8)(color >> 16));
                setXY4(glyphPoly,
                       glyphPosX,                             glyphPosY * 2,
                       glyphPosX,                             (glyphPosY * 2) + 30,
                       glyphPosX + FONT_12X16_GLYPH_SIZE_X, glyphPosY * 2,
                       glyphPosX + FONT_12X16_GLYPH_SIZE_X, (glyphPosY * 2) + 30);

                glyphPosX += charWidth;

                temp_a0 = (idx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X;

                *((u32*)&glyphPoly->u0) = temp_a0 + 0xF000 + (0x7FD3 << 16);                                                     // `u0`, `v0`, `clut`.
                *((u32*)&glyphPoly->u1) = temp_a0 + (((((idx / FONT_12X16_ATLAS_COLUMN_COUNT) & 0xF) | 0x10) << 16) | 0xFF00); // `u1`, `v1`, `page`.
                *((u16*)&glyphPoly->u2) = temp_a0 - 0xFF4;
                *((u16*)&glyphPoly->u3) = temp_a0 - 244;

                addPrim(ot, glyphPoly);
                GsOUT_PACKET_P = (PACKET*)glyphPoly + sizeof(POLY_FT4);
            }
            else
            {
                temp_a0_2 = (u16)glyphPosX;

                glyphSprt              = (SPRT*)packet;
                *((u32*)&glyphSprt->w) = 0x10000C;

                idx        = charCode - CHARCODE_OFFSET;
                glyphPosX += FONT_12X16_GLYPH_WIDTHS[idx];

                addPrimFast(ot, glyphSprt, 4);
                *((u32*)&glyphSprt->r0)   = color;
                *((u32*)(&glyphSprt->x0)) = temp_a0_2 + ((glyphPosY) << 16);
                *((u32*)&glyphSprt->u0)   = (s32)(((idx % FONT_12X16_ATLAS_COLUMN_COUNT) * FONT_12X16_GLYPH_SIZE_X) + 0xF000 + (0x7FD3 << 16)); // `u0`, `v0`, `clut`.

                packet += sizeof(SPRT);

                tPage = (DR_TPAGE*)packet;
                setDrawTPage(tPage, 0, 1, ((idx / FONT_12X16_ATLAS_COLUMN_COUNT) & 0xF) | 0x10);
                addPrim(ot, tPage);

                packet += sizeof(DR_TPAGE);
            }
#endif

            mapMsg++;

            // Stop drawing if length exceeded.
            if (strLength <= 0)
            {
                if (!g_SysWork.enableHighResGlyphs)
                {
                    GsOUT_PACKET_P = packet;
                }

                return result;
            }
        }
    }

    if (!g_SysWork.enableHighResGlyphs)
    {
        GsOUT_PACKET_P = packet;
    }

    Math_DVectorSetFast(&g_StringPosition, glyphPosX, glyphPosY);
    return result;

    #undef LINE_SPACE_SIZE
    #undef CHARCODE_OFFSET
}

void func_8004B658(void) // 0x8004B658
{
    g_MapMsg_GlyphSprite.attribute = 64;
    g_MapMsg_GlyphSprite.cx        = 304;
    g_MapMsg_GlyphSprite.v         = 240;
    g_MapMsg_GlyphSprite.h         = 16;
}

void Gfx_MapMsg_DefaultStringInfoSet(void) // 0x8004B684
{
    g_MapMsg_WidthIdx             = 1;
    D_800C38B0.unused             = 0;
    D_800C38B0.positionIdx        = 1;
    g_StringPositionX1            = SCREEN_POSITION_X(-37.5f);
    g_StringColorId               = StringColorId_White;
    g_SysWork.enableHighResGlyphs = false;
}

void func_8004B6D4(s16 arg0, s16 arg1) // 0x8004B6D4
{
    if (arg0 != NO_VALUE)
    {
        g_MapMsg_GlyphSprite.x = arg0 + (-g_GameWork.gsScreenWidth / 2);
        D_800C391C   = g_MapMsg_GlyphSprite.x;
    }

    if (arg1 != NO_VALUE)
    {
        g_MapMsg_GlyphSprite.y = arg1 + (-g_GameWork.gsScreenHeight / 2);
    }
}

void func_8004B74C(s16 arg0) // 0x8004B74C
{
    if (arg0 < 0 || arg0 >= 5)
    {
        D_800C391E = 0;
        return;
    }

    D_800C391E = arg0;
}

void func_8004B76C(char* str, bool useFixedWidth) // 0x8004B76C
{
    #define GLYPH_SIZE_X       11
    #define GLYPH_SIZE_Y       12
    #define SPACE_SIZE         12
    #define LINE_SPACE_SIZE    16
    #define ATLAS_COLUMN_COUNT 21

    s32       tileRow;
    s32       glyphIdx;
    GsOT*     ot;
    GsSPRITE* glyphSprt;

    glyphSprt  = (GsSPRITE*)PSX_SCRATCH_ADDR(0x30);
    *glyphSprt = g_MapMsg_GlyphSprite;
    ot         = &g_OrderingTable2[g_ActiveBufferIdx];

    // Parse string.
    while (*str != '\0')
    {
        switch (*str)
        {
            // Draw glyph sprite.
            default:
                glyphIdx     = *str - GLYPH_TABLE_ASCII_OFFSET;
                tileRow      = glyphIdx / ATLAS_COLUMN_COUNT;
                glyphSprt->u = (glyphIdx % ATLAS_COLUMN_COUNT) * GLYPH_SIZE_Y;

                if (useFixedWidth)
                {
                    glyphSprt->w = GLYPH_SIZE_X;
                }
                else
                {
                    glyphSprt->w = FONT_12X16_GLYPH_WIDTHS[glyphIdx];
                }

                glyphSprt->tpage = (tileRow & 0xF) | 0x10;
                glyphSprt->cx    = 304;
                glyphSprt->cy    = D_800C391E + 506;

                GsSortFastSprite(glyphSprt, ot, 4);

                glyphSprt->x += glyphSprt->w;
                break;

            // Space.
            case ' ':
            case '\t':
                glyphSprt->x += SPACE_SIZE;
                break;

            // Backspace.
            case '~':
            case '\b':
                glyphSprt->x -= SPACE_SIZE;
                break;

            // Newline.
            case '\n':
                glyphSprt->x  = D_800C391C;
                glyphSprt->y += LINE_SPACE_SIZE;
                break;

            // Carriage return.
            case '\r':
                glyphSprt->x  = D_800C391C;
                glyphSprt->y -= LINE_SPACE_SIZE;
                break;
        }

        str++;
    }

    g_MapMsg_GlyphSprite = *glyphSprt;

    #undef GLYPH_SIZE_X
    #undef GLYPH_SIZE_Y
    #undef SPACE_SIZE
    #undef LINE_SPACE_SIZE
    #undef ATLAS_COLUMN_COUNT
}

void Gfx_StringDrawInt(s32 widthMin, s32 val) // 0x8004B9F8
{
    #define GLYPH_SIZE_X       11
    #define ATLAS_COLUMN_COUNT 10

    s32   quotient;
    s32   isNegative;
    s32   i;
    char* str;

    if (widthMin > 0)
    {
        for (i = 0; i < (widthMin - 1); i++)
        {
            g_MapMsg_GlyphSprite.x += GLYPH_SIZE_X;
        }
    }

    str  = (char*)PSX_SCRATCH_ADDR(0x2F);
    *str = 0;

    if (val < 0)
    {
        isNegative = true;
        val        = -val;
    }
    else
    {
        isNegative = false;
    }

    // Wrap atlas row?
    while (val >= ATLAS_COLUMN_COUNT)
    {
        str--;
        quotient = (val / ATLAS_COLUMN_COUNT) >> 32;
        *str     = (val - (quotient * ATLAS_COLUMN_COUNT)) + '0';

        if (widthMin > 0)
        {
            g_MapMsg_GlyphSprite.x -= GLYPH_SIZE_X;
        }

        val = quotient;
    }

    str--;
    *str = val + '0';

    if (isNegative)
    {
        str--;
        *str          = '-';
        g_MapMsg_GlyphSprite.x -= GLYPH_SIZE_X;
    }

    // Draw numeric string.
    Gfx_StringDraw(str, 5);
    return;

    #undef GLYPH_SIZE_X
    #undef ATLAS_COLUMN_COUNT
}

const s32 unused_Rodata_80025E88 = 0xC9457F00; // @unused
