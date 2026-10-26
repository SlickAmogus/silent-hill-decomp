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
    "\tuniform int u_skinFlat;\n" \
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
    "\tuniform float u_skinBright;\n" \
    "\tuniform int u_skinOrm;\n" \
    "\tuniform sampler2D u_skinOrmTex;\n" \
    "\tuniform vec3 u_skinMat;\n" \
    "\tuniform int u_skinEmissiveOn;\n" \
    "\tuniform sampler2D u_skinEmissiveTex;\n" \
    "\tuniform vec3 u_skinEmissive;\n" \
    "\tvec3 SkinGGX(vec3 N, vec3 V, vec3 L, vec3 F0, float rough) {\n" \
    "\t\tfloat nl = dot(N, L);\n" \
    "\t\tif (nl <= 0.0) return vec3(0.0);\n" \
    "\t\tfloat nv = max(dot(N, V), 1e-3);\n" \
    "\t\tvec3 H = normalize(L + V);\n" \
    "\t\tfloat nh = max(dot(N, H), 0.0);\n" \
    "\t\tfloat vh = max(dot(V, H), 0.0);\n" \
    "\t\tfloat a = rough * rough;\n" \
    "\t\tfloat a2 = a * a;\n" \
    "\t\tfloat dd = nh * nh * (a2 - 1.0) + 1.0;\n" \
    "\t\tfloat k = (rough + 1.0) * (rough + 1.0) / 8.0;\n" \
    "\t\tfloat G = (nl / (nl * (1.0 - k) + k)) * (nv / (nv * (1.0 - k) + k));\n" \
    "\t\tvec3 F = F0 + (1.0 - F0) * pow(1.0 - vh, 5.0);\n" \
    "\t\treturn F * (a2 / (dd * dd * 4.0 * nv) * G);\n" \
    "\t}\n"
#define PC_SKIN_SHADE_AT "\t\tvec3 flAlbedo = fragColor.rgb;\n\t\tfragColor *= PpLitColor();\n"
#define PC_SKIN_SHADE \
    "\t\tvec3 flAlbedo = fragColor.rgb;\n" \
    "\t\tvec3 skN = vec3(0.0);\n" \
    "\t\tvec3 skV = vec3(0.0, 0.0, -1.0);\n" \
    "\t\tvec3 skF0 = vec3(0.04);\n" \
    "\t\tfloat skRough = 1.0;\n" \
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
    "\t\t\tfloat vLen = length(v_viewpos);\n" \
    "\t\t\tif (vLen > 1e-4) skV = -v_viewpos / vLen;\n" \
    "\t\t\tvec3 orm = (u_skinOrm > 0) ? texture2D(u_skinOrmTex, tc).rgb : vec3(1.0);\n" \
    "\t\t\tfloat ao = mix(1.0, orm.r, clamp(u_skinMat.z, 0.0, 1.0));\n" \
    "\t\t\tskRough = clamp(orm.g * u_skinMat.y, 0.05, 1.0);\n" \
    "\t\t\tfloat metal = clamp(orm.b * u_skinMat.x, 0.0, 1.0);\n" \
    "\t\t\tskF0 = mix(vec3(0.04), flAlbedo, metal);\n" \
    "\t\t\tvec3 amb;\n" \
    "\t\t\tvec3 spec = vec3(0.0);\n" \
    "\t\t\tvec3 skBase;\n" \
    "\t\t\tif (u_skinFlat == 0) {\n" \
    "\t\t\t\tvec3 r1 = u_skinL1;\n" \
    "\t\t\t\tif (u_skinPoint > 0) {\n" \
    "\t\t\t\t\tvec3 toL = u_skinLightPos - v_viewpos;\n" \
    "\t\t\t\t\tfloat dL = length(toL);\n" \
    "\t\t\t\t\tif (dL > 1e-4) r1 = -(toL / dL) * u_skinL1Mag;\n" \
    "\t\t\t\t}\n" \
    "\t\t\t\tvec3 ir = clamp(vec3(dot(u_skinL0, -N), dot(r1, -N), dot(u_skinL2, -N)), 0.0, 8.0);\n" \
    "\t\t\t\tamb = u_skinBK * ao;\n" \
    "\t\t\t\tvec3 lit = clamp(amb + vec3(dot(u_skinLC0, ir), dot(u_skinLC1, ir), dot(u_skinLC2, ir)), 0.0, 1.0);\n" \
    "\t\t\t\tskBase = min(lit * u_skinBright, vec3(1.0)) * 2.0;\n" \
    "\t\t\t\tfloat l0 = length(u_skinL0);\n" \
    "\t\t\t\tfloat l1 = length(r1);\n" \
    "\t\t\t\tfloat l2 = length(u_skinL2);\n" \
    "\t\t\t\tif (l0 > 1e-4) spec += vec3(u_skinLC0.x, u_skinLC1.x, u_skinLC2.x) * l0 * SkinGGX(N, skV, -u_skinL0 / l0, skF0, skRough);\n" \
    "\t\t\t\tif (l1 > 1e-4) spec += vec3(u_skinLC0.y, u_skinLC1.y, u_skinLC2.y) * l1 * SkinGGX(N, skV, -r1 / l1, skF0, skRough);\n" \
    "\t\t\t\tif (l2 > 1e-4) spec += vec3(u_skinLC0.z, u_skinLC1.z, u_skinLC2.z) * l2 * SkinGGX(N, skV, -u_skinL2 / l2, skF0, skRough);\n" \
    "\t\t\t\tspec *= u_skinBright * 3.14159;\n" \
    "\t\t\t} else {\n" \
    "\t\t\t\tskBase = v_color.rgb * ao;\n" \
    "\t\t\t\tamb = v_color.rgb * 0.5 * ao;\n" \
    "\t\t\t}\n" \
    "\t\t\tif (u_ppHeroOn > 0) {\n" \
    "\t\t\t\tvec3 lifted = PpHero(skBase, N);\n" \
    "\t\t\t\tskBase += (lifted - skBase) * ao;\n" \
    "\t\t\t\tspec += vec3(u_ppHero.w * 0.5 * 3.14159) * SkinGGX(N, skV, u_ppHero.xyz, skF0, skRough);\n" \
    "\t\t\t}\n" \
    "\t\t\tskBase *= mix(1.0, 0.35, metal);\n" \
    "\t\t\tspec += skF0 * amb * 2.0 * metal * (1.0 - 0.5 * skRough);\n" \
    "\t\t\tfragColor *= vec4(skBase, v_color.a);\n" \
    "\t\t\tfragColor.rgb += min(spec, vec3(1.5));\n" \
    "\t\t} else {\n" \
    "\t\t\tfragColor *= v_color;\n" \
    "\t\t}\n"
