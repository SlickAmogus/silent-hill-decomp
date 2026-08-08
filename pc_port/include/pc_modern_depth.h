#ifndef PC_MODERN_DEPTH_H
#define PC_MODERN_DEPTH_H

#include <stddef.h>

struct GrModernVertex;

#ifdef __cplusplus
extern "C" {
#endif

/* Write stock-equivalent exact item depth into the modern vertex stream.
 * The emitter supplies one raw GTE SZ value per output vertex. */
int Pc_ModernDepth_Apply(struct GrModernVertex* vertices,
                         const unsigned short* sz,
                         size_t count);

#ifdef __cplusplus
}
#endif

#endif /* PC_MODERN_DEPTH_H */
