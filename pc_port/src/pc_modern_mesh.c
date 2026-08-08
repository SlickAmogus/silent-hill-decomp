/* Modern loose ITEM mesh loader and registry.
 *
 * Resolves a retail file-table index to the sibling
 * gamedata/load/<FOLDER>/<NAME>.glb, parses it with vendored cgltf, validates
 * the SH1 inventory-item glTF profile, and retains the parsed document for the
 * later modern draw tasks. This module draws nothing and has no call site yet.
 *
 * ---------------------------------------------------------------------------
 * STOCK-UNCHANGED GUARANTEE
 * ---------------------------------------------------------------------------
 * Pc_ModernMesh_Find returns NULL unless ALL of:
 *   1. g_PcConfig.allowLooseFiles is set                 (default 0),
 *   2. the derived sibling .glb exists and is readable,
 *   3. the GLB passes ModernMesh_Validate.
 * Condition 1 is the first executable test. When false, this module does not
 * allocate, derive a path, touch the filesystem, or log. A missing, unreadable,
 * oversized, malformed, or unsupported file returns NULL; the caller remains
 * on its existing TMD path. This task adds no caller, so even an accepted GLB
 * cannot affect rendering yet.
 *
 * ---------------------------------------------------------------------------
 * STALE-POINTER POLICY
 * ---------------------------------------------------------------------------
 * All unique items share FS_BUFFER_5. Pc_ModernMesh_Find clears s_activeEntry on
 * EVERY call before discovery or validation. Thus selecting a stock/rejected
 * item after a modern item cannot leave the prior item active. Accepted parses
 * are process-lifetime registry objects and are never freed or replaced, so a
 * future draw packet cannot observe freed cgltf pointers. A changed file size
 * is rejected for that process rather than mutating an existing entry.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "main/fileinfo.h"
#include "main/fsqueue.h"

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include "PsyX/PsyX_render.h"
#include "psx/gtereg.h"
#include "psx/libgpu.h"
#include "psyq/libgs.h"
#include "hires_override.h"
#include "pc_config.h"
#include "pc_modern_mesh.h"
#include "pc_modern_shader.h"
#include "pc_modern_vertex.h"
#include "sh_log.h"

#define PC_MODERN_MESH_REGISTRY_MAX 64
#define PC_MODERN_MESH_PATH_MAX     176
#define PC_MODERN_AREA_EPSILON      1.0e-30
#define PC_MODERN_UV_AREA_EPSILON   1.0e-15

/* How far outside 0..1 a UV may stray and still be accepted (then clamped).
 *
 * A hard 0..1 reject threw away whole models over authoring slop: a real Blender
 * export measured V up to 1.0116 on 41 of 6262 vertices (0.65%) -- about three
 * texels past the edge of a 256 page -- and lost the entire mesh for it. The
 * embedded texture is uploaded GL_CLAMP_TO_EDGE, so those samples clamp to the
 * edge texel anyway; rejecting produced a WORSE result (the stock item) than
 * simply honouring what the sampler would already do.
 *
 * It is a tolerance rather than an unconditional clamp because a model authored
 * to TILE (UVs running 0..2 or 0..4) cannot be honoured by a clamped sampler at
 * all, and silently flattening its whole second copy onto the edge row is worse
 * than refusing with a message. 1/32 is ~8 texels of slack at 256 -- generous
 * for unwrap overhang, nowhere near the 2.0 that signals intended tiling. */
#define PC_MODERN_UV_TOLERANCE      0.03125f

static float ModernMesh_ClampUv(float uv)
{
    if (uv < 0.0f) return 0.0f;
    if (uv > 1.0f) return 1.0f;
    return uv;
}

typedef struct PcModernMeshEntry
{
    PcModernMesh mesh;
    cgltf_data*   data;
    long          fileSize;
    /* Per-item expanded draw state.
     *
     * These were twelve module-scope globals. ModernMesh_Expand frees and
     * re-allocates the vertex/sz buffers on every call, and its only caller is
     * Pc_ModernMesh_Emit, which the carousel invokes once per visible item per
     * frame. With shared storage each Emit destroyed the previous item's
     * vertices, so at most ONE imported mesh could survive to the draw --
     * measured as exactly 1 distinct asset per frame across 96/96 frames.
     *
     * Holding the payload on the entry lets every emitted item keep its own
     * expanded geometry, texture binding and footprint until its own
     * PrepareDraw runs. */
    GrModernVertex* drawVertices;
    unsigned short* drawSz;
    int             drawCount;
    u32             drawTexture;
    int             drawFormat;
    int             drawNativeWidth;
    int             drawNativeHeight;
    int             drawOffsetX;
    int             drawOffsetY;
    int             drawHiresWidth;
    int             drawHiresHeight;
} PcModernMeshEntry;

static PcModernMeshEntry s_registry[PC_MODERN_MESH_REGISTRY_MAX];
static int s_registryCount;
/* Most recent successful Find. Selection state only -- it no longer owns any
 * draw payload, so a later Find cannot invalidate an earlier item's geometry. */
static PcModernMeshEntry* s_activeEntry;

enum
{
    PcModernTextureSource_InstalledOverride = 1u << 0,
    PcModernTextureSource_EmbeddedGlb = 1u << 1,
    PcModernTextureSource_RetailVram = 1u << 2
};

extern FILE* Pc_LooseFOpen(const char* path, const char* mode);

static int ModernMesh_Fail(char* error, size_t capacity, const char* text)
{
    if (error != NULL && capacity != 0)
    {
        snprintf(error, capacity, "%s", text);
    }
    return 0;
}

static int ModernMesh_Finite(cgltf_float value)
{
    return isfinite((double)value) != 0;
}

static int ModernMesh_Path(s32 fileIdx, char* out, size_t outSize)
{
    const s_FileInfo* file = &g_FileTable[fileIdx];
    const char* folder = g_FilePaths[file->pathIdx];
    char strippedFolder[16];
    char name[32];
    char* extension;
    size_t i;
    size_t length;

    if (folder[0] == '\\')
    {
        folder++;
    }
    length = strlen(folder);
    while (length > 0 && folder[length - 1] == '\\')
    {
        length--;
    }
    if (length >= sizeof(strippedFolder))
    {
        length = sizeof(strippedFolder) - 1;
    }
    for (i = 0; i < length; i++)
    {
        strippedFolder[i] = folder[i];
    }
    strippedFolder[length] = '\0';

    Fs_GetFileName(name, fileIdx);
    extension = strrchr(name, '.');
    if (extension == NULL || strcmp(extension, ".TMD") != 0)
    {
        return 0;
    }
    memcpy(extension, ".glb", 5);
    return snprintf(out, outSize, "gamedata/load/%s/%s", strippedFolder, name) < (int)outSize;
}

