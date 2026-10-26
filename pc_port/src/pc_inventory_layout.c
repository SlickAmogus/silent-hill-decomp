/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_inventory_layout.h"
#include <math.h>
#ifndef PC_INVENTORY_LAYOUT_TEST
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/item_screens.h"
#include "hires_override.h"
#include "pc_menu_font.h"
#include "lang_ru.h"
#include "lang_text.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <PsyX/common/glad.h>
#include <SDL.h>
#endif

int Pc_InventoryLayout(float w,float h,PcInventoryPanel p[7])
{
    if(w<=0 || h<=0)return 0;
    /* Recompose the existing panels, rather than replace their text, animated
     * health scan, model carousel or command state machine. */
    const float src[7][4]={{0,0,106,180},{106,0,108,180},{214,0,106,180},
        {0,180,320,130},{0,310,320,95},{0,405,320,43},{0,0,0,0}};
    const float dst[6][4]={{.035f,.025f,.28f,.37f},{.36f,.025f,.28f,.37f},
        {.685f,.025f,.28f,.37f},{.035f,.40f,.93f,.285f},
        {.035f,.70f,.93f,.18f},{.035f,.90f,.93f,.085f}};
    for(int i=0;i<6;i++) {
        p[i]=(PcInventoryPanel){src[i][0]/320,src[i][1]/448,src[i][2]/320,src[i][3]/448,
            dst[i][0]*w,dst[i][1]*h,dst[i][2]*w,dst[i][3]*h};
        /* Preserve the authored 4:3 display proportions of every region. */
        float aspect=(src[i][2]/320.f)*(4.f/3.f)/(src[i][3]/448.f);
        float ph=fminf(p[i].h,p[i].w/aspect),pw=ph*aspect;
        p[i].x+=(p[i].w-pw)*.5f;p[i].y+=(p[i].h-ph)*.5f;
        p[i].w=pw;p[i].h=ph;
    }
    return 6;
}

int Pc_InventoryCommandRow(float x,float y,float w,float h,int count)
{
    PcInventoryPanel p[7];if(!Pc_InventoryLayout(w,h,p))return -1;
    if(x<p[2].x+p[2].w*.03f || x>p[2].x+p[2].w*.94f)return -1;
    for(int i=0;i<count;i++)if(y>=h*(.17f+i*.085f)-h*.005f &&
        y<=h*(.17f+i*.085f)+fminf(50,h*.04f)*1.3f)return i;
    return -1;
}

int Pc_InventoryPointer(float x,float y,float w,float h,float* ox,float* oy)
{
    if(w>0 && h>0 && y>=h*.90f && y<=h*.985f) {
        for(int i=0;i<3;i++)if(x>=w*(.10f+i*.285f) && x<=w*(.33f+i*.285f)) {
            *ox=(64.f+i*96.f)/320.f;*oy=421.f/448.f;return 1;
        }
        return 0;
    }
    PcInventoryPanel p[7];int n=Pc_InventoryLayout(w,h,p);
    for(int i=0;i<n;i++)if(x>=p[i].x && x<=p[i].x+p[i].w && y>=p[i].y && y<=p[i].y+p[i].h) {
        *ox=p[i].sx+(x-p[i].x)/p[i].w*p[i].sw;
        *oy=p[i].sy+(y-p[i].y)/p[i].h*p[i].sh;return 1;
    }
    return 0;
}

#ifndef PC_INVENTORY_LAYOUT_TEST
static GLuint tiles[6];
static GLuint resolveFbo,resolveTexture;
static int resolveWidth,resolveHeight,resolveAlpha;
static int widths[6],heights[6],formats[6],portraitTried;
static int textMode;
typedef struct { char text[1536]; int x,y; } InventoryText;
static InventoryText texts[24];
static int textCount;

void Pc_InventoryTextBegin(int commands)
{
    textMode=Pc_InventoryAdapted()?(commands?2:1):0;
    if(textMode==2)textCount=0;
}
void Pc_InventoryTextEnd(void){textMode=0;}

