#ifndef static_assert
#define static_assert _Static_assert
#endif
#include "PsyX/PsyX_render.h"
#include "pc_modern_depth.h"
#include "pc_modern_vertex.h"

#define PC_MODERN_ATTRIBUTE_COUNT 8
_Static_assert(PC_MODERN_ATTRIBUTE_COUNT <= 8, "modern layout exceeds GLES2 attributes");

static GLuint s_modernVertexArray;
static GLuint s_modernVertexBuffer;

void Pc_ModernVertex_Upload(const struct GrModernVertex* vertices, int count)
{
    static const GLint sizes[PC_MODERN_ATTRIBUTE_COUNT] = {4, 1, 2, 4, 4, 3, 3, 3};
    static const GLenum types[PC_MODERN_ATTRIBUTE_COUNT] = {GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_BYTE, GL_FLOAT, GL_FLOAT, GL_FLOAT};
    static const size_t offsets[PC_MODERN_ATTRIBUTE_COUNT] = {
        offsetof(GrModernVertex, x), offsetof(GrModernVertex, z),
        offsetof(GrModernVertex, u), offsetof(GrModernVertex, r),
        offsetof(GrModernVertex, tcx), offsetof(GrModernVertex, ppx),
        offsetof(GrModernVertex, nx), offsetof(GrModernVertex, vsx)};
    int i;

    if (vertices == NULL || count < 0 || count >= MAX_VERTEX_BUFFER_SIZE)
        return;
    if (s_modernVertexArray == 0)
    {
        glGenBuffers(1, &s_modernVertexBuffer);
        glGenVertexArrays(1, &s_modernVertexArray);
        glBindVertexArray(s_modernVertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, s_modernVertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, sizeof(GrModernVertex) * MAX_VERTEX_BUFFER_SIZE, NULL, GL_DYNAMIC_DRAW);
        for (i = 0; i < PC_MODERN_ATTRIBUTE_COUNT; i++)
        {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], types[i], i == a_color ? GL_TRUE : GL_FALSE,
                                  sizeof(GrModernVertex), (const void*)offsets[i]);
        }
    }
    else
    {
        glBindVertexArray(s_modernVertexArray);
        glBindBuffer(GL_ARRAY_BUFFER, s_modernVertexBuffer);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(*vertices), vertices);
}

int Pc_ModernVertex_UploadExact(struct GrModernVertex* vertices,
                                const unsigned short* sz,
                                int count)
{
    if (count <= 0 || !Pc_ModernDepth_Apply(vertices, sz, (size_t)count))
        return 0;
    Pc_ModernVertex_Upload(vertices, count);
    return 1;
}

static void ModernVertex_Flush(PcVertexBatchDiscriminator* batch,
                               PcVertexBatchFlush flush,
                               void* userData)
{
    if (batch == NULL)
        return;
    if (batch->stream != PC_VERTEX_STREAM_NONE && batch->vertexCount != 0 && flush != NULL)
        flush(batch->stream, batch->vertexCount, userData);
    batch->stream = PC_VERTEX_STREAM_NONE;
    batch->vertexCount = 0;
}

void Pc_VertexBatch_Reset(PcVertexBatchDiscriminator* batch)
{
    if (batch == NULL)
        return;
    batch->stream = PC_VERTEX_STREAM_NONE;
    batch->vertexCount = 0;
}

void Pc_VertexBatch_Select(PcVertexBatchDiscriminator* batch,
                           PcVertexStream stream,
                           PcVertexBatchFlush flush,
                           void* userData)
{
    if (batch == NULL || stream == PC_VERTEX_STREAM_NONE)
        return;
    if (batch->stream != PC_VERTEX_STREAM_NONE && batch->stream != stream)
        ModernVertex_Flush(batch, flush, userData);
    batch->stream = stream;
}

void Pc_VertexBatch_Add(PcVertexBatchDiscriminator* batch,
                        PcVertexStream stream,
                        size_t vertexCount,
                        PcVertexBatchFlush flush,
                        void* userData)
{
    if (batch == NULL || stream == PC_VERTEX_STREAM_NONE || vertexCount == 0)
        return;
    Pc_VertexBatch_Select(batch, stream, flush, userData);
    batch->vertexCount += vertexCount;
}

void Pc_VertexBatch_Flush(PcVertexBatchDiscriminator* batch,
                          PcVertexBatchFlush flush,
                          void* userData)
{
    ModernVertex_Flush(batch, flush, userData);
}
