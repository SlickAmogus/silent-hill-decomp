/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Title assets/composition only. The original game owns selection/launching;
 * the compositor owns GL shader/VBO state and the borrowed drawing primitives. */
#include "pc_title_screen.h"
#include "pc_title_style.h"
#include "pc_menu_font.h"
#include "pc_mouse_cursor.h"
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/screen/screen_fade.h"
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_render.h>
#include "stb_image.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int Pc_TitleScreen_Available(void)
{
    extern int Pc_TitleMenu_Rows(int*,int*,int*);
    int ids[6],selected,difficulty;
    return Pc_MenuLanguageSupported() &&
        Pc_MenuFontReady() && Pc_TitleMenu_Rows(ids,&selected,&difficulty)>0;
}

int Pc_TitleMenu_MouseRow(int count)
{
    float x,y;
    if(!Pc_MouseCursor_ViewportPos(&x,&y) || x<.28f || x>.72f)return -1;
    float pitch=count>5?.038f:.047f;
    int row=(int)floorf((y-.61f)/pitch);
    return y>=.61f && row>=0 && row<count?row:-1;
}

static unsigned char* title_read_file(const char* path, long* outSize)
{
    FILE* f = fopen(path, "rb");
    long  n;
    unsigned char* buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    if (outSize) *outSize = n;
    buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

static void title_image(const PcTitlePainter* painter,GLuint texture,float x,float y,float w,float h,
                           float vpW,float vpH,float brightness,float opacity)
{
    painter->image(texture,x,y,w,h,vpW,vpH,brightness,brightness,brightness,opacity);
}

/* Reconstruct the original palette image once, in linear-sized pixels, before
 * GPU scaling. A conservative cubic filter softens enlarged stair-steps without
 * inventing replacement art or changing the original logo/font designs. */
static float title_cubic(float a,float b,float c,float d,float t)
{
    return b+.5f*t*(c-a+t*(2*a-5*b+4*c-d+t*(3*(b-c)+d-a)));
}

static unsigned char* title_reconstruct(const unsigned char* src,int w,int h)
{
    unsigned char* out=malloc((size_t)w*h*16);
    int x,y,c,j,k;
    if(!out)return NULL;
    for(y=0;y<h*2;y++)for(x=0;x<w*2;x++) {
        float sx=(x+.5f)*.5f-.5f,sy=(y+.5f)*.5f-.5f;
        int ix=(int)floorf(sx),iy=(int)floorf(sy);
        for(c=0;c<3;c++) {
            float col[4];
            for(j=0;j<4;j++) {
                float row[4];int yy=iy+j-1;if(yy<0)yy=0;if(yy>=h)yy=h-1;
                for(k=0;k<4;k++) {int xx=ix+k-1;if(xx<0)xx=0;if(xx>=w)xx=w-1;
                    row[k]=src[(yy*w+xx)*4+c];}
                col[j]=title_cubic(row[0],row[1],row[2],row[3],sx-ix);
            }
            float v=title_cubic(col[0],col[1],col[2],col[3],sy-iy);
            out[(y*w*2+x)*4+c]=(unsigned char)fminf(255,fmaxf(0,v));
        }
        out[(y*w*2+x)*4+3]=255;
    }
    return out;
}

static GLuint title_png(const char* name,int* width,int* height)
{
    int comp;GLuint tex=0;
    long length=0;
    unsigned char* file=title_read_file(name,&length);
    unsigned char* data=file?stbi_load_from_memory(file,(int)length,width,height,&comp,4):NULL;
    free(file);
    if(!data)return 0;
    glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,*width,*height,0,GL_RGBA,GL_UNSIGNED_BYTE,data);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    stbi_image_free(data);return tex;
}

typedef struct {
    GLuint texture;
    GLuint wideTexture;
    int wideW,wideH,wideTried;
    GLuint sides[2];
    GLuint labels[20],fogTexture;
    int labelW[20],labelH[20];
    int style;
} TitleAssets;
static TitleAssets s_assets={.style=-99};

