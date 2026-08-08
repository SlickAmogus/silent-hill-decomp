#include <assert.h>

#include "pc_modern_vertex.h"

typedef struct FlushLog
{
    PcVertexStream stream[4];
    size_t count[4];
    int used;
} FlushLog;

static void RecordFlush(PcVertexStream stream, size_t count, void* userData)
{
    FlushLog* log = (FlushLog*)userData;
    assert(log->used < 4);
    log->stream[log->used] = stream;
    log->count[log->used] = count;
    log->used++;
}

int main(void)
{
    PcVertexBatchDiscriminator batch;
    FlushLog log = {0};

    Pc_VertexBatch_Reset(&batch);
    Pc_VertexBatch_Add(&batch, PC_VERTEX_STREAM_LEGACY, 6, RecordFlush, &log);
    Pc_VertexBatch_Add(&batch, PC_VERTEX_STREAM_LEGACY, 3, RecordFlush, &log);
    assert(log.used == 0);
    assert(batch.stream == PC_VERTEX_STREAM_LEGACY && batch.vertexCount == 9);

    Pc_VertexBatch_Add(&batch, PC_VERTEX_STREAM_MODERN, 12, RecordFlush, &log);
    assert(log.used == 1);
    assert(log.stream[0] == PC_VERTEX_STREAM_LEGACY && log.count[0] == 9);
    assert(batch.stream == PC_VERTEX_STREAM_MODERN && batch.vertexCount == 12);

    Pc_VertexBatch_Add(&batch, PC_VERTEX_STREAM_LEGACY, 3, RecordFlush, &log);
    assert(log.used == 2);
    assert(log.stream[1] == PC_VERTEX_STREAM_MODERN && log.count[1] == 12);

    Pc_VertexBatch_Flush(&batch, RecordFlush, &log);
    assert(log.used == 3);
    assert(log.stream[2] == PC_VERTEX_STREAM_LEGACY && log.count[2] == 3);
    assert(batch.stream == PC_VERTEX_STREAM_NONE && batch.vertexCount == 0);
    return 0;
}