static long ModernMesh_ProbeSize(const char* path)
{
    long size = -1;
    FILE* file = Pc_LooseFOpen(path, "rb");

    if (file == NULL)
    {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) == 0)
    {
        size = ftell(file);
    }
    fclose(file);
    return size;
}

static void* ModernMesh_Read(const char* path, long size)
{
    void* bytes;
    FILE* file = Pc_LooseFOpen(path, "rb");

    if (file == NULL)
    {
        return NULL;
    }
    bytes = malloc((size_t)size);
    if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size)
    {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    return bytes;
}

static int ModernMesh_NodeReachable(const cgltf_scene* scene, const cgltf_node* wanted,
                                    cgltf_size depth, char* error, size_t errorCapacity)
{
    cgltf_size i;

    if (depth > PC_MODERN_MESH_MAX_NODE_DEPTH)
    {
        ModernMesh_Fail(error, errorCapacity, "node hierarchy exceeds depth 64");
        return -1;
    }
    for (i = 0; i < scene->nodes_count; i++)
    {
        const cgltf_node* node = scene->nodes[i];
        cgltf_size parentDepth = 1;
        const cgltf_node* parent;

        if (node == wanted)
        {
            return 1;
        }
        for (parent = wanted; parent != NULL; parent = parent->parent)
        {
            if (parent == node)
            {
                return 1;
            }
            if (++parentDepth > PC_MODERN_MESH_MAX_NODE_DEPTH)
            {
                ModernMesh_Fail(error, errorCapacity, "node hierarchy exceeds depth 64");
                return -1;
            }
        }
    }
    return 0;
}

static int ModernMesh_CheckNode(const cgltf_node* node, char* error, size_t errorCapacity)
{
    cgltf_float matrix[16];
    double determinant;
    int i;

    if (node->has_matrix && (node->has_translation || node->has_rotation || node->has_scale))
    {
        return ModernMesh_Fail(error, errorCapacity, "node uses both matrix and TRS");
    }
    if (node->skin != NULL)
    {
        /* Skins deform geometry, so we genuinely cannot consume them. Cameras
         * do not: a camera node is scene furniture sitting beside the mesh, and
         * the walk only ever descends to the single reachable MESH node. This
         * check used to reject `node->camera != NULL` too, which threw out the
         * Khronos Duck -- a textbook-clean mesh -- purely because the sample
         * scene ships a camera next to it. */
        return ModernMesh_Fail(error, errorCapacity, "skinned nodes are unsupported");
    }
    cgltf_node_transform_world(node, matrix);
    for (i = 0; i < 16; i++)
    {
        if (!ModernMesh_Finite(matrix[i]))
        {
            return ModernMesh_Fail(error, errorCapacity, "node transform is non-finite");
        }
    }
    determinant = (double)matrix[0] * ((double)matrix[5] * matrix[10] - (double)matrix[6] * matrix[9])
                - (double)matrix[4] * ((double)matrix[1] * matrix[10] - (double)matrix[2] * matrix[9])
                + (double)matrix[8] * ((double)matrix[1] * matrix[6] - (double)matrix[2] * matrix[5]);
    if (!isfinite(determinant) || fabs(determinant) <= 1.0e-20)
    {
        return ModernMesh_Fail(error, errorCapacity, "node transform is singular");
    }
    return 1;
}

static const cgltf_attribute* ModernMesh_Attribute(const cgltf_primitive* primitive,
                                                    cgltf_attribute_type type, int index)
{
    cgltf_size i;

    for (i = 0; i < primitive->attributes_count; i++)
    {
        if (primitive->attributes[i].type == type && primitive->attributes[i].index == index)
        {
            return &primitive->attributes[i];
        }
    }
    return NULL;
}

static int ModernMesh_CheckAccessor(const cgltf_accessor* accessor, cgltf_type type,
                                    cgltf_component_type component, cgltf_size count,
                                    char* error, size_t errorCapacity)
{
    cgltf_size i;
    int k;

    if (accessor == NULL || accessor->type != type || accessor->component_type != component ||
        accessor->count != count || accessor->is_sparse || accessor->buffer_view == NULL)
    {
        return ModernMesh_Fail(error, errorCapacity, "accessor format/count is unsupported");
    }
    for (i = 0; i < accessor->count; i++)
    {
        cgltf_float values[4] = { 0, 0, 0, 0 };
        cgltf_size components = type == cgltf_type_vec2 ? 2 : (type == cgltf_type_vec3 ? 3 : 4);
        if (!cgltf_accessor_read_float(accessor, i, values, components))
        {
            return ModernMesh_Fail(error, errorCapacity, "accessor points outside packed GLB data");
        }
        for (k = 0; k < (int)components; k++)
        {
            if (!ModernMesh_Finite(values[k]))
            {
                return ModernMesh_Fail(error, errorCapacity, "accessor contains NaN or infinity");
            }
        }
    }
    return 1;
}

