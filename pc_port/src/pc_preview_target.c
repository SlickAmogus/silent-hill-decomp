/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc_preview_target.h"
#include <PsyX/common/glad.h>
#include <string.h>

void Pc_PreviewTarget_Destroy(PcPreviewTarget* t)
{
    if (!t || t->active) return;
    if (t->depth) glDeleteRenderbuffers(1, &t->depth);
    if (t->framebuffer) glDeleteFramebuffers(1, &t->framebuffer);
    if (t->color) glDeleteTextures(1, &t->color);
    memset(t, 0, sizeof(*t));
}

int Pc_PreviewTarget_Create(PcPreviewTarget* t, int width, int height)
{
    GLint draw, read, texture, renderbuffer, unpackBuffer, maxTexture, maxRenderbuffer;
    PcPreviewTarget next = {0};
    if (!t || t->active || t->framebuffer || t->color || t->depth ||
        width <= 0 || height <= 0) return 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &maxRenderbuffer);
    if (width > maxTexture || height > maxTexture ||
        width > maxRenderbuffer || height > maxRenderbuffer) return 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glGenTextures(1, &next.color);
    glBindTexture(GL_TEXTURE_2D, next.color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenRenderbuffers(1, &next.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, next.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    glGenFramebuffers(1, &next.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, next.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, next.depth);
    int ready = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    if (!ready) { Pc_PreviewTarget_Destroy(&next); return 0; }
    /* Replacing a currently bound old attachment would invalidate restored GL
     * bindings. Keep replacement explicit: destroy only after ending its use. */
    next.width = width; next.height = height;
    *t = next;
    return 1;
}

int Pc_PreviewTarget_Begin(PcPreviewTarget* t)
{
    if (!t || !t->framebuffer || t->active) return 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &t->drawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &t->readFramebuffer);
    glGetIntegerv(GL_VIEWPORT, t->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, t->scissor);
    t->scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glViewport(0, 0, t->width, t->height);
    glDisable(GL_SCISSOR_TEST);
    t->active = 1;
    return 1;
}

void Pc_PreviewTarget_End(PcPreviewTarget* t)
{
    if (!t || !t->active) return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->drawFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->readFramebuffer);
    glViewport(t->viewport[0], t->viewport[1], t->viewport[2], t->viewport[3]);
    glScissor(t->scissor[0], t->scissor[1], t->scissor[2], t->scissor[3]);
    if (t->scissorEnabled) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    t->active = 0;
}

int Pc_PreviewTarget_SelfTest(void)
{
    PcPreviewTarget t = {0};
    GLfloat clear[4]; GLboolean mask[4];
    GLint draw, read, viewport[4], restored[4], scissor[4], restoredScissor[4];
    GLint packBuffer, pack, rowLength, skipRows, skipPixels;
    GLboolean scissorEnabled=glIsEnabled(GL_SCISSOR_TEST);
    unsigned char pixel[4] = {0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    if (!Pc_PreviewTarget_Create(&t, 480, 270)) return 0;
    if (!Pc_PreviewTarget_Begin(&t)) { Pc_PreviewTarget_Destroy(&t); return 0; }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(.25f, .5f, .75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(240, 135, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipRows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    glClearColor(clear[0], clear[1], clear[2], clear[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    Pc_PreviewTarget_End(&t);
    GLint afterDraw, afterRead;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &afterDraw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &afterRead);
    glGetIntegerv(GL_VIEWPORT, restored);
    glGetIntegerv(GL_SCISSOR_BOX, restoredScissor);
    int pass = pixel[0] >= 63 && pixel[0] <= 65 && pixel[1] >= 127 && pixel[1] <= 129 &&
        pixel[2] >= 190 && pixel[2] <= 192 && pixel[3] == 255 &&
        draw == afterDraw && read == afterRead && !memcmp(viewport, restored, sizeof(viewport)) &&
        !memcmp(scissor, restoredScissor, sizeof(scissor)) && scissorEnabled==glIsEnabled(GL_SCISSOR_TEST);
    Pc_PreviewTarget_Destroy(&t);
    return pass;
}
