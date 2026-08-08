#ifndef static_assert
#define static_assert _Static_assert
#endif
#include "pc_modern_shader.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

static char* BuildModernSource(const char* source)
{
    char* uvPatched;
    char* colorPatched;

    uvPatched = ReplaceOnce(source, PC_UV_ASSIGNMENT, PC_FLOAT_UV_ASSIGNMENT);
    if (uvPatched == NULL)
        return NULL;

    colorPatched = ReplaceOnce(uvPatched, PC_COLOR_MULTIPLY, PC_COLOR_MULTIPLY_CONST);
    free(uvPatched);
    return colorPatched;
}

static void InitialiseVariant(TexFormat format, const char* legacySource)
{
    PcModernShaderVariant* variant = FindVariant(format);
    if (variant == NULL || variant->source != NULL)
        return;
    variant->source = BuildModernSource(legacySource);
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