static int ModernMesh_CheckPrimitive(const cgltf_primitive* primitive,
                                     const cgltf_float matrix[16],
                                     size_t* vertices, size_t* triangles,
                                     char* error, size_t errorCapacity)
{
    const cgltf_attribute* position = ModernMesh_Attribute(primitive, cgltf_attribute_type_position, 0);
    const cgltf_attribute* uv = ModernMesh_Attribute(primitive, cgltf_attribute_type_texcoord, 0);
    cgltf_size positionCount;
    cgltf_size indexCount;
    unsigned char* used;
    size_t usedCount = 0;
    cgltf_size i;

    if (primitive->type != cgltf_primitive_type_triangles)
    {
        return ModernMesh_Fail(error, errorCapacity, "only triangle primitives are supported");
    }
    if (primitive->targets_count != 0)
    {
        return ModernMesh_Fail(error, errorCapacity, "morph targets are unsupported");
    }
    if (position == NULL || uv == NULL || position->data == NULL || uv->data == NULL)
    {
        return ModernMesh_Fail(error, errorCapacity, "POSITION and TEXCOORD_0 are required");
    }
    positionCount = position->data->count;
    if (!ModernMesh_CheckAccessor(position->data, cgltf_type_vec3, cgltf_component_type_r_32f,
                                  positionCount, error, errorCapacity) ||
        !ModernMesh_CheckAccessor(uv->data, cgltf_type_vec2, cgltf_component_type_r_32f,
                                  positionCount, error, errorCapacity))
    {
        return 0;
    }
    for (i = 0; i < primitive->attributes_count; i++)
    {
        const cgltf_attribute* attribute = &primitive->attributes[i];
        if (attribute->type == cgltf_attribute_type_normal)
        {
            if (attribute->index != 0 || !ModernMesh_CheckAccessor(attribute->data, cgltf_type_vec3,
                    cgltf_component_type_r_32f, positionCount, error, errorCapacity))
                return 0;
        }
        else if (attribute->type == cgltf_attribute_type_color)
        {
            const cgltf_accessor* a = attribute->data;
            int formatOk = a != NULL && attribute->index == 0 &&
                (a->type == cgltf_type_vec3 || a->type == cgltf_type_vec4) &&
                (a->component_type == cgltf_component_type_r_32f ||
                 ((a->component_type == cgltf_component_type_r_8u || a->component_type == cgltf_component_type_r_16u) && a->normalized));
            if (!formatOk || a->count != positionCount || a->is_sparse || a->buffer_view == NULL)
                return ModernMesh_Fail(error, errorCapacity, "COLOR_0 format/count is unsupported");
            for (cgltf_size colorIndex = 0; colorIndex < a->count; colorIndex++)
            {
                cgltf_float color[4] = { 0, 0, 0, 1 };
                cgltf_size colorComponents = a->type == cgltf_type_vec3 ? 3 : 4;
                int colorComponent;
                if (!cgltf_accessor_read_float(a, colorIndex, color, colorComponents))
                    return ModernMesh_Fail(error, errorCapacity, "COLOR_0 data is unreadable");
                for (colorComponent = 0; colorComponent < (int)colorComponents; colorComponent++)
                    if (!ModernMesh_Finite(color[colorComponent]))
                        return ModernMesh_Fail(error, errorCapacity, "COLOR_0 contains NaN or infinity");
            }
        }
        else if (attribute->type != cgltf_attribute_type_position &&
                 attribute->type != cgltf_attribute_type_texcoord &&
                 attribute->type != cgltf_attribute_type_tangent)
        {
            /* TANGENT is ignorable: we consume POSITION, TEXCOORD_0, and
             * optionally NORMAL/COLOR_0, and never read tangents. An extra
             * attribute a loader does not consume is not an error -- rejecting
             * on it threw out the Avocado, whose mesh is otherwise a clean fit. */
            return ModernMesh_Fail(error, errorCapacity, "unsupported vertex attribute");
        }
        else if (attribute->index != 0)
        {
            return ModernMesh_Fail(error, errorCapacity, "only attribute set 0 is supported");
        }
    }

    if (primitive->indices != NULL)
    {
        if (primitive->indices->type != cgltf_type_scalar ||
            (primitive->indices->component_type != cgltf_component_type_r_8u &&
             primitive->indices->component_type != cgltf_component_type_r_16u &&
             primitive->indices->component_type != cgltf_component_type_r_32u) ||
            primitive->indices->is_sparse || primitive->indices->buffer_view == NULL)
            return ModernMesh_Fail(error, errorCapacity, "index accessor format is unsupported");
        indexCount = primitive->indices->count;
    }
    else
    {
        indexCount = positionCount;
    }
    if (indexCount == 0 || indexCount % 3 != 0)
    {
        return ModernMesh_Fail(error, errorCapacity, "triangle index count is invalid");
    }
    used = (unsigned char*)calloc(positionCount == 0 ? 1 : positionCount, 1);
    if (used == NULL)
    {
        return ModernMesh_Fail(error, errorCapacity, "out of memory validating vertices");
    }
    for (i = 0; i < indexCount; i++)
    {
        cgltf_size index = primitive->indices != NULL ? cgltf_accessor_read_index(primitive->indices, i) : i;
        if (index >= positionCount || index > 8191)
        {
            free(used);
            return ModernMesh_Fail(error, errorCapacity, "triangle index is out of range");
        }
        if (!used[index])
        {
            used[index] = 1;
            usedCount++;
        }
    }
    free(used);

    for (i = 0; i < indexCount; i += 3)
    {
        cgltf_size indices[3];
        cgltf_float p[3][3];
        cgltf_float t[3][2];
        double ab[3], ac[3], cross[3], area2;
        int corner;
        int axis;

        for (corner = 0; corner < 3; corner++)
        {
            cgltf_float raw[3];
            indices[corner] = primitive->indices != NULL ? cgltf_accessor_read_index(primitive->indices, i + corner) : i + corner;
            if (!cgltf_accessor_read_float(position->data, indices[corner], raw, 3) ||
                !cgltf_accessor_read_float(uv->data, indices[corner], t[corner], 2))
                return ModernMesh_Fail(error, errorCapacity, "vertex data is unreadable");
            p[corner][0] = matrix[0] * raw[0] + matrix[4] * raw[1] + matrix[8] * raw[2] + matrix[12];
            p[corner][1] = matrix[1] * raw[0] + matrix[5] * raw[1] + matrix[9] * raw[2] + matrix[13];
            p[corner][2] = matrix[2] * raw[0] + matrix[6] * raw[1] + matrix[10] * raw[2] + matrix[14];
            if (t[corner][0] < -PC_MODERN_UV_TOLERANCE || t[corner][0] > 1.0f + PC_MODERN_UV_TOLERANCE ||
                t[corner][1] < -PC_MODERN_UV_TOLERANCE || t[corner][1] > 1.0f + PC_MODERN_UV_TOLERANCE)
                return ModernMesh_Fail(error, errorCapacity,
                                       "UV is outside 0..1 by more than the authoring tolerance "
                                       "(tiled/wrapped UVs cannot be drawn by a clamped sampler)");
            /* Within tolerance the UV is clamped, not rejected — matching what
             * GL_CLAMP_TO_EDGE does at sample time. Clamp the local copy too so
             * the UV-area test below sees the geometry that will actually draw. */
            t[corner][0] = ModernMesh_ClampUv(t[corner][0]);
            t[corner][1] = ModernMesh_ClampUv(t[corner][1]);
        }
        for (axis = 0; axis < 3; axis++)
        {
            ab[axis] = (double)p[1][axis] - p[0][axis];
            ac[axis] = (double)p[2][axis] - p[0][axis];
        }
        cross[0] = ab[1] * ac[2] - ab[2] * ac[1];
        cross[1] = ab[2] * ac[0] - ab[0] * ac[2];
        cross[2] = ab[0] * ac[1] - ab[1] * ac[0];
        if (cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] <= PC_MODERN_AREA_EPSILON)
            return ModernMesh_Fail(error, errorCapacity, "triangle has zero geometric area");
        area2 = ((double)t[1][0] - t[0][0]) * ((double)t[2][1] - t[0][1])
              - ((double)t[1][1] - t[0][1]) * ((double)t[2][0] - t[0][0]);
        if (fabs(area2) <= PC_MODERN_UV_AREA_EPSILON)
            return ModernMesh_Fail(error, errorCapacity, "triangle has zero UV area");
    }

    *vertices += usedCount;
    *triangles += indexCount / 3;
    if (*vertices > PC_MODERN_MESH_MAX_VERTICES || *triangles > PC_MODERN_MESH_MAX_TRIANGLES)
        return ModernMesh_Fail(error, errorCapacity, "flattened vertex/triangle limit exceeded");
    return 1;
}

