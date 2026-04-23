#ifndef PC_PORT_XA_PLAYER_H
#define PC_PORT_XA_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void XaPlayer_Play(uint16_t xaIdx);
void XaPlayer_Stop(void);
void XaPlayer_Update(void);
void XaPlayer_SetVolume(int16_t volLeft, int16_t volRight);

#ifdef __cplusplus
}
#endif

#endif // PC_PORT_XA_PLAYER_H