static int title_load_art(void)
{
    int pal=Pc_TitleUsesPalLogo(),tw=320,th=pal?96:480,i;
    if(s_assets.texture && s_assets.style==Pc_TitleStyle())return 1;
    {
        unsigned short raw[160*480],clut[256];
        unsigned char* rgba=malloc((size_t)tw*th*4);
        if(!rgba)return 0;
        GR_ReadVRAM(raw,pal?896:864,0,pal?80:160,th);
        GR_ReadVRAM(clut,g_TitleImg.clutX,g_TitleImg.clutY,pal?16:256,1);
        for(i=0;i<tw*th;i++) {
            int c=clut[pal?((raw[i/4]>>((i%4)*4))&15):((raw[i/2]>>((i%2)*8))&255)];
            rgba[i*4]=(c&31)*255/31;rgba[i*4+1]=((c>>5)&31)*255/31;
            rgba[i*4+2]=((c>>10)&31)*255/31;rgba[i*4+3]=255;
        }
        if(s_assets.texture)glDeleteTextures(1,&s_assets.texture);
        int highW,highH;
        s_assets.texture=title_png(pal?"gamedata/menu-hd/title-pal.png":
            Pc_TitleUsesJpArt()?"gamedata/menu-hd/title-jp.png":"gamedata/menu-hd/title-us.png",&highW,&highH);
        if(!s_assets.texture) {
            glGenTextures(1,&s_assets.texture);
            glBindTexture(GL_TEXTURE_2D,s_assets.texture);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
            unsigned char* smooth=title_reconstruct(rgba,tw,th);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,smooth?tw*2:tw,smooth?th*2:th,0,GL_RGBA,GL_UNSIGNED_BYTE,smooth?smooth:rgba);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            free(smooth);
        }
        if(s_assets.wideTexture)glDeleteTextures(1,&s_assets.wideTexture);
        s_assets.wideTexture=0;s_assets.wideTried=0;
        free(rgba);s_assets.style=Pc_TitleStyle();
    }
    return 1;
}

static void title_draw_background(const PcTitlePainter* painter)
{
    float w=painter->width,h=painter->height;
    float artW=h*4/3.f,artH=h,artX,artY;
    int pal=Pc_TitleUsesPalLogo();
    GLuint artTexture=s_assets.texture;
    if(!pal && !Pc_TitleUsesJpArt() && w/h>=1.55f) {
        if(!s_assets.wideTried) {
            s_assets.wideTried=1;
            s_assets.wideTexture=title_png("gamedata/menu-hd/title-us-wide.png",&s_assets.wideW,&s_assets.wideH);
        }
        if(s_assets.wideTexture && s_assets.wideH>0) {
            artTexture=s_assets.wideTexture;
            artW=h*(float)s_assets.wideW/s_assets.wideH;
        }
    }
    /* Contain the chosen composition: never squeeze it or crop its logo. The
     * separate 4:3 and widescreen assets can switch immediately on resize. */
    if(artW>w) {artH*=w/artW;artW=w;}
    artX=(w-artW)*.5f;artY=(h-artH)*.5f;
    painter->rect(0,0,w,h,w,h,0,0,0,1);
    if(pal)title_image(painter,s_assets.texture,artX,h*.19f,artW,artW*.15f,w,h,1,1);
    else {
        /* Fill only the extra side area with dim, blurred unlettered edges.
         * The complete central composition is neither stretched nor cropped. */
        if(artX>0 && !Pc_TitleUsesJpArt()) {
            int sw,sh;
            if(!s_assets.sides[0])s_assets.sides[0]=title_png("gamedata/menu-hd/side-left.png",&sw,&sh);
            if(!s_assets.sides[1])s_assets.sides[1]=title_png("gamedata/menu-hd/side-right.png",&sw,&sh);
            if(s_assets.sides[0])title_image(painter,s_assets.sides[0],0,0,artX,h,w,h,.65f,1);
            if(s_assets.sides[1])title_image(painter,s_assets.sides[1],w-artX,0,artX,h,w,h,.65f,1);
        }
        title_image(painter,artTexture,artX,artY,artW,artH,w,h,1,1);
    }
}