/* Resolve the ONE texture we actually consume: the base-colour map of the
 * single primitive's material. Everything else a PBR asset ships -- normal,
 * metallic-roughness, occlusion, emissive -- is never read by this renderer,
 * so its presence must not be an error. */
static const cgltf_texture* ModernMesh_BaseColorTexture(const cgltf_data* data)
{
    cgltf_size meshIndex;

    for (meshIndex = 0; meshIndex < data->meshes_count; meshIndex++)
    {
        cgltf_size primitiveIndex;
        for (primitiveIndex = 0; primitiveIndex < data->meshes[meshIndex].primitives_count; primitiveIndex++)
        {
            const cgltf_material* material =
                data->meshes[meshIndex].primitives[primitiveIndex].material;
            if (material != NULL && material->has_pbr_metallic_roughness &&
                material->pbr_metallic_roughness.base_color_texture.texture != NULL)
            {
                return material->pbr_metallic_roughness.base_color_texture.texture;
            }
        }
    }
    return NULL;
}

static int ModernMesh_EmbeddedImage(cgltf_data* data,
                                    const unsigned char** bytes, size_t* size,
                                    char* error, size_t errorCapacity)
{
    const cgltf_texture* texture = NULL;
    cgltf_size meshIndex;

    *bytes = NULL;
    *size = 0;
    if (data->images_count == 0 && data->textures_count == 0)
        return 1;
    /* We consume exactly ONE texture: the base-colour map of the single
     * primitive's material. A PBR asset routinely ships extra images
     * (normal / metallic-roughness / occlusion) that we never read. Requiring
     * images_count == 1 rejected the Avocado for carrying maps we were always
     * going to ignore. Bind the base-colour texture explicitly instead of
     * assuming index 0, and let unused images be unused. */
    texture = ModernMesh_BaseColorTexture(data);
    if (texture == NULL)
        return ModernMesh_Fail(error, errorCapacity, "a base-colour texture is required");
    if (texture->image == NULL || texture->image->buffer_view == NULL ||
        texture->image->uri != NULL || texture->image->mime_type == NULL ||
        strcmp(texture->image->mime_type, "image/png") != 0)
        return ModernMesh_Fail(error, errorCapacity, "embedded texture must be one bufferView PNG image");
    for (meshIndex = 0; meshIndex < data->meshes_count; meshIndex++)
    {
        cgltf_size primitiveIndex;
        for (primitiveIndex = 0; primitiveIndex < data->meshes[meshIndex].primitives_count; primitiveIndex++)
        {
            const cgltf_material* material = data->meshes[meshIndex].primitives[primitiveIndex].material;
            if (material == NULL || !material->has_pbr_metallic_roughness ||
                material->pbr_metallic_roughness.base_color_texture.texture != texture ||
                material->pbr_metallic_roughness.base_color_texture.texcoord != 0)
                return ModernMesh_Fail(error, errorCapacity, "every primitive must use the embedded base-color texture on TEXCOORD_0");
        }
    }
    *bytes = cgltf_buffer_view_data(texture->image->buffer_view);
    *size = texture->image->buffer_view->size;
    if (*bytes == NULL || *size == 0 || *size > PC_MODERN_MESH_MAX_FILE_BYTES)
        return ModernMesh_Fail(error, errorCapacity, "embedded PNG bufferView is empty or oversized");
    return 1;
}

/* Derive the per-asset uniform normalization scale.
 *
 * Prior behaviour was a blind `* 100.0` applied per vertex inside the transform
 * loop. That is not a normalization: it is a constant that happened to suit one
 * hand-prepared asset. Measured against the stock extent of 297, the two proof
 * assets miss in OPPOSITE directions under the constant (Duck world extent
 * 2.2100 -> 221.0 = 0.74x; Avocado world extent 6.0554 -> 605.5 = 2.04x), so no
 * single constant can serve both.
 *
 * The scale is computed from this asset's own POSITION accessor min/max. All
 * eight bounding-box corners are pushed through the node world matrix, because
 * a rotated or non-uniformly scaled node makes any cheaper corner choice
 * underestimate the true world extent.
 *
 * FAIL-CLOSED: if any accessor lacks bounds, or the measured extent is
 * non-finite or degenerate, or the derived scale falls outside a sanity window,
 * the mesh keeps PC_MODERN_MESH_LEGACY_SCALE and renders exactly as it did
 * before this function existed. A missing measurement must not draw garbage. */
static void ModernMesh_DeriveNormalization(const cgltf_mesh* mesh,
                                           const cgltf_float world[16],
                                           PcModernMesh* result)
{
    double extent = 0.0;
    int measured = 0;
    cgltf_size primitiveIndex;

    result->normalizeScale = PC_MODERN_MESH_LEGACY_SCALE;
    result->sourceExtent = 0.0;
    if (mesh == NULL || world == NULL)
        return;

    for (primitiveIndex = 0; primitiveIndex < mesh->primitives_count; primitiveIndex++)
    {
        const cgltf_primitive* primitive = &mesh->primitives[primitiveIndex];
        const cgltf_attribute* position = ModernMesh_Attribute(primitive, cgltf_attribute_type_position, 0);
        const cgltf_accessor* accessor;
        int corner;

        if (position == NULL || position->data == NULL)
            return;
        accessor = position->data;
        if (!accessor->has_min || !accessor->has_max || accessor->type != cgltf_type_vec3)
            return;
        for (corner = 0; corner < 3; corner++)
            if (!ModernMesh_Finite(accessor->min[corner]) || !ModernMesh_Finite(accessor->max[corner]) ||
                accessor->min[corner] > accessor->max[corner])
                return;

        for (corner = 0; corner < 8; corner++)
        {
            double local[3];
            double transformed[3];
            int axis;

            for (axis = 0; axis < 3; axis++)
                local[axis] = (corner & (1 << axis)) ? (double)accessor->max[axis] : (double)accessor->min[axis];
            transformed[0] = world[0] * local[0] + world[4] * local[1] + world[8] * local[2] + world[12];
            transformed[1] = world[1] * local[0] + world[5] * local[1] + world[9] * local[2] + world[13];
            transformed[2] = world[2] * local[0] + world[6] * local[1] + world[10] * local[2] + world[14];
            for (axis = 0; axis < 3; axis++)
            {
                double magnitude;
                if (!isfinite(transformed[axis]))
                    return;
                magnitude = fabs(transformed[axis]);
                if (magnitude > extent)
                    extent = magnitude;
            }
        }
        measured = 1;
    }

    if (!measured || !isfinite(extent) || extent <= 0.0)
        return;
    {
        const double scale = PC_MODERN_MESH_STOCK_EXTENT / extent;
        if (!isfinite(scale) || scale < PC_MODERN_MESH_MIN_SCALE || scale > PC_MODERN_MESH_MAX_SCALE)
            return;
        result->normalizeScale = scale;
        result->sourceExtent = extent;
    }
}