int Pc_InventoryTextCapture(const char* text,int length,int x,int y,int* complete)
{
    if(!textMode)return 0;
    if(!text){*complete=1;return 1;}
    char partial[768],utf8[1536];int n=0,remaining=length;
    /* Descriptions and names are complete in the native inventory. Command
     * capture retains the caller's budget; other game text is not intercepted. */
    if(textMode==1)remaining=(int)sizeof(partial);
    while(text[n] && n<(int)sizeof(partial)-1 && remaining>0) {
        unsigned char c=(unsigned char)text[n];partial[n++]=(char)c;
        if(c>=0x27 && c!='_')remaining--;
    }
    partial[n]=0;*complete=text[n]==0 && remaining>0;
    const char* menu=Pc_RuMenuUtf8(text);
    if(!strcmp(text,"Stock:"))menu="Количество:";
    else if(!strcmp(text,"Fuel:"))menu="Топливо:";
    if(menu || textMode==2) {
        snprintf(utf8,sizeof(utf8),"%s",menu && Pc_MenuLanguageRussian()?menu:partial);
        for(char* p=utf8;*p;p++)if(*p=='_')*p=' ';
    }
    else {
        int numeric=1;
        for(int i=0;i<n;i++)if(partial[i]!=' ' && partial[i]!='_' &&
            (partial[i]<'0'||partial[i]>'9'))numeric=0;
        if(numeric)snprintf(utf8,sizeof(utf8),"%s",partial);
        else Pc_RuDiscTextUtf8(textMode==2?Pc_LangMenuText(partial):partial,utf8,sizeof(utf8));
    }
    if(textCount<24 && utf8[0]) {
        InventoryText* entry=&texts[textCount++];entry->x=x+160;entry->y=y+112;
        snprintf(entry->text,sizeof(entry->text),"%s",utf8);
    }
    return 1;
}

static void Inventory_Paragraph(const PcTitlePainter* painter,const char* text,int* slot,
                                float px,float x,float y,float width,float bottom)
{
    /* Fit the complete description, rather than silently dropping the final
     * authored lines. The same glyph advances are used for measuring and draw. */
    for(int attempt=0;attempt<8;attempt++) {
        const char* p=text;int lines=0;float line=0,word=0;
        while(*p) {
            const char* first=p;unsigned char c=(unsigned char)*p++;
            if(c>=0xc0)while((*p&0xc0)==0x80)p++;
            char glyph[8];size_t length=(size_t)(p-first);
            if(length>=sizeof(glyph))break;
            memcpy(glyph,first,length);glyph[length]=0;
            float advance=Pc_MenuFontWidth(glyph,px);
            if(c=='\n'){lines++;line=word=0;continue;}
            if(c==' ')word=0;else word+=advance;
            line+=advance;
            if(line>width){lines++;line=word;}
        }
        lines++;
        if(lines*px*1.25f<=bottom-y || px<=painter->height*.022f)break;
        px*=.9f;
    }
    const char* start=text;char line[768];
    while(*start && y+px<=bottom && *slot<94) {
        const char* end=start,*space=NULL,*next;size_t n=0;
        while(*end) {
            if(*end=='\n')break;
            next=end;unsigned char c=(unsigned char)*next++;
            if(c>=0xc0)while((*next&0xc0)==0x80)next++;
            size_t size=(size_t)(next-start);if(size>=sizeof(line))break;
            memcpy(line,start,size);line[size]=0;
            if(Pc_MenuFontWidth(line,px)>width && n)break;
            if(*end==' ')space=end;
            end=next;n=size;
        }
        if(*end && *end!='\n' && space && space>start)end=space;
        if(end==start)end=start+1;
        n=(size_t)(end-start);if(n>=sizeof(line))n=sizeof(line)-1;
        memcpy(line,start,n);line[n]=0;
        float ink=fminf(width,Pc_MenuFontWidth(line,px));
        painter->text((*slot)++,line,px,x+(width-ink)*.5f,y,width,painter->width,painter->height,.88f,.9f,.88f);
        start=end;while(*start==' ' || *start=='\n' || *start=='\t')start++;
        y+=px*1.25f;
    }
}

