#ifndef static_assert
#define static_assert _Static_assert
#endif
#include "pc_modern_shader.h"

#include <stdio.h>
#include <string.h>

static const char* kLegacySource =
    "AFFINE_VARYING vec4 v_texcoord;\n"
    "void main() {\n"
    "\t\tv_texcoord = a_texcoord;\n"
    "\t\tv_texcoord.xy += a_extra.xy * 0.5;\n"
    "\t\tv_color = a_color;\n"
    "\t\tv_color.xyz *= a_texcoord.z;\n"
    "}\n";

static const char* kMissingColorMultiply =
    "AFFINE_VARYING vec4 v_texcoord;\n"
    "void main() {\n"
    "\t\tv_texcoord = a_texcoord;\n"
    "}\n";

ShaderID GR_Shader_Compile(const char* source)
{
    static ShaderID nextShader = 100;
    return source != NULL ? nextShader++ : (ShaderID)-1;
}

int main(void)
{
    const char* source;
    Pc_ModernShader_Initialise(kLegacySource, kLegacySource,
                               kLegacySource, kMissingColorMultiply);
    source = Pc_ModernShader_GetSource(TF_4_BIT);
    if (source == NULL || strstr(source, "v_texcoord = vec4(a_texcoord.xy, 1.0, 0.0);") == NULL)
        return 1;
    if (strstr(source, "AFFINE_VARYING vec4 v_texcoord;") == NULL ||
        strstr(source, "v_texcoord.xy += a_extra.xy * 0.5;") == NULL ||
        strstr(source, "v_color = a_color;") == NULL)
        return 2;
    /* The a_texcoord.z colour multiplier must be gone and replaced by the
     * constant the legacy path supplies via the bright attribute. */
    if (strstr(source, "v_color.xyz *= 2.0;") == NULL ||
        strstr(source, "v_color.xyz *= a_texcoord.z;") != NULL)
        return 3;
    if (strcmp(kLegacySource,
               "AFFINE_VARYING vec4 v_texcoord;\nvoid main() {\n"
               "\t\tv_texcoord = a_texcoord;\n"
               "\t\tv_texcoord.xy += a_extra.xy * 0.5;\n"
               "\t\tv_color = a_color;\n"
               "\t\tv_color.xyz *= a_texcoord.z;\n}\n") != 0)
        return 4;
    if (Pc_ModernShader_Get(TF_4_BIT) == (ShaderID)-1 ||
        Pc_ModernShader_Get((TexFormat)99) != (ShaderID)-1 ||
        Pc_ModernShader_GetSource((TexFormat)99) != NULL)
        return 5;
    /* Fail-closed: a legacy source missing the colour multiplier must yield no
     * modern variant at all, never a half-patched one. */
    if (Pc_ModernShader_GetSource(TF_32_BIT_RGBA) != NULL ||
        Pc_ModernShader_Get(TF_32_BIT_RGBA) != (ShaderID)-1)
        return 6;
    puts("modern_shader_test=PASS");
    return 0;
}
