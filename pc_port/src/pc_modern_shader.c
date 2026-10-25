/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef static_assert
#define static_assert _Static_assert
#endif
#include "pc_modern_shader.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "sh_log.h"

static int s_skinLitShader;

int Pc_ModernShader_SkinLit(void)
{
    return s_skinLitShader;
}

#define PC_MODERN_SHADER_COUNT 4
#define PC_MODERN_ATTRIBUTE_COUNT 8
#define PC_UV_ASSIGNMENT "\t\tv_texcoord = a_texcoord;\n"
#define PC_FLOAT_UV_ASSIGNMENT "\t\tv_texcoord = vec4(a_texcoord.xy, 1.0, 0.0);\n"

/* The modern vertex layout binds a_texcoord with size=2 (GrModernVertex holds
 * only float u,v there), so a_texcoord.z reads the GL default 0.0 instead of
 * the legacy per-vertex "bright" multiplier that PsyX_GPU.cpp sets to 2. The
 * shared GTE vertex shader multiplies v_color.xyz by that component, which
 * drives every modern fragment to pure black. Substitute the constant the
 * legacy path supplies for lit prims. */
#define PC_COLOR_MULTIPLY "\t\tv_color.xyz *= a_texcoord.z;\n"
#define PC_COLOR_MULTIPLY_CONST "\t\tv_color.xyz *= 2.0;\n"

/* Per-pixel character lighting, RGBA variant only (skins always upload RGBA).
 * The PSX lights a character per vertex with the GTE: three light rows dotted
 * with the inward normal, clamped, through the colour matrix, plus the back
 * colour. The same sum runs here per pixel on the interpolated (optionally
 * normal-mapped) normal, and the point light's row is aimed from each pixel
 * instead of from each bone's origin. Uniforms come from pc_skin_chara.c. */
#define PC_SKIN_VARYINGS_AT "varying float v_texfmt;\n"
#define PC_SKIN_VARYINGS \
    "varying float v_texfmt;\n" \
    "varying vec4 v_skinN;\n" \
    "varying vec4 v_skinT;\n" \
    "#ifdef VERTEX\n" \
    "attribute vec4 a_skinNormal;\n" \
    "attribute vec4 a_skinTangent;\n" \
    "#endif\n"
#define PC_SKIN_VERTEX_AT "\t\tv_uvlim = a_uvlim;\n"
#define PC_SKIN_VERTEX \
    "\t\tv_uvlim = a_uvlim;\n" \
    "\t\tv_skinN = a_skinNormal;\n" \
    "\t\tv_skinT = a_skinTangent;\n"
#define PC_SKIN_UNIFORMS_AT "\tuniform vec2 u_hiresHalf;\n"
#define PC_SKIN_UNIFORMS \
    "\tuniform vec2 u_hiresHalf;\n" \
    "\tuniform int u_skinLit;\n" \
    "\tuniform int u_skinNormalMap;\n" \
    "\tuniform sampler2D u_skinNormalTex;\n" \
    "\tuniform int u_skinPoint;\n" \
    "\tuniform vec3 u_skinL0;\n" \
    "\tuniform vec3 u_skinL1;\n" \
    "\tuniform vec3 u_skinL2;\n" \
    "\tuniform float u_skinL1Mag;\n" \
    "\tuniform vec3 u_skinLightPos;\n" \
    "\tuniform vec3 u_skinLC0;\n" \
    "\tuniform vec3 u_skinLC1;\n" \
    "\tuniform vec3 u_skinLC2;\n" \
    "\tuniform vec3 u_skinBK;\n" \
    "\tuniform float u_skinBright;\n"
#define PC_SKIN_SHADE_AT "\t\tvec3 flAlbedo = fragColor.rgb;\n\t\tfragColor *= v_color;\n"
#define PC_SKIN_SHADE \
    "\t\tvec3 flAlbedo = fragColor.rgb;\n" \
    "\t\tvec3 skN = vec3(0.0);\n" \
    "\t\tif (u_skinLit > 0) {\n" \
    "\t\t\tvec3 N = v_skinN.xyz;\n" \
    "\t\t\tfloat nLen = length(N);\n" \
    "\t\t\tN = (nLen > 1e-6) ? N / nLen : vec3(0.0, 0.0, -1.0);\n" \
    "\t\t\tif (u_skinNormalMap > 0 && dot(v_skinT.xyz, v_skinT.xyz) > 1e-8) {\n" \
    "\t\t\t\tvec3 T = v_skinT.xyz - N * dot(N, v_skinT.xyz);\n" \
    "\t\t\t\tfloat tLen = length(T);\n" \
    "\t\t\t\tif (tLen > 1e-6) {\n" \
    "\t\t\t\t\tT /= tLen;\n" \
    "\t\t\t\t\tvec3 B = cross(N, T) * ((v_skinT.w < 0.0) ? -1.0 : 1.0);\n" \
    "\t\t\t\t\tvec3 nm = texture2D(u_skinNormalTex, tc).xyz * 2.0 - 1.0;\n" \
    "\t\t\t\t\tN = normalize(T * nm.x + B * nm.y + N * max(nm.z, 0.05));\n" \
    "\t\t\t\t}\n" \
    "\t\t\t}\n" \
    "\t\t\tskN = N;\n" \
    "\t\t\tvec3 r1 = u_skinL1;\n" \
    "\t\t\tif (u_skinPoint > 0) {\n" \
    "\t\t\t\tvec3 toL = u_skinLightPos - v_viewpos;\n" \
    "\t\t\t\tfloat dL = length(toL);\n" \
    "\t\t\t\tif (dL > 1e-4) r1 = -(toL / dL) * u_skinL1Mag;\n" \
    "\t\t\t}\n" \
    "\t\t\tvec3 ir = clamp(vec3(dot(u_skinL0, -N), dot(r1, -N), dot(u_skinL2, -N)), 0.0, 8.0);\n" \
    "\t\t\tvec3 lit = clamp(u_skinBK + vec3(dot(u_skinLC0, ir), dot(u_skinLC1, ir), dot(u_skinLC2, ir)), 0.0, 1.0);\n" \
    "\t\t\tfragColor *= vec4(min(lit * u_skinBright, vec3(1.0)) * 2.0, v_color.a);\n" \
    "\t\t} else {\n" \
    "\t\t\tfragColor *= v_color;\n" \
    "\t\t}\n"