static void title_draw_fog(const PcTitlePainter* painter)
{
    float w=painter->width,h=painter->height;
    int i;
    /* Retail fog simulation is still advanced by MainMenu_FogUpdate. Upload
     * its current vertex colours, not a canned image, over the same lower band. */
    {
        unsigned char rgb[441*3],rgba[21*12*4];
        extern void Pc_TitleFogRgb(unsigned char*);
        Pc_TitleFogRgb(rgb);
        for(i=0;i<21*12;i++) {
            memcpy(rgba+i*4,rgb+(9*21+i)*3,3);rgba[i*4+3]=255;
        }
        if(!s_assets.fogTexture) {
            glGenTextures(1,&s_assets.fogTexture);glBindTexture(GL_TEXTURE_2D,s_assets.fogTexture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,21,12,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        } else {glBindTexture(GL_TEXTURE_2D,s_assets.fogTexture);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,21,12,GL_RGBA,GL_UNSIGNED_BYTE,rgba);}
        /* PSX tpage 42 uses ABR=1 (additive), not an opaque fog photograph. */
        glBlendFunc(GL_ONE,GL_ONE);
        title_image(painter,s_assets.fogTexture,0,h*232/448.f,w,h*264/448.f,w,h,.5f,1);
        glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    }
}

static void title_draw_entries(const PcTitlePainter* painter)
{
    float w=painter->width,h=painter->height,px=h*.034f;
    int ids[6],selected,difficulty,count,i;
    int ru=Pc_MenuLanguageRussian();
    extern int Pc_TitleMenu_Rows(int*,int*,int*);
    static const char* names[][2]={{"LOAD","ЗАГРУЗКА"},{"CONTINUE","ДАЛЕЕ"},
        {"START","СТАРТ"},{"OPTION","ОПЦИИ"},{"ACHIEVEMENTS","ДОСТИЖЕНИЯ"},{"EXIT","ВЫХОД"}};
    static const char* levels[][2]={{"EASY","ЛЕГКО"},{"NORMAL","НОРМА"},{"HARD","ТРУДНО"}};
    count=Pc_TitleMenu_Rows(ids,&selected,&difficulty);
    for(i=0;i<count;i++) {
        int active=ids[i]==selected,idx=difficulty?6+ids[i]:ids[i];
        if(ru)idx+=9;
        float y=h*(.61f+i*(count>5?.038f:.047f));
        float r=active?48/255.f:.83f,g=active?1:.83f,b=active?80/255.f:.83f;
        const char* text=difficulty?levels[ids[i]][ru]:names[ids[i]][ru];
        if(!s_assets.labels[idx]) {char file[96];snprintf(file,sizeof(file),"gamedata/menu-hd/label-%02d.png",idx);
            s_assets.labels[idx]=title_png(file,&s_assets.labelW[idx],&s_assets.labelH[idx]);}
        if(s_assets.labels[idx]) {
            float lh=px*s_assets.labelH[idx]/64.f,lw=px*s_assets.labelW[idx]/64.f;
            painter->image(s_assets.labels[idx],(w-lw)/2,y,lw,lh,w,h,r,g,b,1);
        } else {
            float lw=Pc_MenuFontWidth(text,px);
            painter->text(3+i,text,px,(w-lw)/2,y,w*.6f,w,h,r,g,b);
        }
    }
}

void Pc_TitleScreen_Draw(const PcTitlePainter* painter)
{
    float w=painter->width,h=painter->height;
    if(!title_load_art())return;
    title_draw_background(painter);
    title_draw_fog(painter);
    title_draw_entries(painter);
    /* The GL compositor is after the PSX fade, so reproduce its intensity. */
    float fade=1-Screen_FadeInProgressGet()/4096.f;
    if(fade>0)painter->rect(0,0,w,h,w,h,0,0,0,fminf(fade,1));
}