#define PC_SKIN_BEAM_AT "\t\t\t\t\tif (dot(N, flP) > 0.0) N = -N;\n"
#define PC_SKIN_BEAM \
    "\t\t\t\t\tif (dot(N, flP) > 0.0) N = -N;\n" \
    "\t\t\t\t\tif (u_skinLit > 0) N = skN;\n"
/* The flashlight beam adds its own highlight; emissive goes in after the
 * beam has dimmed the scene, so glowing parts stay lit in the dark. */
#define PC_SKIN_BEAMSPEC_AT "\t\t\t\tvec3 fl = u_flColor * (cone * atten * ndl * shadow);\n"
#define PC_SKIN_BEAMSPEC \
    "\t\t\t\tvec3 fl = u_flColor * (cone * atten * ndl * shadow);\n" \
    "\t\t\t\tif (u_skinLit > 0) fragColor.rgb += min(SkinGGX(skN, skV, L, skF0, skRough) * u_flColor * (cone * atten * shadow * 3.14159 * (1.0 - clamp(v_fade, 0.0, 1.0))), vec3(1.5));\n"
#define PC_SKIN_EMISSIVE_AT "\t\tfloat fogAmt = clamp(v_fogAmount * u_fogStrength, 0.0, 1.0);\n"
#define PC_SKIN_EMISSIVE \
    "\t\tif (u_skinEmissiveOn > 0) fragColor.rgb += texture2D(u_skinEmissiveTex, tc).rgb * u_skinEmissive;\n" \
    "\t\tfloat fogAmt = clamp(v_fogAmount * u_fogStrength, 0.0, 1.0);\n"

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
        { PC_SKIN_BEAMSPEC_AT, PC_SKIN_BEAMSPEC },
        { PC_SKIN_EMISSIVE_AT, PC_SKIN_EMISSIVE },
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