#define PC_SKIN_BEAM_AT "\t\t\t\t\tif (dot(N, flP) > 0.0) N = -N;\n"
#define PC_SKIN_BEAM \
    "\t\t\t\t\tif (dot(N, flP) > 0.0) N = -N;\n" \
    "\t\t\t\t\tif (u_skinLit > 0) N = skN;\n"

_Static_assert(PC_MODERN_ATTRIBUTE_COUNT <= 8, "modern shader exceeds GLES2 attributes");

typedef struct PcModernShaderVariant
{
    TexFormat format;
    ShaderID shader;
    char* source;
} PcModernShaderVariant;

static PcModernShaderVariant s_variants[PC_MODERN_SHADER_COUNT] = {
    { TF_4_BIT, (ShaderID)-1, NULL },
    { TF_8_BIT, (ShaderID)-1, NULL },
    { TF_16_BIT, (ShaderID)-1, NULL },
    { TF_32_BIT_RGBA, (ShaderID)-1, NULL }
};

static PcModernShaderVariant* FindVariant(TexFormat format)
{
    int i;
    for (i = 0; i < PC_MODERN_SHADER_COUNT; i++)
    {
        if (s_variants[i].format == format)
            return &s_variants[i];
    }
    return NULL;
}

/* Fail-closed single substitution: returns NULL when the needle is absent or
 * appears more than once, so a legacy shader edit can never silently produce a
 * half-patched modern variant. */
static char* ReplaceOnce(const char* source, const char* needle, const char* replacement)
{
    const char* match;
    const size_t oldLength = strlen(needle);
    const size_t newLength = strlen(replacement);
    size_t prefixLength;
    char* result;

    if (source == NULL)
        return NULL;
    match = strstr(source, needle);
    if (match == NULL || strstr(match + oldLength, needle) != NULL)
        return NULL;

    prefixLength = (size_t)(match - source);
    result = (char*)malloc(strlen(source) - oldLength + newLength + 1);
    if (result == NULL)
        return NULL;

    memcpy(result, source, prefixLength);
    memcpy(result + prefixLength, replacement, newLength);
    strcpy(result + prefixLength + newLength, match + oldLength);
    return result;
}

static char* BuildModernSource(const char* source, TexFormat format)
{
    static const char* const skin[][2] = {
        { PC_SKIN_VARYINGS_AT, PC_SKIN_VARYINGS },
        { PC_SKIN_VERTEX_AT, PC_SKIN_VERTEX },
        { PC_SKIN_UNIFORMS_AT, PC_SKIN_UNIFORMS },
        { PC_SKIN_SHADE_AT, PC_SKIN_SHADE },
        { PC_SKIN_BEAM_AT, PC_SKIN_BEAM },
    };
    char* uvPatched;
    char* colorPatched;
    size_t i;

    uvPatched = ReplaceOnce(source, PC_UV_ASSIGNMENT, PC_FLOAT_UV_ASSIGNMENT);
    if (uvPatched == NULL)
        return NULL;

    colorPatched = ReplaceOnce(uvPatched, PC_COLOR_MULTIPLY, PC_COLOR_MULTIPLY_CONST);
    free(uvPatched);
    if (format != TF_32_BIT_RGBA || colorPatched == NULL)
        return colorPatched;
    {
        char* lit = ReplaceOnce(colorPatched, skin[0][0], skin[0][1]);
        for (i = 1; lit != NULL && i < sizeof(skin) / sizeof(skin[0]); i++)
        {
            char* next = ReplaceOnce(lit, skin[i][0], skin[i][1]);
            free(lit);
            lit = next;
        }
        /* Items share this variant, so a shader edit that moves an anchor
         * costs only the per-pixel character path, not the variant. */
        if (lit == NULL)
        {
            SH_DBG("[SKIN] per-pixel lighting patch did not apply (anchor %d); characters stay per-vertex", (int)i - 1);
            return colorPatched;
        }
        free(colorPatched);
        s_skinLitShader = 1;
        return lit;
    }
}

static void InitialiseVariant(TexFormat format, const char* legacySource)
{
    PcModernShaderVariant* variant = FindVariant(format);
    if (variant == NULL || variant->source != NULL)
        return;
    variant->source = BuildModernSource(legacySource, format);
    if (variant->source != NULL)
        variant->shader = GR_Shader_Compile(variant->source);
}

void Pc_ModernShader_Initialise(const char* source4,
                                const char* source8,
                                const char* source16,
                                const char* sourceRgba)
{
    InitialiseVariant(TF_4_BIT, source4);
    InitialiseVariant(TF_8_BIT, source8);
    InitialiseVariant(TF_16_BIT, source16);
    InitialiseVariant(TF_32_BIT_RGBA, sourceRgba);
}

ShaderID Pc_ModernShader_Get(TexFormat format)
{
    PcModernShaderVariant* variant = FindVariant(format);
    return variant != NULL ? variant->shader : (ShaderID)-1;
}

const char* Pc_ModernShader_GetSource(TexFormat format)
{
    PcModernShaderVariant* variant = FindVariant(format);
    return variant != NULL ? variant->source : NULL;
}