static int ModernMesh_Validate(cgltf_data* data, int inheritBindingCount,
                               char* error, size_t errorCapacity, PcModernMesh* result)
{
    const cgltf_node* meshNode = NULL;
    cgltf_float matrix[16];
    size_t vertices = 0;
    size_t triangles = 0;
    cgltf_size i;
    const unsigned char* embeddedImageBytes;
    size_t embeddedImageSize;

    if (data->file_type != cgltf_file_type_glb || data->asset.version == NULL || strcmp(data->asset.version, "2.0") != 0)
        return ModernMesh_Fail(error, errorCapacity, "export as a glTF 2.0 Binary (.glb)");
    /* glTF 2.0 spec: extensionsUsed is INFORMATIONAL. A conforming loader may
     * ignore an entry it does not understand; only extensionsRequired is
     * binding. We flatten materials to a single base-colour texture anyway, so
     * a model that merely MENTIONS KHR_materials_ior in a material we are about
     * to discard must not be thrown out. Rejecting on extensionsUsed was a spec
     * violation and was the single largest cause of the 0/118 community
     * acceptance rate (69 of 118 rejects). */
    if (data->extensions_required_count)
        return ModernMesh_Fail(error, errorCapacity, "glTF required extensions are unsupported");
    if (data->buffers_count != 1 || data->buffers[0].uri != NULL || data->buffers[0].data == NULL)
        return ModernMesh_Fail(error, errorCapacity, "external/data-URI buffers are unsupported");
    for (i = 0; i < data->images_count; i++)
        if (data->images[i].uri != NULL)
            return ModernMesh_Fail(error, errorCapacity, "external/data-URI images are unsupported");
    if (data->scenes_count != 1 || data->scene == NULL || data->scene != &data->scenes[0])
        return ModernMesh_Fail(error, errorCapacity, "exactly one selected scene is required");
    /* Cameras are scene furniture, not geometry. ModernMesh_Walk only descends
     * to the single reachable MESH node; a camera node is skipped like any
     * other non-mesh node. Rejecting on cameras_count threw out the Khronos
     * Duck -- whose mesh is a textbook fit (1 mesh, POSITION/NORMAL/TEXCOORD_0,
     * triangles, embedded buffer + image, zero extensions) -- purely because
     * the sample scene ships a camera beside it. */
    if (data->meshes_count != 1 || data->animations_count || data->skins_count)
        return ModernMesh_Fail(error, errorCapacity, "one rigid, non-animated mesh is required");
    for (i = 0; i < data->accessors_count; i++)
    {
        int k;
        if (data->accessors[i].is_sparse)
            return ModernMesh_Fail(error, errorCapacity, "sparse accessors are unsupported");
        for (k = 0; k < 16; k++)
            if ((data->accessors[i].has_min && !ModernMesh_Finite(data->accessors[i].min[k])) ||
                (data->accessors[i].has_max && !ModernMesh_Finite(data->accessors[i].max[k])))
                return ModernMesh_Fail(error, errorCapacity, "accessor bounds contain NaN or infinity");
    }
    for (i = 0; i < data->nodes_count; i++)
    {
        int reachable = ModernMesh_NodeReachable(data->scene, &data->nodes[i], 1, error, errorCapacity);
        if (reachable < 0)
            return 0;
        if (reachable && !ModernMesh_CheckNode(&data->nodes[i], error, errorCapacity))
            return 0;
        if (reachable && data->nodes[i].mesh != NULL)
        {
            if (meshNode != NULL || data->nodes[i].mesh != &data->meshes[0])
                return ModernMesh_Fail(error, errorCapacity, "exactly one reachable mesh node is required");
            meshNode = &data->nodes[i];
        }
    }
    if (meshNode == NULL)
        return ModernMesh_Fail(error, errorCapacity, "selected scene contains no mesh node");
    if (data->meshes[0].weights_count != 0)
        return ModernMesh_Fail(error, errorCapacity, "morph weights are unsupported");
    for (i = 0; i < data->materials_count; i++)
    {
        const cgltf_material* material = &data->materials[i];
        if (material->alpha_mode != cgltf_alpha_mode_opaque || material->double_sided || material->unlit ||
            material->emissive_factor[0] != 0.0f || material->emissive_factor[1] != 0.0f || material->emissive_factor[2] != 0.0f)
            return ModernMesh_Fail(error, errorCapacity, "material must be opaque, single-sided, and non-emissive");
    }
    if (inheritBindingCount >= 0 && inheritBindingCount != 1)
        return ModernMesh_Fail(error, errorCapacity, "retail texture binding is ambiguous");
    cgltf_node_transform_world(meshNode, matrix);
    for (i = 0; i < data->meshes[0].primitives_count; i++)
        if (!ModernMesh_CheckPrimitive(&data->meshes[0].primitives[i], matrix,
                                       &vertices, &triangles, error, errorCapacity))
            return 0;
    if (vertices == 0 || triangles == 0)
        return ModernMesh_Fail(error, errorCapacity, "mesh is empty");
    if (!ModernMesh_EmbeddedImage(data, &embeddedImageBytes, &embeddedImageSize, error, errorCapacity))
        return 0;

    memset(result, 0, sizeof(*result));
    result->gltf = data;
    result->meshNode = meshNode;
    result->vertexCount = vertices;
    result->triangleCount = triangles;
    result->hasEmbeddedTexture = embeddedImageBytes != NULL;
    result->embeddedImageBytes = embeddedImageBytes;
    result->embeddedImageSize = embeddedImageSize;
    ModernMesh_DeriveNormalization(&data->meshes[0], matrix, result);
    return 1;
}

