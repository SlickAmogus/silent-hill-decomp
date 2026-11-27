/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_START_SCREEN_H
#define PC_START_SCREEN_H

/* The launcher's simple view for a build that has no launcher (UWP): shown
 * once at boot, after the window exists and before the disc is mounted, and
 * returns when the player picks Play. dataDir is the disc folder; it is
 * rewritten when a disc turns up on a drive while the page is open. */
void Pc_StartScreen_Run(char* dataDir, int dataDirSize);

#endif /* PC_START_SCREEN_H */
