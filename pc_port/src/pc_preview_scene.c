/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_preview_scene.h"
#include "pc_preview_target.h"
#include "pc_preview_mesh.h"
#include "pc_preview_tim.h"
#include "lang_text.h"
#include "main/fileinfo.h"
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static struct {
    PcPreviewTarget target;
    GLuint program,vao,buffer,texture;
    GLsizei vertices;
    int attempted,rendered;
    float centre[3],scale;
} s_scene;

static GLuint compile(GLenum kind,const char* source)
{
    GLuint shader=glCreateShader(kind);GLint ok;
    const char* parts[]={PsyX_Shader_Preamble(kind==GL_VERTEX_SHADER,PSYX_GLSL_LEGACY),source};
    glShaderSource(shader,2,parts,NULL);glCompileShader(shader);
    glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[512];glGetShaderInfoLog(shader,sizeof(log),NULL,log);
        SDL_Log("[PREVIEW-SCENE] shader: %s",log);glDeleteShader(shader);return 0;}
    return shader;
}

void Pc_PreviewScene_Destroy(void)
{
    int hadResources=s_scene.target.framebuffer!=0;
    Pc_PreviewTarget_Destroy(&s_scene.target);
    if(s_scene.buffer)glDeleteBuffers(1,&s_scene.buffer);
    if(s_scene.vao)glDeleteVertexArrays(1,&s_scene.vao);
    if(s_scene.program)glDeleteProgram(s_scene.program);
    if(s_scene.texture)glDeleteTextures(1,&s_scene.texture);
    memset(&s_scene,0,sizeof(s_scene));
    if(hadResources)SDL_Log("[PREVIEW-SCENE] diagnostic GPU resources released");
}

