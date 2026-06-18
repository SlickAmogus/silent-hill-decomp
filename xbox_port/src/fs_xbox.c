/*
 * fs_xbox.c - Xbox file-system setup for the Silent Hill port.
 *
 * Mounts the running XBE's own directory as D: so the debug log and the game
 * data (the BIN disc image, read by the reused xa_player.c) live next to the
 * executable — "same directory as the XBE". nxdk's automatic D: mount is in a
 * separate library (libnxdk_automount_d) that this project does not link, so
 * do it explicitly here (same logic as nxdk's automount_d.c).
 */
#include <nxdk/mount.h>
#include <nxdk/path.h>
#include <windows.h>
#include <string.h>

void XboxFs_MountHomeDrive(void)
{
    char  ntPath[MAX_PATH];
    char* lastSlash;

    if (nxIsDriveMounted('D'))
        return;

    nxGetCurrentXbeNtPath(ntPath);

    /* Strip the XBE filename, keep the trailing backslash -> the directory. */
    lastSlash = strrchr(ntPath, '\\');
    if (lastSlash)
        *(lastSlash + 1) = '\0';

    nxMountDrive('D', ntPath);
}
