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
 * where NNN is the one-based sample number the launcher's Audio tool shows, and
 * BANK is the bank's name on the disc -- MAP000, MAP001_2, PISTOL. The tool
 * labels a sample "MAP000_005", which is bank MAP000 sample 5, NOT a bank
 * called MAP000_005; the separator before the number is a dot here.
 * <BANK>_<NNN>.wav is accepted too, because that is exactly what the tool names
 * its exports, so an unedited round trip works either way.
 *
 * The WAV plays at ITS OWN sample rate: what you hear in your audio editor is
 * what plays in game, at any rate the editor saves. The mixer uploads the file
 * at the rate its header declares and treats the pitch the game keys the voice
 * with as 1.0, so the original sample's (often very low) authoring rate no
 * longer matters. Pitch the game applies AFTER the trigger still scales
 * relative to that baseline, so modulated sounds keep their modulation; what a
 * replacement gives up is trigger-time pitch variation. If the RIFF header
 * cannot be read the file falls back to the old fixed-44100 upload (speed then
 * depends on the original sample's rate); the [SFXMOD] log line prints the
 * rate it read from each file, and 0 there is the tell.
 *
 * (The tool's own "replace inside the VAB" path resamples for you; that applies
 * to a repacked bank, not to these loose files.)
 */

/* Called after a bank's sample data reaches SPU RAM. `vabHeader` is the parsed
 * VH still resident in main RAM, `spuBase` the address the body was written to,
 * and `discSector` the bank's active start sector (used to recover its name).
 * Safe to call repeatedly: a slot's previous registrations are dropped first. */
void Pc_SfxOverride_OnBankLoaded(const void* vabHeader, int spuBase, int discSector);

/* Playback-time lookup by the SPU address a voice was pointed at. Returns 1 and
 * fills the outputs when that address is an overridden sample. The samples are
 * owned by this module and stay valid until the bank is replaced. */
int Pc_SfxOverride_Lookup(int spuAddr, const short** outPcm, int* outSampleCount, int* outRate);

/* Drop everything (map teardown / shutdown). */
void Pc_SfxOverride_Reset(void);

#endif /* SH_PC_PORT */

#endif /* PC_SFX_OVERRIDE_H */
