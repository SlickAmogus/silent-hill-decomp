#ifndef PC_MODERN_MESH_H
#define PC_MODERN_MESH_H

#include <stddef.h>

#include "common.h"

typedef struct cgltf_data cgltf_data;
typedef struct cgltf_node cgltf_node;

#ifdef __cplusplus
extern "C" {
#endif

#define PC_MODERN_MESH_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define PC_MODERN_MESH_MAX_NODE_DEPTH 64u
#define PC_MODERN_MESH_MAX_VERTICES   0x2000u
#define PC_MODERN_MESH_MAX_TRIANGLES  0x2000u

/* Target world extent for an imported item, in PSX model units.
 * Measured from the stock health-drink model UNQ21.TMD (flags=0, nobj=1,
 * vn=178): max|coord| = 297, per-axis (118, 118, 297). An imported glTF is
 * uniformly scaled so its own world-space max|coord| lands here, which makes
 * it occupy the same on-screen footprint as the retail item it replaces. */
#define PC_MODERN_MESH_STOCK_EXTENT   297.0

/* Pre-normalization fallback. Used ONLY when a GLB's POSITION bounds are
 * missing or degenerate, so an asset that cannot be measured keeps exactly the
 * behaviour it had before per-asset normalization existed. */
#define PC_MODERN_MESH_LEGACY_SCALE   100.0

/* Sanity window for a derived scale. Outside this the derivation is treated as
 * untrustworthy and the legacy constant is used instead. */
#define PC_MODERN_MESH_MIN_SCALE      1.0e-4
#define PC_MODERN_MESH_MAX_SCALE      1.0e+7

typedef struct PcModernMesh
{
    s32               fileIdx;
    const cgltf_data*  gltf;
    const cgltf_node*  meshNode;
    size_t             vertexCount;
    size_t             triangleCount;
    /* Uniform world-space scale applied once per vertex in ModernMesh_Expand,
     * derived at accept time from this asset's own POSITION bounds. Never zero:
     * a failed derivation stores PC_MODERN_MESH_LEGACY_SCALE. */
    double             normalizeScale;
    /* Measured world-space max|coord| before normalization; 0.0 when the bounds
     * could not be derived. Diagnostic only. */
    double             sourceExtent;
    int                hasEmbeddedTexture;
    const unsigned char* embeddedImageBytes;
    size_t             embeddedImageSize;
    u32                embeddedTextureId;
    int                embeddedTextureWidth;
    int                embeddedTextureHeight;
    u32                loggedTextureSources;
} PcModernMesh;

/* Resolve and validate gamedata/load/<folder>/<retail basename>.glb.
 * Returns NULL unless loose files are enabled, the sibling GLB exists, and it
 * passes the SH1 item profile. A miss also clears the active item so a prior
 * modern item can never stand in for the current stock item. */
const PcModernMesh* Pc_ModernMesh_Find(s32 fileIdx);

/* Current successful Find result, or NULL after any miss/rejection. */
const PcModernMesh* Pc_ModernMesh_Active(void);

/* Unique-item load hook: selects the sibling GLB and returns the original read
 * destination unchanged. Every call clears stale active state on a miss. */
void* Pc_ModernMesh_SelectRead(s32 fileIdx, void* destination);

/* Carousel link seam. Loads/selects the item's GLB and tags the already-linked
 * stock pack object with its modern identity. A miss leaves the stock object
 * untagged and returns zero. */
int Pc_ModernMesh_LinkObject(s32 fileIdx, void* object);

/* Expand the active mesh at the stock item draw seam and enqueue one ordered
 * modern command. Returns zero without modifying the OT on any failure. */
int Pc_ModernMesh_Emit(void* object, void* orderingTable, int shift);

typedef struct PcModernDrawBinding
{
    u32 textureId;
    int texFormat;
    int vertexCount;
    int nativeWidth, nativeHeight;
    int offsetX, offsetY;
    int hiresWidth, hiresHeight;
} PcModernDrawBinding;

/* Renderer preparation seam. Uploads the already-expanded command and returns
 * its inherited retail texture binding/count; zero leaves it inert. */
int Pc_ModernMesh_PrepareDraw(u32 meshHandle, PcModernDrawBinding* binding);

/* Test/tool seam: validate one in-memory GLB without changing the registry.
 * inheritBindingCount is -1 when the retail binding is not yet known; otherwise
 * inherit requires exactly one distinct textured tpage/CLUT pair. */
int Pc_ModernMesh_ValidateMemory(const void* bytes, size_t size,
                                 int inheritBindingCount,
                                 char* error, size_t errorCapacity,
                                 PcModernMesh* result,
                                 cgltf_data** parsedData);

/* Releases a parse returned by Pc_ModernMesh_ValidateMemory. */
void Pc_ModernMesh_FreeParsed(cgltf_data* data);

#ifdef __cplusplus
}
#endif

#endif /* PC_MODERN_MESH_H */
