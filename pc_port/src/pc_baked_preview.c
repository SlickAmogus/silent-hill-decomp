/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_baked_preview.h"
#include <PsyX/common/glad.h>
#include <SDL.h>
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* TSV: key value frames fps relative-PNG-pattern. Manifest and files are local
 * packaged resources. The bounded parser rejects paths outside this folder. */
typedef struct {char key[64],pattern[160];float value;int frames,fps;} Entry;
#define ENTRY_LIMIT 1024
#define FRAME_MEMORY_LIMIT (32u*1024u*1024u)
static Entry entries[ENTRY_LIMIT];
static int count,tried,current=-1,frame=-1;
static GLuint texture;
static int imageW,imageH;
static unsigned char failed[ENTRY_LIMIT];
static unsigned started;
static unsigned char* cached[60];

static void clear_frames(void)
{
    int i;for(i=0;i<60;i++){stbi_image_free(cached[i]);cached[i]=NULL;}
}

static unsigned char* load_frame(const Entry* e,int index,int* w,int* h)
{
    char path[256],name[192];int n;
    if(e->frames>1)snprintf(name,sizeof(name),e->pattern,index);
    else snprintf(name,sizeof(name),"%s",e->pattern);
    snprintf(path,sizeof(path),"gamedata/previews/%s",name);
    FILE* f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)){fclose(f);return NULL;}
    long size=ftell(f);rewind(f);
    if(size<1 || size>4*1024*1024){fclose(f);return NULL;}
    unsigned char* raw=malloc((size_t)size);
    if(!raw){fclose(f);return NULL;}
    size_t got=fread(raw,1,(size_t)size,f);fclose(f);
    if(got!=(size_t)size || !stbi_info_from_memory(raw,(int)size,w,h,&n) ||
       *w<1||*h<1||*w>1280||*h>720 ||
       (size_t)*w**h*4*e->frames>FRAME_MEMORY_LIMIT) {free(raw);return NULL;}
    unsigned char* pixels=stbi_load_from_memory(raw,(int)size,w,h,&n,4);free(raw);
    return pixels;
}

static int safe_pattern(const char* p,int frames)
{
    int conversions=0;
    if(!*p || p[0]=='/' || strstr(p,"..") || strchr(p,'\\'))return 0;
    while(*p) {
        if(*p=='%') {
            if(strncmp(p,"%02d",4))return 0;
            conversions++;p+=4;
        }else {
            if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||strchr("_-./",*p)))return 0;
            p++;
        }
    }
    return conversions==(frames>1);
}

static void load_manifest(void)
{
    char line[320];FILE* f=fopen("gamedata/previews/manifest.tsv","rb");
    tried=1;if(!f)return;
    while(count<ENTRY_LIMIT && fgets(line,sizeof(line),f)) {
        Entry e;char tail;
        if(line[0]=='#')continue;
        if(sscanf(line,"%63s %f %d %d %159s %c",e.key,&e.value,&e.frames,&e.fps,e.pattern,&tail)!=5 ||
           !isfinite(e.value)||e.frames<1||e.frames>60||e.fps<1||e.fps>60||!safe_pattern(e.pattern,e.frames))continue;
        entries[count++]=e;
    }
    fclose(f);
}

int Pc_BakedPreview_Get(const char* key,float value,PcBakedPreview* result)
{
    int i,best=-1,next;float distance=INFINITY;
    if(!result)return 0;
    memset(result,0,sizeof(*result));
    if(!key || !isfinite(value))return 0;
    if(!tried)load_manifest();
    for(i=0;i<count;i++)if(!strcmp(key,entries[i].key)) {
        float d=fabsf(entries[i].value-value);
        if(d<distance){distance=d;best=i;}
    }
    if(best<0)return 0;
    if(failed[best])return 0;
    Entry* e=&entries[best];
    if(current!=best) {
        clear_frames();current=best;frame=-1;started=SDL_GetTicks();
        for(i=0;i<e->frames;i++) {
            if(getenv("SH_PREVIEW_TRACE"))fprintf(stderr,"Preview %s frame %d/%d\n",key,i+1,e->frames);
            int w,h;cached[i]=load_frame(e,i,&w,&h);
            if(!cached[i] || (i && (w!=imageW||h!=imageH))) {
                clear_frames();failed[best]=1;current=-1;return 0;
            }
            imageW=w;imageH=h;
        }
        /* Start after decoding, so loading time does not skip the first frame. */
        started=SDL_GetTicks();
    }
    next=e->frames==1?0:(int)(((SDL_GetTicks()-started)* (unsigned long long)e->fps/1000)%e->frames);
    if(frame!=next) {
        int w=imageW,h=imageH;
        GLint binding,align,pbo,row,rows,skip;
        glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);glGetIntegerv(GL_UNPACK_ALIGNMENT,&align);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,&rows);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skip);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        if(!texture)glGenTextures(1,&texture);
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,cached[next]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        /* Cached decoded frames: animation performs no disk reads or PNG work. */
        glBindTexture(GL_TEXTURE_2D,binding);glPixelStorei(GL_UNPACK_ALIGNMENT,align);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,row);glPixelStorei(GL_UNPACK_SKIP_ROWS,rows);glPixelStorei(GL_UNPACK_SKIP_PIXELS,skip);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
        frame=next;
    }
    result->texture=texture;result->width=imageW;result->height=imageH;
    result->value=e->value;result->animated=e->frames>1;result->approximate=distance>.0001f;
    return texture!=0;
}

void Pc_BakedPreview_Close(void)
{
    if(texture)glDeleteTextures(1,&texture);
    clear_frames();
    texture=0;current=frame=-1;
}