void Pc_InventoryClose(void)
{
    if(resolveFbo)glDeleteFramebuffers(1,&resolveFbo);
    if(resolveTexture)glDeleteTextures(1,&resolveTexture);
    resolveFbo=resolveTexture=0;resolveWidth=resolveHeight=0;
    if(tiles[0])glDeleteTextures(6,tiles);
    for(int i=0;i<6;i++){tiles[i]=0;widths[i]=heights[i]=0;}
    portraitTried=0;
    textCount=0;textMode=0;
}

int Pc_InventoryAdapted(void)
{
    return g_GameWork.gameState==GameState_InventoryScreen &&
        Pc_MenuLanguageSupported() && !Pc_LangSjisActive() &&
        g_GameWork.gameStateSteps[1]>0 && g_GameWork.gameStateSteps[1]<21 &&
        g_Inventory_SelectionId<InventorySelectionId_Examine;
}

void Pc_InventoryDraw(const PcTitlePainter* painter)
{
    float w=painter->width,h=painter->height;
    GLint readFbo,readBuffer,drawFbo,tex,unpack,vp[4],alignment,rowLength,skipRows,skipPixels;
    PcInventoryPanel p[7];
    if(!Pc_InventoryAdapted())return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);glGetIntegerv(GL_READ_BUFFER,&readBuffer);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo);glGetIntegerv(GL_VIEWPORT,vp);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&tex);glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&rowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skipRows);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skipPixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    /* Isolated optional HEROPIC2 override; the game's native health scanline
     * and colour animation continue to run, and the BIN remains unchanged. */
    if(!portraitTried) {
        portraitTried=1;
        int nw,nh,ox,oy,hw,hh;
        /* Do not grow the override table on every inventory opening or replace
         * an existing user-supplied portrait override. */
        unsigned existing=HiresOverride_LookupByTpageClut(159,846,&nw,&nh,&ox,&oy,&hw,&hh);
        FILE* f=existing?NULL:fopen("gamedata/menu-hd/inventory-portrait.png","rb");
        if(f) {
            fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);
            if(n>0 && n<=4*1024*1024) {
                unsigned char* data=malloc((size_t)n);
                if(data && fread(data,1,(size_t)n,f)==(size_t)n)
                    HiresOverride_RegisterFromTim("inventory-portrait",data,(unsigned)n,972,359,52,152,224,13,8);
                free(data);
            }
            fclose(f);
        }
    }
    /* Native-resolution overlays draw into the window; ordinary overlays draw
     * into the scene FBO, which must first be resolved if MSAA is enabled. */
    extern unsigned int GR_ScreenReadFBO(void);
    int windowTarget=(GLuint)drawFbo==PSYX_DEFAULT_FBO;
    int windowAlpha=8;
    if(windowTarget)SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE,&windowAlpha);
    GLint tileFormat=windowTarget && windowAlpha==0?GL_RGB8:GL_RGBA8;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,windowTarget?(GLuint)drawFbo:(GLuint)GR_ScreenReadFBO());
    glReadBuffer(windowTarget && !drawFbo?GL_BACK:GL_COLOR_ATTACHMENT0);
    int sourceX=vp[0],sourceY=vp[1];
    if(windowTarget) {
        GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);
        if(samples>0) {
            /* GLES forbids CopyTexSubImage from a multisampled window. Resolve
             * once, without scaling, using the window's exact RGB/RGBA format. */
            int alpha=8;SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE,&alpha);
            if(!resolveFbo)glGenFramebuffers(1,&resolveFbo);
            if(!resolveTexture)glGenTextures(1,&resolveTexture);
            glBindTexture(GL_TEXTURE_2D,resolveTexture);
            if(resolveWidth!=vp[2] || resolveHeight!=vp[3] || resolveAlpha!=alpha) {
                glTexImage2D(GL_TEXTURE_2D,0,alpha?GL_RGBA8:GL_RGB8,vp[2],vp[3],0,
                    alpha?GL_RGBA:GL_RGB,GL_UNSIGNED_BYTE,NULL);
                resolveWidth=vp[2];resolveHeight=vp[3];resolveAlpha=alpha;
            }
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,resolveFbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,resolveTexture,0);
            GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(vp[0],vp[1],vp[0]+vp[2],vp[1]+vp[3],0,0,vp[2],vp[3],GL_COLOR_BUFFER_BIT,GL_NEAREST);
            if(scissor)glEnable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)drawFbo);
            glBindFramebuffer(GL_READ_FRAMEBUFFER,resolveFbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            sourceX=sourceY=0;
        }
    }
    if(!tiles[0])glGenTextures(6,tiles);
    /* Individual tile textures let the compositor's simple 0..1 painter keep
     * owning shaders/VAOs and all GL state. Copy from the existing read target. */
    int n=Pc_InventoryLayout(w,h,p);
    for(int i=0;i<n;i++) {
        int x=sourceX+(int)(p[i].sx*w),y=sourceY+(int)((1-p[i].sy-p[i].sh)*h);
        int tw=(int)(p[i].sw*w),th=(int)(p[i].sh*h);
        glBindTexture(GL_TEXTURE_2D,tiles[i]);
        if(widths[i]!=tw || heights[i]!=th || formats[i]!=tileFormat) {
            glTexImage2D(GL_TEXTURE_2D,0,tileFormat,tw,th,0,tileFormat==GL_RGB8?GL_RGB:GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            widths[i]=tw;heights[i]=th;formats[i]=tileFormat;
        }
        glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,x,y,tw,th);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    painter->rect(0,0,w,h,w,h,0,0,0,1);
    for(int i=0;i<n-1;i++)painter->image(tiles[i],p[i].x-(i==0?p[i].w*.105f:0),p[i].y+p[i].h,p[i].w,-p[i].h,w,h,1,1,1,1);
    /* The legacy strips cross the old panel boundaries. Do not magnify those
     * clipped fragments: replace this narrow decoration band consistently. */
    painter->rect(0,h*.075f,w,h*.042f,w,h,0,0,0,1);
    static const char* labels[6][2]={{"STATUS","СТАТУС"},{"EQUIPMENT","СНАРЯЖЕНИЕ"},
        {"COMMAND","КОМАНДЫ"},{"OPTIONS","ОПЦИИ"},{"EXIT","ВЫХОД"},{"MAP","КАРТА"}};
    int ru=Pc_MenuLanguageRussian();float px=fminf(60,h*.041f);
    for(int i=0;i<6;i++) {
        float width=i<3?p[i].w:w*.23f;
        float x=i<3?p[i].x:w*(.10f+(i-3)*.285f);
        float y=i<3?h*.03f:h*.91f;
        float font=px;
        float ink=Pc_MenuFontWidth(labels[i][ru],font);
        if(ink>width*.94f){font*=width*.94f/ink;ink=Pc_MenuFontWidth(labels[i][ru],font);}
        float barY=i<3?h*.10f:h*.955f;
        for(int j=0;j<8;j++) {
            float glow=(j<4?j+1:8-j)/4.f;
            painter->rect(x,barY+j*h*.0012f,width,h*.0013f,w,h,0,.24f*glow,.34f*glow,1);
        }
        x+=(width-ink)*.5f;
        if(i>=3) {
            int id=i==3?InventorySelectionId_Settings:i==4?InventorySelectionId_Exit:InventorySelectionId_Map;
            float r=g_Inventory_SelectionId==id?48/255.f:.84f;
            float g=g_Inventory_SelectionId==id?1:.87f;
            float b=g_Inventory_SelectionId==id?80/255.f:.84f;
            painter->text(64+i,labels[i][ru],font,x,y,width,w,h,r,g,b);
        }else painter->text(64+i,labels[i][ru],font,x,y,width,w,h,.84f,.87f,.84f);
    }
    /* Draw the selector only after recomposition: one axis-aligned outline,
     * never a delayed legacy trail split between two copied panels. */
    int selection=g_Inventory_SelectionId,nav=-1;float bx=0,by=0,bw=0,bh=0;
    if(selection==InventorySelectionId_Item) {
        bx=p[3].x+p[3].w*.375f;by=p[3].y+p[3].h*.015f;bw=p[3].w*.25f;bh=p[3].h*.94f;
    }else if(selection==InventorySelectionId_EquippedItem) {
        bx=p[1].x;by=h*.12f;bw=p[1].w;bh=h*.18f;
    }else if(selection==InventorySelectionId_Health) {
        bx=p[0].x;by=h*.12f;bw=p[0].w;bh=h*.255f;
    }else if(selection==InventorySelectionId_ItemCmd || selection==InventorySelectionId_EquippedItemCmd) {
        bx=p[2].x;by=h*.12f;bw=p[2].w;bh=h*.25f;
    }else if(selection==InventorySelectionId_Settings)nav=0;
    else if(selection==InventorySelectionId_Exit)nav=1;
    else if(selection==InventorySelectionId_Map)nav=2;
    if(nav>=0){bx=w*(.10f+nav*.285f);by=h*.895f;bw=w*.23f;bh=h*.085f;}
    if(bw>0) {
        float t=fmaxf(2,h*.002f);
        painter->rect(bx,by,bw,t,w,h,.36f,.34f,.72f,1);
        painter->rect(bx,by+bh-t,bw,t,w,h,.36f,.34f,.72f,1);
        painter->rect(bx,by,t,bh,w,h,.36f,.34f,.72f,1);
        painter->rect(bx+bw-t,by,t,bh,w,h,.36f,.34f,.72f,1);
    }
    int slot=70,commandRow=0;
    float detailFont=fminf(48,h*.035f),commandFont=fminf(50,h*.04f);
    for(int i=0;i<textCount && slot<94;i++) {
        InventoryText* text=&texts[i];float x,y,area;
        if(text->y<0) {
            x=p[2].x+p[2].w*.06f;y=h*(.17f+commandRow*.085f);area=p[2].w*.88f;
            int selected=(selection==InventorySelectionId_ItemCmd || selection==InventorySelectionId_EquippedItemCmd) && commandRow==g_Inventory_CmdSelectedIdx;
            if(selected)painter->rect(x-h*.007f,y-h*.005f,area,commandFont*1.3f,w,h,0,.22f,.08f,1);
            painter->text(slot++,text->text,commandFont,x,y,area,w,h,selected?.18f:.76f,selected?1:.8f,selected?.31f:.76f);
            commandRow++;continue;
        }
        if(text->y==30) {x=p[1].x;y=h*.325f;area=p[1].w;}
        else if(text->y==184)continue; /* Internal inventory slot number, not an item quantity. */
        else if(text->y==176 || text->y==200){x=w*(text->x>=260?.87f:.72f);y=h*.70f;area=w*.12f;}
        else if(text->y==208){area=w*.48f;x=(w-fminf(area,Pc_MenuFontWidth(text->text,detailFont)))*.5f;y=h*.70f;}
        else {Inventory_Paragraph(painter,text->text,&slot,detailFont,w*.10f,h*.765f,w*.80f,h*.885f);continue;}
        /* Numeric parts of the equipped-ammunition line retain their order but
         * are positioned independently of the original pixel coordinates. */
        if(text->y==30 && text->x>122)x+=p[1].w*.70f;
        painter->text(slot++,text->text,detailFont,x,y,area,w,h,.88f,.9f,.88f);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,(GLuint)unpack);glBindTexture(GL_TEXTURE_2D,(GLuint)tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);glPixelStorei(GL_UNPACK_ROW_LENGTH,rowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS,skipRows);glPixelStorei(GL_UNPACK_SKIP_PIXELS,skipPixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)readFbo);glReadBuffer((GLenum)readBuffer);
}
#endif