static int load(void)
{
    /* Raw bytes are read into an owned buffer. No Fs queue, model reformatting,
     * shared VRAM upload, map callbacks or game-state changes are involved. */
    const s_FileInfo* file=&g_FileTable[FILE_BG_SC0000_IPD];
    size_t size=(size_t)file->blockCount*256;
    if(size<0x188 || size>4*1024*1024)return 0;
    unsigned char* bytes=Pc_LangReadDiscFile(file->startSector,(unsigned)size);
    if(!bytes)return 0;
    size_t lm=(size_t)bytes[4]|(size_t)bytes[5]<<8|(size_t)bytes[6]<<16|(size_t)bytes[7]<<24;
    PcPreviewMesh mesh={0};
    int decoded=lm<size && Pc_PreviewMesh_Decode(bytes+lm,size-lm,0,&mesh);
    if(!decoded){free(bytes);SDL_Log("[PREVIEW-SCENE] school mesh decode failed");return 0;}
    /* Keep the two raw material sheets local too. Do not upload to shared PSX
     * VRAM: palette selection is decoded into this preview's own atlas. */
    const unsigned char* lmBytes=bytes+lm;
    size_t materials=(size_t)lmBytes[4]|(size_t)lmBytes[5]<<8|
        (size_t)lmBytes[6]<<16|(size_t)lmBytes[7]<<24;
    unsigned materialCount=lmBytes[3];
    unsigned char* sheets[16]={0};size_t sheetSizes[16]={0};
    if(materialCount>16 || materials>size-lm || materialCount>(size-lm-materials)/24) {
        free(bytes);Pc_PreviewMesh_Free(&mesh);return 0;
    }
    for(unsigned i=0;i<materialCount;i++) {
        char name[9];memcpy(name,lmBytes+materials+i*24,8);name[8]=0;
        int idx=Fs_FindNextFile(name,FileType_Tim,0);
        if(idx>=0 && idx<FS_FILE_COUNT) {
            const s_FileInfo* info=&g_FileTable[idx];sheetSizes[i]=(size_t)info->blockCount*256;
            if(sheetSizes[i]>0 && sheetSizes[i]<=4*1024*1024)
                sheets[i]=Pc_LangReadDiscFile(info->startSector,(unsigned)sheetSizes[i]);
        }
    }
    free(bytes);
    static const char* vs=
        "attribute vec3 a_position; attribute vec3 a_normal; attribute vec2 a_uv;\n"
        "uniform vec3 u_centre; uniform float u_scale; varying vec2 v_uv;\n"
        "varying float v_light;\n"
        "void main(){vec3 p=(a_position-u_centre)*u_scale;\n"
        "v_uv=a_uv;v_light=.4+.6*abs(dot(a_normal,normalize(vec3(.3,.8,.5))));\n"
        "vec3 q=vec3(.866*p.x+.5*p.z,.28*p.x+.82*p.y-.485*p.z,\n"
        "-.41*p.x+.574*p.y+.71*p.z);\n"
        "gl_Position=vec4(q.x/1.77778,q.y,q.z*.4,1.0);}\n";
    static const char* fs=
        "#ifdef GL_ES\nprecision mediump float;\n#endif\n"
        "varying float v_light;varying vec2 v_uv; uniform sampler2D u_texture;\n"
        "void main(){vec4 c=texture2D(u_texture,v_uv);if(c.a<.5)discard;\n"
        "gl_FragColor=vec4(c.rgb*v_light,1.0);}\n";
    GLuint vert=compile(GL_VERTEX_SHADER,vs),frag=compile(GL_FRAGMENT_SHADER,fs);
    if(!vert || !frag){if(vert)glDeleteShader(vert);if(frag)glDeleteShader(frag);
        for(int i=0;i<16;i++)free(sheets[i]);Pc_PreviewMesh_Free(&mesh);return 0;}
    s_scene.program=glCreateProgram();glAttachShader(s_scene.program,vert);glAttachShader(s_scene.program,frag);
    glBindAttribLocation(s_scene.program,0,"a_position");
    glBindAttribLocation(s_scene.program,1,"a_normal");
    glBindAttribLocation(s_scene.program,2,"a_uv");glLinkProgram(s_scene.program);
    glDeleteShader(vert);glDeleteShader(frag);GLint linked;
    glGetProgramiv(s_scene.program,GL_LINK_STATUS,&linked);
    if(!linked){for(int i=0;i<16;i++)free(sheets[i]);Pc_PreviewMesh_Free(&mesh);return 0;}
    float* vertices=malloc(mesh.vertexCount*8*sizeof(float));
    unsigned char* atlas=calloc(1024*1024,4);
    int palettes[16],materialsUsed[16],paletteCount=0,valid=vertices && atlas;
    for(size_t v=0;valid && v<mesh.vertexCount;v++) {
        const float* tc=mesh.textureCoordinates+v*4;
        int mat=(int)tc[2],clut=(int)tc[3],tile;
        for(tile=0;tile<paletteCount;tile++)if(palettes[tile]==clut && materialsUsed[tile]==mat)break;
        if(tile==paletteCount) {
            if(tile>=16 || mat<0 || mat>=(int)materialCount || !sheets[mat]){valid=0;break;}
            int tw,th;unsigned char* pixels=Pc_PreviewTim_Decode(sheets[mat],sheetSizes[mat],clut,&tw,&th);
            if(!pixels || tw>256 || th>256){free(pixels);valid=0;break;}
            for(int y=0;y<th;y++)memcpy(atlas+(((tile/4)*256+y)*1024+(tile%4)*256)*4,
                pixels+y*tw*4,(size_t)tw*4);
            free(pixels);palettes[tile]=clut;materialsUsed[tile]=mat;paletteCount++;
        }
        vertices[v*8+6]=((tile%4)*256+tc[0]+.5f)/1024.f;
        vertices[v*8+7]=((tile/4)*256+tc[1]+.5f)/1024.f;
    }
    for(int i=0;i<16;i++)free(sheets[i]);
    if(!valid){free(vertices);free(atlas);Pc_PreviewMesh_Free(&mesh);
        SDL_Log("[PREVIEW-SCENE] material palette decode failed");return 0;}
    glGenTextures(1,&s_scene.texture);glBindTexture(GL_TEXTURE_2D,s_scene.texture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1024,1024,0,GL_RGBA,GL_UNSIGNED_BYTE,atlas);free(atlas);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    for(size_t t=0;t<mesh.vertexCount;t+=3) {
        float a[3],b[3],n[3];
        for(int k=0;k<3;k++) {a[k]=mesh.positions[(t+1)*3+k]-mesh.positions[t*3+k];
            b[k]=mesh.positions[(t+2)*3+k]-mesh.positions[t*3+k];}
        n[0]=a[1]*b[2]-a[2]*b[1];n[1]=a[2]*b[0]-a[0]*b[2];n[2]=a[0]*b[1]-a[1]*b[0];
        float length=sqrtf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for(int j=0;j<3;j++)for(int k=0;k<3;k++) {
            vertices[(t+j)*8+k]=mesh.positions[(t+j)*3+k];
            vertices[(t+j)*8+k+3]=length>0?n[k]/length:0;
        }
    }
    glGenVertexArrays(1,&s_scene.vao);glBindVertexArray(s_scene.vao);
    glGenBuffers(1,&s_scene.buffer);glBindBuffer(GL_ARRAY_BUFFER,s_scene.buffer);
    glBufferData(GL_ARRAY_BUFFER,mesh.vertexCount*8*sizeof(float),vertices,GL_STATIC_DRAW);free(vertices);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,8*sizeof(float),NULL);
    glEnableVertexAttribArray(1);glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(3*sizeof(float)));
    glEnableVertexAttribArray(2);glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(6*sizeof(float)));
    s_scene.vertices=(GLsizei)mesh.vertexCount;
    float largest=0;
    for(int i=0;i<3;i++){
        s_scene.centre[i]=(mesh.minimum[i]+mesh.maximum[i])*.5f;
        float length=mesh.maximum[i]-mesh.minimum[i];if(length>largest)largest=length;
    }
    s_scene.scale=largest>0?2.f/largest:1;
    Pc_PreviewMesh_Free(&mesh);
    if(!Pc_PreviewTarget_Create(&s_scene.target,480,270))return 0;
    SDL_Log("[PREVIEW-SCENE] owned SC0000 static model: %d triangles, target 480x270",(int)s_scene.vertices/3);
    return 1;
}

