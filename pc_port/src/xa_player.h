#ifndef PC_PORT_XA_PLAYER_H
#define PC_PORT_XA_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Called by game to start XA playback
// xaIdx: index into g_XaItemData
// fileIdx: which raw file (1-9, maps to 05_02152 through 45_28784)
// sectorOffset: starting sector within that file
// numSectors: how many sectors to read (duration in sectors)
void XaPlayer_PlayWithParams(uint16_t xaIdx, uint16_t fileIdx, uint32_t sectorOffset, uint32_t numSectors);

// Legacy interface (used by Sd_TaskPoolExecute)
void XaPlayer_Play(uint16_t xaIdx);

void XaPlayer_Stop(void);
void XaPlayer_Update(void);
void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight);

#ifdef __cplusplus
}
#endif

#endif // PC_PORT_XA_PLAYER_H