int Pc_ModernMesh_ValidateMemory(const void* bytes, size_t size, int inheritBindingCount,
                                 char* error, size_t errorCapacity,
                                 PcModernMesh* result, cgltf_data** parsedData)
{
    cgltf_options options;
    cgltf_data* data = NULL;
    void* owned;
    cgltf_result parseResult;

    if (result == NULL || parsedData == NULL || bytes == NULL || size == 0 || size > PC_MODERN_MESH_MAX_FILE_BYTES)
        return ModernMesh_Fail(error, errorCapacity, "GLB size is outside 1..16 MiB");
    memset(&options, 0, sizeof(options));
    owned = malloc(size);
    if (owned == NULL)
        return ModernMesh_Fail(error, errorCapacity, "out of memory reading GLB");
    memcpy(owned, bytes, size);
    parseResult = cgltf_parse(&options, owned, size, &data);
    if (parseResult != cgltf_result_success)
    {
        free(owned);
        return ModernMesh_Fail(error, errorCapacity, "cgltf could not parse GLB");
    }
    data->file_data = owned;
    data->file_size = size;
    if (cgltf_load_buffers(&options, data, NULL) != cgltf_result_success ||
        cgltf_validate(data) != cgltf_result_success ||
        !ModernMesh_Validate(data, inheritBindingCount, error, errorCapacity, result))
    {
        cgltf_free(data);
        return 0;
    }
    *parsedData = data;
    return 1;
}

void Pc_ModernMesh_FreeParsed(cgltf_data* data)
{
    cgltf_free(data);
}

const PcModernMesh* Pc_ModernMesh_Find(s32 fileIdx)
{
    char path[PC_MODERN_MESH_PATH_MAX];
    char error[192];
    long size;
    void* bytes;
    PcModernMesh mesh;
    cgltf_data* data = NULL;
    int i;

    s_activeEntry = NULL;
    if (!g_PcConfig.allowLooseFiles)
        return NULL;
    if (fileIdx < 0 || fileIdx >= FS_FILE_COUNT || !ModernMesh_Path(fileIdx, path, sizeof(path)))
        return NULL;
    size = ModernMesh_ProbeSize(path);
    if (size < 0)
        return NULL;
    if (size == 0 || (unsigned long)size > PC_MODERN_MESH_MAX_FILE_BYTES)
    {
        SH_DBG("[MODERN_MESH] reject %s: size %ld outside 1..%u", path, size, PC_MODERN_MESH_MAX_FILE_BYTES);
        return NULL;
    }
    for (i = 0; i < s_registryCount; i++)
    {
        if (s_registry[i].mesh.fileIdx == fileIdx)
        {
            if (s_registry[i].fileSize == size)
                s_activeEntry = &s_registry[i];
            else
                SH_DBG("[MODERN_MESH] reject changed %s: restart required", path);
            return s_activeEntry != NULL ? &s_activeEntry->mesh : NULL;
        }
    }
    if (s_registryCount >= PC_MODERN_MESH_REGISTRY_MAX)
    {
        SH_DBG("[MODERN_MESH] registry full; native item model for %s", path);
        return NULL;
    }
    bytes = ModernMesh_Read(path, size);
    if (bytes == NULL)
    {
        SH_DBG("[MODERN_MESH] read failed: %s", path);
        return NULL;
    }
    if (!Pc_ModernMesh_ValidateMemory(bytes, (size_t)size, -1, error, sizeof(error), &mesh, &data))
    {
        free(bytes);
        SH_DBG("[MODERN_MESH] reject %s: %s — native item model", path, error);
        return NULL;
    }
    free(bytes);
    mesh.fileIdx = fileIdx;
    s_registry[s_registryCount].mesh = mesh;
    s_registry[s_registryCount].data = data;
    s_registry[s_registryCount].fileSize = size;
    s_activeEntry = &s_registry[s_registryCount];
    s_registryCount++;
    SH_DBG("[MODERN_MESH] ACCEPT %s: %zu vertices, %zu triangles%s", path,
           mesh.vertexCount, mesh.triangleCount, mesh.hasEmbeddedTexture ? ", embedded texture retained" : "");
    SH_DBG("[MODERN_MESH] normalize %s: world extent %.6f -> scale %.4f (target %.1f)%s", path,
           s_activeEntry->mesh.sourceExtent, s_activeEntry->mesh.normalizeScale, PC_MODERN_MESH_STOCK_EXTENT,
           s_activeEntry->mesh.sourceExtent > 0.0 ? "" : " [FALLBACK: bounds unusable, legacy constant]");
    return &s_activeEntry->mesh;
}

const PcModernMesh* Pc_ModernMesh_Active(void)
{
    return s_activeEntry != NULL ? &s_activeEntry->mesh : NULL;
}

void* Pc_ModernMesh_SelectRead(s32 fileIdx, void* destination)
{
    Pc_ModernMesh_Find(fileIdx);
    return destination;
}

int Pc_ModernMesh_LinkObject(s32 fileIdx, void* object)
{
    GsDOBJ2* drawObject = (GsDOBJ2*)object;
    const PcModernMesh* mesh;

    if (drawObject == NULL || drawObject->tmd == NULL)
        return 0;
    mesh = Pc_ModernMesh_Find(fileIdx);
    if (mesh == NULL)
        return 0;
    drawObject->id = (unsigned long)mesh->fileIdx + 1u;
    return 1;
}

static int ModernMesh_RetailBinding(const GsDOBJ2* object, int* tpage, int* clut)
{
    const struct TMD_STRUCT* tmd;
    const unsigned char* primitive;
    int primitiveCount;
    int found = 0;

    if (object == NULL || object->tmd == NULL || tpage == NULL || clut == NULL)
        return 0;
    tmd = (const struct TMD_STRUCT*)object->tmd;
    primitive = (const unsigned char*)tmd->primtop;
    primitiveCount = (int)tmd->primn;
    while (primitive != NULL && primitiveCount-- > 0)
    {
        const int words = primitive[1];
        if (words == 0)
            return 0;
        if ((primitive[3] & 4) != 0)
        {
            const int nextClut = primitive[6] | (primitive[7] << 8);
            const int nextTpage = primitive[10] | (primitive[11] << 8);
            if (found && (nextTpage != *tpage || nextClut != *clut))
                return 0;
            *tpage = nextTpage;
            *clut = nextClut;
            found = 1;
        }
        primitive += (1 + words) * 4;
    }
    return found;
}

static int ModernMesh_EnsureEmbeddedTexture(PcModernMesh* mesh)
{
    unsigned char* rgba = NULL;
    int width = 0;
    int height = 0;

    if (!mesh->hasEmbeddedTexture)
        return 0;
    if (mesh->embeddedTextureId != 0)
        return 1;
    if (mesh->embeddedImageBytes == NULL || mesh->embeddedImageSize == 0 ||
        mesh->embeddedImageSize > 0xffffffffu ||
        HiresOverride_DecodeToRGBA(mesh->embeddedImageBytes, (unsigned int)mesh->embeddedImageSize,
                                   &rgba, &width, &height) != 0)
        return 0;
    mesh->embeddedTextureId = HiresOverride_CreateTextureRGBA(rgba, width, height);
    free(rgba);
    if (mesh->embeddedTextureId == 0)
        return 0;
    mesh->embeddedTextureWidth = width;
    mesh->embeddedTextureHeight = height;
    SH_DBG("[MODERN_MESH] embedded PNG uploaded: %dx%d texture=%u", width, height,
           (unsigned)mesh->embeddedTextureId);
    return 1;
}

