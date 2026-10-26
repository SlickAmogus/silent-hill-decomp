/* SPDX-License-Identifier: GPL-3.0-or-later */
/* CPU-only disc/font decoding, independent of GL compositor and input state. */
#include "pc_menu_font.h"
#include "pc_config.h"
#include "game.h"
#include "font_region.h"
#include "lang_text.h"
#include "lang_ru.h"
#include "lang_pack.h"
#include "lang_quick.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"
#include <libgpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font_ru.inc"

typedef struct {
    unsigned char* raw;
    TIM_IMAGE tim;
} MenuDiscFont;
static MenuDiscFont s_discFont, s_latinFont;
static MenuDiscFont s_uiRussianFont;
static unsigned char s_uiRussianPixels[128*96];
static RECT s_uiRussianRect={0,0,64,96};
static int s_discRussian=-1;
static const s_FontLayout* s_discLayout;

static unsigned font_le32(const unsigned char* p)
{
    return p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);
}

/* ReadTIM has no size argument. Validate both 4bpp blocks before giving it
 * optional asset data, including the CLUT accessed by the CPU glyph raster. */
static int font_tim(unsigned char* raw,size_t size,TIM_IMAGE* tim)
{
    size_t pos=8;
    int block;
    if(size<8 || font_le32(raw)!=0x10 || font_le32(raw+4)!=8)return 0;
    for(block=0;block<2;block++) {
        unsigned length,w,h;
        if(pos>size || size-pos<12)return 0;
        length=font_le32(raw+pos);
        w=raw[pos+8]|((unsigned)raw[pos+9]<<8);
        h=raw[pos+10]|((unsigned)raw[pos+11]<<8);
        if(length<12 || length>size-pos || !w || !h ||
           (size_t)w*h>(length-12)/2 || (!block && (size_t)w*h<16))return 0;
        pos+=length;
    }
    OpenTIM((u_long*)raw);
    return ReadTIM(tim)!=NULL;
}

int Pc_MenuFontReady(void)
{
    int russian=Pc_RuActive() || Font_RussianLayoutActive();
    /* A PC language pack can own a synthetic PAL atlas instead of the disc
     * TIM. Keep the existing font fallback until that atlas is available here. */
    if (Font_RussianLayoutActive() && !Pc_RuActive()) return 0;
    if (s_discFont.raw && (s_discLayout!=g_FontLayout || s_discRussian!=russian)) {
        free(s_discFont.raw); memset(&s_discFont,0,sizeof(s_discFont));
    }
    if (!s_discFont.raw) {
        const s_FileInfo* info = &g_FileTable[FILE_1ST_FONT16_TIM];
        s_discFont.raw = Pc_LangReadDiscFile(info->startSector, (unsigned)info->blockCount << 8);
        if (!s_discFont.raw) return 0;
        if (!font_tim(s_discFont.raw,(unsigned)info->blockCount<<8,&s_discFont.tim)) {
            free(s_discFont.raw);memset(&s_discFont,0,sizeof(s_discFont));return 0;
        }
        Font_PatchPackGlyphs(s_discFont.tim.paddr,s_discFont.tim.prect->w,s_discFont.tim.prect->h);
        s_discLayout = g_FontLayout;
        s_discRussian=russian;
    }
    if (!s_latinFont.raw) {
        FILE* f = fopen("gamedata/font/eur16.tim","rb");
        if (f) {
            long n;
            fseek(f,0,SEEK_END); n=ftell(f); rewind(f);
            if (n>0 && n<1048576) {
                s_latinFont.raw=malloc((size_t)n);
                if (s_latinFont.raw && fread(s_latinFont.raw,1,(size_t)n,f)==(size_t)n) {
                    if(!font_tim(s_latinFont.raw,(size_t)n,&s_latinFont.tim)) {
                        free(s_latinFont.raw);memset(&s_latinFont,0,sizeof(s_latinFont));
                    }
                } else { free(s_latinFont.raw); s_latinFont.raw=NULL; }
            }
            fclose(f);
        }
    }
    if(s_latinFont.tim.caddr && !s_uiRussianFont.tim.paddr) {
        for(int cell=0;cell<126;cell++)for(int y=0;y<16;y++)for(int x=0;x<6;x++)
            s_uiRussianPixels[((cell/21)*16+y)*128+(cell%21)*6+x]=FONT_RU_ATLAS[cell][y*6+x];
        s_uiRussianFont.tim.paddr=(u_long*)s_uiRussianPixels;
        s_uiRussianFont.tim.prect=&s_uiRussianRect;
        s_uiRussianFont.tim.caddr=s_latinFont.tim.caddr;
    }
    return s_discFont.tim.paddr && s_discFont.tim.caddr;
}