unsigned Pc_PreviewScene_Diagnostic(void)
{
    if(s_scene.rendered)return s_scene.target.color;
    if(s_scene.attempted)return 0;
    s_scene.attempted=1;
    GLint program,vao,buffer,depthFunc,unit,texture,unpackBuffer,unpack,unpackRow,unpackRows,unpackPixels;
    GLfloat clear[4],clearDepth;
    GLboolean depth,blend,cull,mask[4],depthMask;
    glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&buffer);glGetIntegerv(GL_DEPTH_FUNC,&depthFunc);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);glGetFloatv(GL_DEPTH_CLEAR_VALUE,&clearDepth);
    glGetBooleanv(GL_COLOR_WRITEMASK,mask);glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask);
    depth=glIsEnabled(GL_DEPTH_TEST);blend=glIsEnabled(GL_BLEND);cull=glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&unit);glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpackBuffer);
    glGetIntegerv(GL_UNPACK_ALIGNMENT,&unpack);glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpackRow);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS,&unpackRows);glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpackPixels);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    int ok=load();
    if(ok && Pc_PreviewTarget_Begin(&s_scene.target)) {
        glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glClearColor(.012f,.018f,.015f,1);
#if defined(__ANDROID__) || defined(SH_IOS)
        glClearDepthf(1);
#else
        glClearDepth(1);
#endif
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glUseProgram(s_scene.program);glBindVertexArray(s_scene.vao);
        glUniform3fv(glGetUniformLocation(s_scene.program,"u_centre"),1,s_scene.centre);
        glUniform1f(glGetUniformLocation(s_scene.program,"u_scale"),s_scene.scale);
        glBindTexture(GL_TEXTURE_2D,s_scene.texture);
        glUniform1i(glGetUniformLocation(s_scene.program,"u_texture"),0);
        glDrawArrays(GL_TRIANGLES,0,s_scene.vertices);
        Pc_PreviewTarget_End(&s_scene.target);s_scene.rendered=1;
    }
    glUseProgram(program);glBindVertexArray(vao);glBindBuffer(GL_ARRAY_BUFFER,buffer);
    glBindTexture(GL_TEXTURE_2D,texture);glActiveTexture(unit);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER,unpackBuffer);glPixelStorei(GL_UNPACK_ALIGNMENT,unpack);
    glPixelStorei(GL_UNPACK_ROW_LENGTH,unpackRow);glPixelStorei(GL_UNPACK_SKIP_ROWS,unpackRows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS,unpackPixels);
    glDepthFunc(depthFunc);glDepthMask(depthMask);glColorMask(mask[0],mask[1],mask[2],mask[3]);
    glClearColor(clear[0],clear[1],clear[2],clear[3]);
#if defined(__ANDROID__) || defined(SH_IOS)
    glClearDepthf(clearDepth);
#else
    glClearDepth(clearDepth);
#endif
    if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(cull)glEnable(GL_CULL_FACE);else glDisable(GL_CULL_FACE);
    if(!s_scene.rendered){Pc_PreviewScene_Destroy();s_scene.attempted=1;return 0;}
    return s_scene.target.color;
}