static int ModernMesh_Expand(PcModernMeshEntry* entry, int tpage, int clut)
{
    const PcModernMesh* mesh = &entry->mesh;
    const cgltf_mesh* gltfMesh;
    cgltf_float world[16];
    float drawOffsetX = 0.0f, drawOffsetY = 0.0f;
    size_t outputCount = mesh->triangleCount * 3;
    size_t output = 0;
    cgltf_size primitiveIndex;
    /* Per-asset normalization, derived once at accept time in
     * ModernMesh_DeriveNormalization. Read here, never recomputed per vertex.
     * A record predating derivation (or one whose derivation failed closed)
     * carries the legacy constant, so behaviour degrades to the old path
     * rather than to a zero scale. */
    const double normalizeScale = (mesh->normalizeScale > 0.0 && isfinite(mesh->normalizeScale))
                                      ? mesh->normalizeScale
                                      : PC_MODERN_MESH_LEGACY_SCALE;

    if (outputCount == 0 || outputCount >= MAX_VERTEX_BUFFER_SIZE)
        return 0;
    free(entry->drawVertices);
    free(entry->drawSz);
    entry->drawVertices = (GrModernVertex*)calloc(outputCount, sizeof(*entry->drawVertices));
    entry->drawSz = (unsigned short*)calloc(outputCount, sizeof(*entry->drawSz));
    entry->drawCount = 0;
    if (entry->drawVertices == NULL || entry->drawSz == NULL)
        return 0;

    gltfMesh = mesh->meshNode->mesh;
    cgltf_node_transform_world(mesh->meshNode, world);
    PsyX_GetDrawEnvOffset(&drawOffsetX, &drawOffsetY);
    for (primitiveIndex = 0; primitiveIndex < gltfMesh->primitives_count; primitiveIndex++)
    {
        const cgltf_primitive* primitive = &gltfMesh->primitives[primitiveIndex];
        const cgltf_attribute* position = ModernMesh_Attribute(primitive, cgltf_attribute_type_position, 0);
        const cgltf_attribute* uv = ModernMesh_Attribute(primitive, cgltf_attribute_type_texcoord, 0);
        const cgltf_attribute* color = ModernMesh_Attribute(primitive, cgltf_attribute_type_color, 0);
        cgltf_size indexCount = primitive->indices != NULL ? primitive->indices->count : position->data->count;
        cgltf_size indexOffset;

        for (indexOffset = 0; indexOffset < indexCount; indexOffset++)
        {
            cgltf_size index = primitive->indices != NULL ? cgltf_accessor_read_index(primitive->indices, indexOffset) : indexOffset;
            cgltf_float source[3], transformed[3], texcoord[2], rgba[4] = { 1, 1, 1, 1 };
            GrModernVertex* vertex = &entry->drawVertices[output];
            double x, y, z;
            double nx, ny, nz;

            if (!cgltf_accessor_read_float(position->data, index, source, 3) ||
                !cgltf_accessor_read_float(uv->data, index, texcoord, 2) ||
                (color != NULL && !cgltf_accessor_read_float(color->data, index, rgba, color->data->type == cgltf_type_vec3 ? 3 : 4)))
                return 0;
            transformed[0] = world[0] * source[0] + world[4] * source[1] + world[8] * source[2] + world[12];
            transformed[1] = world[1] * source[0] + world[5] * source[1] + world[9] * source[2] + world[13];
            transformed[2] = world[2] * source[0] + world[6] * source[1] + world[10] * source[2] + world[14];
            /* glTF world space -> PSX model space: (x, -z, y), then the
             * per-asset uniform normalization that replaces the blind * 100.0. */
            nx = (double)transformed[0] * normalizeScale;
            ny = -(double)transformed[2] * normalizeScale;
            nz = (double)transformed[1] * normalizeScale;
            x = (C2_R11 * nx + C2_R12 * ny + C2_R13 * nz) / 4096.0 + C2_TRX;
            y = (C2_R21 * nx + C2_R22 * ny + C2_R23 * nz) / 4096.0 + C2_TRY;
            z = (C2_R31 * nx + C2_R32 * ny + C2_R33 * nz) / 4096.0 + C2_TRZ;
            if (z <= 0.0 || z > 65535.0)
                return 0;
            vertex->x = (float)(C2_OFX / 65536.0 + x * C2_H / z) + drawOffsetX;
            vertex->y = (float)(C2_OFY / 65536.0 + y * C2_H / z) + drawOffsetY;
            vertex->page = (float)(tpage & 0x1f);
            vertex->clut = (float)clut;
            /* Clamped for the same reason the validator tolerates: the sampler is
             * CLAMP_TO_EDGE, so this is what the hardware would do regardless. */
            vertex->u = ModernMesh_ClampUv(texcoord[0]) * 256.0f;
            vertex->v = ModernMesh_ClampUv(texcoord[1]) * 256.0f;
            vertex->r = (unsigned char)(rgba[0] * 128.0f + 0.5f);
            vertex->g = (unsigned char)(rgba[1] * 128.0f + 0.5f);
            vertex->b = (unsigned char)(rgba[2] * 128.0f + 0.5f);
            vertex->a = (unsigned char)(rgba[3] * 255.0f + 0.5f);
            vertex->ppx = vertex->x;
            vertex->ppy = vertex->y;
            vertex->ppw = (float)z;
            vertex->vsx = (float)x;
            vertex->vsy = (float)y;
            vertex->vsz = (float)z;
            entry->drawSz[output++] = (unsigned short)z;
        }
    }
    entry->drawCount = (int)output;
    return entry->drawCount == (int)outputCount;
}