/* Native FONT16 cells, not an approximation using an operating-system font.
 * Latin abbreviations on repainted Russian discs use the shipped retail PAL
 * atlas; Cyrillic uses exactly the mounted translation's glyph mapping. */
static int font_glyph(unsigned cp, const MenuDiscFont** source, int* cell, int* advance)
{
    unsigned b=cp;
    s_GlyphEmit emits[2];
    int n;
    *source=&s_discFont;
    if (cp==' ' || cp=='_') { *advance=4; return 0; }
    if(cp==0x419 && Pc_RuActive() && Pc_RuDiscByte(cp)==Pc_RuDiscByte(0x439))cp=0x418;
    if (cp>=0x400 && cp<=0x4ff) b=Pc_RuActive()?Pc_RuDiscByte(cp):Pc_RuPackByte(cp);
    if(cp>=0x400 && cp<=0x4ff && !Pc_RuActive() && s_uiRussianFont.tim.paddr) {
        *source=&s_uiRussianFont;*cell=(int)b-0x27;
        *advance=*cell>=0 && *cell<126?FONT_RU_WIDTHS[*cell]:5;
        return *cell>=0 && *cell<126;
    }
    if (cp==0x2014 || cp==0x2013) b='-';
    if (cp==0x00ab || cp==0x00bb || cp==0x201c || cp==0x201d) b='\'';
    if (cp<128 && s_latinFont.raw) {
        static const unsigned char widths[84]={
            3,7,7,11,11,4,10,4,6,10,10,10,10,10,10,10,10,10,10,4,4,
            10,11,10,8,13,12,12,12,13,11,11,13,12,9,9,12,12,13,12,13,11,
            13,12,10,11,13,12,12,12,11,12,6,4,6,8,0,3,9,10,9,9,9,
            7,11,11,6,6,10,6,13,11,10,11,10,8,8,7,10,10,12,10,10,9};
        *source=&s_latinFont;
        *cell=(int)b-0x27;
        *advance=*cell>=0 && *cell<84?widths[*cell]:6;
        return *cell>=0 && *cell<84;
    }
    if (b=='!') b='\\';
    if (b=='&') b='^';
    n=Font_MapChar(b,emits);
    if (n>0) { *cell=emits[n-1].cell; *advance=emits[n-1].advance; return 1; }
    *advance=5;
    return 0;
}

float Pc_MenuFontWidth(const char* text,float px)
{
    float w=0,line=0;
    if (!Pc_MenuFontReady()) return 0;
    while (*text) {
        if(*text=='\n'){if(line>w)w=line;line=0;text++;continue;}
        const MenuDiscFont* source; int cell,advance;
        font_glyph(Pc_LangUtf8Next(&text),&source,&cell,&advance);
        line+=advance*px/16;
    }
    return line>w?line:w;
}

