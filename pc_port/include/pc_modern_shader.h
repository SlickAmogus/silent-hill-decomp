/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_MODERN_SHADER_H
#define PC_MODERN_SHADER_H

#include "PsyX/PsyX_render.h"

#ifdef __cplusplus
extern "C" {
#endif

void Pc_ModernShader_Initialise(const char* source4,
                                const char* source8,
                                const char* source16,
                                const char* sourceRgba);
ShaderID Pc_ModernShader_Get(TexFormat format);
const char* Pc_ModernShader_GetSource(TexFormat format);

#ifdef __cplusplus
}
#endif

#endif /* PC_MODERN_SHADER_H */
