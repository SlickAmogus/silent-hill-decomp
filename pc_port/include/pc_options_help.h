/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_OPTIONS_HELP_H
#define PC_OPTIONS_HELP_H

enum { PC_PREVIEW_NONE, PC_PREVIEW_FOV, PC_PREVIEW_CULLING,
       PC_PREVIEW_CAMERA, PC_PREVIEW_MAP, PC_PREVIEW_CONTROLS };

typedef struct {
    const char* key;
    const char* name[2]; /* English, Russian; UTF-8, never disc-font bytes. */
    const char* description[2];
    int preview;
} PcOptionHelp;

const PcOptionHelp* Pc_OptionsHelp(const char* key);

/* Viewport pixels, top/left origin; pure layout, shared with tests. */
typedef struct {
    float left, top, width, height, listWidth, gap, padding, font;
    int stacked;
} PcOptionsLayout;
PcOptionsLayout Pc_OptionsLayout(float width, float height);
enum { PC_SCENE_NONE, PC_SCENE_SCHOOL, PC_SCENE_STREET, PC_SCENE_WEATHER };
int Pc_OptionsPreviewScene(const char* key);

#endif
