/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_SFX_OVERRIDE_H
#define PC_SFX_OVERRIDE_H

#ifdef SH_PC_PORT

/* Loose-file replacement for individual sounds inside a VAB bank.
 *
 * A bank is a container of ADPCM samples ("VAGs") uploaded to SPU RAM in one
 * block; a voice then plays from an address inside that block. Replacing a
 * sound normally means repacking the whole bank, and a replacement can never
 * exceed what the format can address.
 *
 * This does it the way the texture path does it: when a bank is uploaded, every
 * sample in it is checked for a loose file, and the ones that have one are
 * registered against the SPU address the voice will play from. At playback the
 * PsyCross mixer asks this registry before decoding, and a hit substitutes
 * PC-owned PCM of ANY length or rate. Untouched samples in the same bank never
 * see the registry and decode from the original data exactly as before.
 *
 * Naming, mirroring the texture convention:
 *     gamedata/load/SND/<BANK>.<NNN>.wav      e.g. SND/PISTOL.002.wav
 * where NNN is the one-based sample number the launcher's Audio tool shows.
 *
 * The WAV should be at the rate the Audio tool exported (the sound's real
 * in-game rate). The engine pitches the buffer exactly as it pitched the
 * original, so a file at that rate plays at the intended speed; a file at some
 * other rate plays proportionally faster or slower, like changing tape speed.
 */

/* Called after a bank's sample data reaches SPU RAM. `vabHeader` is the parsed
 * VH still resident in main RAM, `spuBase` the address the body was written to,
 * and `discSector` the bank's active start sector (used to recover its name).
 * Safe to call repeatedly: a slot's previous registrations are dropped first. */
void Pc_SfxOverride_OnBankLoaded(const void* vabHeader, int spuBase, int discSector);

/* Playback-time lookup by the SPU address a voice was pointed at. Returns 1 and
 * fills the outputs when that address is an overridden sample. The samples are
 * owned by this module and stay valid until the bank is replaced. */
int Pc_SfxOverride_Lookup(int spuAddr, const short** outPcm, int* outSampleCount);

/* Drop everything (map teardown / shutdown). */
void Pc_SfxOverride_Reset(void);

#endif /* SH_PC_PORT */

#endif /* PC_SFX_OVERRIDE_H */