unsigned char* Pc_MenuFontRaster(const char* text,float px,int* outW,int* outH)
{
    float pen=1,scale=px/16;
    int w,h,lines=1,lineY=0;
    unsigned char* rgba;
    /* The native rasterizer implements EN/RU, not PAL accents or CJK. Preserve
     * the existing Unicode font path for every other active language. */
    if (!g_PcConfig.uiLanguage && (Pc_LangSjisActive() || (!Pc_RuActive() && !Font_RussianLayoutActive() &&
        strcmp(g_PcConfig.languageCode,"en") && strcmp(g_PcConfig.languageCode,"en-us")))) return NULL;
    if (!text || px<=0 || !outW || !outH) return NULL;
    for(const char* p=text;*p;p++)if(*p=='\n')lines++;
    w=(int)ceilf(Pc_MenuFontWidth(text,px)+px+2);h=(int)ceilf(px+4+3*scale)+(lines-1)*(int)ceilf(px*1.05f);
    if (!Pc_MenuFontReady() || w<=0 || w>4096 || h<=0 || h>512) return NULL;
    rgba=calloc((size_t)w*h,4);
    if (!rgba) return 0;
    while (*text) {
        if(*text=='\n'){pen=1;lineY+=(int)ceilf(px*1.05f);text++;continue;}
        const MenuDiscFont* font;
        int cell,advance;
        unsigned cp=Pc_LangUtf8Next(&text);
        int composeBreve=cp==0x419 && Pc_RuActive() &&
            Pc_RuDiscByte(0x419)==Pc_RuDiscByte(0x439);
        if (font_glyph(composeBreve?0x418:cp,&font,&cell,&advance)) {
            int stride=font->tim.prect->w*2;
            int strip=font->tim.prect->w*4>512;
            int gx=(cell%21)*12+(strip?(cell/21)*256:0);
            int gy=strip?0:(cell/21)*16;
            int x,y;
            const unsigned char* pixels=(const unsigned char*)font->tim.paddr;
            const unsigned short* palette=(const unsigned short*)font->tim.caddr;
            for(y=0;y<(int)ceilf(16*scale);y++) for(x=0;x<(int)ceilf(12*scale);x++) {
                int tx=gx+(int)(x/scale),ty=gy+(int)(y/scale);
                int dx=(int)pen+x,dy=lineY+y+1+(int)ceilf(3*scale),index;
                unsigned short color;
                if (dx<0 || dy<0 || dx>=w || dy>=h || tx<0 || ty<0 ||
                    tx>=stride*2 || ty>=font->tim.prect->h) continue;
                index=(pixels[ty*stride+tx/2]>>((tx&1)*4))&15;
                color=palette[index];
                if (color&0x7fff) {
                    int a=((color&31)+((color>>5)&31)+((color>>10)&31))*255/93;
                    unsigned char* p=rgba+(dy*w+dx)*4;
                    p[0]=p[1]=p[2]=255;
                    if (a>p[3]) p[3]=(unsigned char)a;
                }
            }
            /* ViTCo painted only lowercase й. Keep the capital И body and
             * copy its native lowercase breve, rather than shrinking the whole
             * letter in an uppercase heading. No disc/VRAM atlas is modified. */
            if(composeBreve) {
                const MenuDiscFont* mark;int mc,ma;
                if(font_glyph(0x439,&mark,&mc,&ma)) {
                    int ms=mark->tim.prect->w*2;
                    int stripMark=mark->tim.prect->w*4>512;
                    int mx=(mc%21)*12+(stripMark?(mc/21)*256:0);
                    int my=stripMark?0:(mc/21)*16;
                    const unsigned char* mp=(const unsigned char*)mark->tim.paddr;
                    const unsigned short* cl=(const unsigned short*)mark->tim.caddr;
                    for(y=0;y<(int)ceilf(3*scale);y++)for(x=0;x<(int)ceilf(12*scale);x++) {
                        int tx=mx+(int)(x/scale),ty=my+1+(int)(y/scale);
                        int dx=(int)pen+x,dy=lineY+y,index;
                        if(dx<0||dy<0||dx>=w||dy>=h||tx<0||ty<0||tx>=ms*2||ty>=mark->tim.prect->h)continue;
                        index=(mp[ty*ms+tx/2]>>((tx&1)*4))&15;
                        if(cl[index]&0x7fff){unsigned char* p=rgba+(dy*w+dx)*4;
                            int a=((cl[index]&31)+((cl[index]>>5)&31)+((cl[index]>>10)&31))*255/93;
                            p[0]=p[1]=p[2]=255;if(a>p[3])p[3]=a;}
                    }
                }
            }
        }
        pen+=advance*scale;
    }
    *outW=w;*outH=h;
    return rgba;
}

int Pc_MenuLanguageRussian(void)
{
    if(g_PcConfig.uiLanguage)return g_PcConfig.uiLanguage==2;
    const char* code = g_PcConfig.languageCode;
    return Pc_RuActive() || (Pc_LangPackActive() &&
        code[0] == 'r' && code[1] == 'u' &&
        (code[2] == 0 || code[2] == '-' || code[2] == '_'));
}

int Pc_MenuLanguageSupported(void)
{
    if(g_PcConfig.uiLanguage)return 1;
    return !Pc_LangSjisActive() && (Pc_MenuLanguageRussian() ||
        !strcmp(g_PcConfig.languageCode,"en") || !strcmp(g_PcConfig.languageCode,"en-us"));
}
