/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_INVENTORY_LAYOUT_H
#define PC_INVENTORY_LAYOUT_H
#include "pc_title_screen.h"
typedef struct { float sx,sy,sw,sh,x,y,w,h; } PcInventoryPanel;
/* Source coordinates are fractions of the original 320x448 picture. */
int Pc_InventoryLayout(float width,float height,PcInventoryPanel panels[7]);
int Pc_InventoryPointer(float x,float y,float width,float height,float* originalX,float* originalY);
int Pc_InventoryCommandRow(float x,float y,float width,float height,int count);
int Pc_InventoryAdapted(void);
void Pc_InventoryDraw(const PcTitlePainter* painter);
void Pc_InventoryClose(void);
void Pc_InventoryTextBegin(int commands);
void Pc_InventoryTextEnd(void);
int Pc_InventoryTextCapture(const char* text,int length,int x,int y,int* complete);
#endif