int Pc_ModernMesh_Emit(void* object, void* orderingTable, int shift)
{
    GsDOBJ2* drawObject = (GsDOBJ2*)object;
    GsOT* ot = (GsOT*)orderingTable;
    int tpage = 0, clut = 0, bucket, i;
    DR_PSYX_MODERN_MESH* packet;
    PcModernMeshEntry* entry;

    if (drawObject == NULL || drawObject->tmd == NULL || drawObject->id == 0 || ot == NULL || shift < 0)
        return 0;
    if (Pc_ModernMesh_Find((s32)drawObject->id - 1) == NULL ||
        !ModernMesh_RetailBinding(drawObject, &tpage, &clut))
        return 0;
    /* Find just set this, and every write below lands on THIS item's own
     * storage, so a later emit in the same frame cannot clobber it. */
    entry = s_activeEntry;
    if (entry == NULL)
        return 0;
    entry->drawFormat = (tpage >> 7) & 3;
    if (entry->drawFormat > TF_16_BIT || Pc_ModernShader_Get((TexFormat)entry->drawFormat) == (ShaderID)-1 ||
        !ModernMesh_Expand(entry, tpage, clut))
        return 0;
    entry->drawNativeWidth = entry->drawNativeHeight = entry->drawOffsetX = entry->drawOffsetY = 0;
    entry->drawHiresWidth = entry->drawHiresHeight = 0;

    /* An EMBEDDED texture outranks an installed pack override.
     *
     * The two are authored against different meshes. A GLB's embedded PNG is
     * painted for the GLB's OWN UVs; a texture pack replaces the retail TIM,
     * whose art is laid out for the RETAIL item's UVs. Letting the pack win
     * meant a replacement model was drawn with an atlas that has no
     * relationship to its unwrap — reliably garbage, and hard to attribute
     * because both halves are individually correct.
     *
     * The override still wins for a GLB that ships NO texture: that model
     * deliberately inherits the retail binding and is unwrapped against it, so
     * the pack's higher-resolution version of that same art is exactly right.
     * Which is also why the lookup is not merely reordered but SKIPPED when a
     * mesh is self-textured — its (tpage, clut) is the stock item's, so the
     * lookup would hit an override meant for geometry that is no longer there. */
    if (entry->mesh.hasEmbeddedTexture)
    {
        if (!ModernMesh_EnsureEmbeddedTexture(&entry->mesh))
        {
            /* Fail CLOSED to the stock model rather than falling through to the
             * retail binding: this mesh's UVs are its own, so retail art would
             * be the same mismatch the ordering above exists to prevent. */
            SH_DBG("[MODERN_MESH] embedded texture decode/upload failed — native item model");
            return 0;
        }
        entry->drawTexture = entry->mesh.embeddedTextureId;
        entry->drawFormat = TF_32_BIT_RGBA;
        /* Modern UVs are stored as 0..256. The RGBA shader computes
         * texture2D(..., uv / nativeSize), so a 256x256 denominator maps the
         * full authored 0..1 range to the full embedded image. Setting hires
         * dimensions to zero disables native-cell footprint clamping. */
        entry->drawNativeWidth = entry->drawNativeHeight = 256;
        entry->drawOffsetX = entry->drawOffsetY = 0;
        entry->drawHiresWidth = entry->drawHiresHeight = 0;
        if ((entry->mesh.loggedTextureSources & PcModernTextureSource_EmbeddedGlb) == 0)
        {
            SH_DBG("[MODERN_MESH] item=%d texture source=embedded-glb texture=%u image=%dx%d",
                   (int)entry->mesh.fileIdx, (unsigned)entry->drawTexture,
                   entry->mesh.embeddedTextureWidth, entry->mesh.embeddedTextureHeight);
            entry->mesh.loggedTextureSources |= PcModernTextureSource_EmbeddedGlb;
        }
    }
    else if ((entry->drawTexture = HiresOverride_LookupByTpageClut(
                  tpage, clut, &entry->drawNativeWidth, &entry->drawNativeHeight,
                  &entry->drawOffsetX, &entry->drawOffsetY,
                  &entry->drawHiresWidth, &entry->drawHiresHeight)) != 0)
    {
        entry->drawFormat = TF_32_BIT_RGBA;
        if ((entry->mesh.loggedTextureSources & PcModernTextureSource_InstalledOverride) == 0)
        {
            SH_DBG("[MODERN_MESH] item=%d texture source=installed-override texture=%u",
                   (int)entry->mesh.fileIdx, (unsigned)entry->drawTexture);
            entry->mesh.loggedTextureSources |= PcModernTextureSource_InstalledOverride;
        }
    }
    else
    {
        entry->drawTexture = g_vramTexture;
        if ((entry->mesh.loggedTextureSources & PcModernTextureSource_RetailVram) == 0)
        {
            SH_DBG("[MODERN_MESH] item=%d texture source=retail-vram", (int)entry->mesh.fileIdx);
            entry->mesh.loggedTextureSources |= PcModernTextureSource_RetailVram;
        }
    }
    bucket = 0;
    for (i = 0; i < entry->drawCount; i++)
        bucket += entry->drawSz[i] >> (shift + 2);
    bucket /= entry->drawCount;
    if (bucket < 1)
        bucket = 1;
    if (bucket >= (1 << ot->length))
        bucket = (1 << ot->length) - 1;
    packet = (DR_PSYX_MODERN_MESH*)GsOUT_PACKET_P;
    setlen(packet, 2);
    packet->code[0] = 0xB3000000;
    packet->code[1] = (u32)entry->mesh.fileIdx + 1u;
    addPrim(&ot->org[bucket], packet);
    GsOUT_PACKET_P = (PACKET*)((unsigned char*)packet + sizeof(*packet));
    return 1;
}

int Pc_ModernMesh_PrepareDraw(u32 meshHandle, PcModernDrawBinding* binding)
{
    PcModernMeshEntry* entry = NULL;
    int i;

    if (binding == NULL)
        return 0;
    memset(binding, 0, sizeof(*binding));
    binding->texFormat = -1;
    if (meshHandle == 0)
        return 0;
    /* Resolve the item from the handle the emitted packet carries, NOT from a
     * single global "active" record. The carousel emits several items per
     * frame; by the time the renderer drains the ordering table the global
     * points at whichever item was emitted LAST, so every other item failed
     * this gate. That is why exactly one imported mesh was visible per frame.
     * Still fail-closed: an unknown handle draws nothing. */
    for (i = 0; i < s_registryCount; i++)
    {
        if ((u32)s_registry[i].mesh.fileIdx + 1u == meshHandle)
        {
            entry = &s_registry[i];
            break;
        }
    }
    if (entry == NULL || entry->drawCount < 3 ||
        Pc_ModernShader_Get((TexFormat)entry->drawFormat) == (ShaderID)-1 ||
        !Pc_ModernVertex_UploadExact(entry->drawVertices, entry->drawSz, entry->drawCount))
        return 0;
    binding->textureId = entry->drawTexture;
    binding->texFormat = entry->drawFormat;
    binding->vertexCount = entry->drawCount;
    binding->nativeWidth = entry->drawNativeWidth;
    binding->nativeHeight = entry->drawNativeHeight;
    binding->offsetX = entry->drawOffsetX;
    binding->offsetY = entry->drawOffsetY;
    binding->hiresWidth = entry->drawHiresWidth;
    binding->hiresHeight = entry->drawHiresHeight;
    return 1;
}
