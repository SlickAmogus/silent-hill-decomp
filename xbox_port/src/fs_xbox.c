/*
 * fs_xbox.c - Xbox file-system setup for the Silent Hill port.
 *
 * Mounts the running XBE's own directory as D: so the debug log and the game
 * data (the BIN disc image, read by the reused xa_player.c) live next to the
 * executable — "same directory as the XBE". nxdk's automatic D: mount is in a
 * separate library (libnxdk_automount_d) that this project does not link, so
 * do it explicitly here (same logic as nxdk's automount_d.c).
 *
 * Also mounts E: (the HDD data partition) and resolves the memory-card save
 * directory (E:\UDATA\SH010000, dashboard-managed) for mcard_xbox.c.
 */
#include <nxdk/mount.h>
#include <nxdk/path.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "sh_log.h"

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

/* Mount E: — the Xbox HDD "data" partition (\Device\Harddisk0\Partition1),
 * where the dashboard keeps its managed save area (E:\UDATA\<title>\).
 * Idempotent; same pattern as nxdk's winapi_drivelist sample. Returns 1 when
 * the drive is available. */
int XboxFs_MountE(void)
{
    if (nxIsDriveMounted('E'))
        return 1;
    return nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\") ? 1 : 0;
}

static int XboxFs_EnsureDir(const char* path)
{
    if (CreateDirectoryA(path, NULL))
        return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* The MS dashboard's save-game manager lists every E:\UDATA\<dir> that
 * carries a TitleMeta.xbx and labels it with the TitleName line. Format is
 * UTF-16LE text with a BOM. Written once; failure is non-fatal (the save
 * still works, the dashboard entry is just unnamed). */
static void XboxFs_WriteTitleMeta(const char* dir)
{
    static const char meta[] = "TitleName=Silent Hill\r\n";
    char  path[128];
    FILE* f;
    int   i;

    snprintf(path, sizeof(path), "%s\\TitleMeta.xbx", dir);
    f = fopen(path, "rb");
    if (f) { fclose(f); return; }

    f = fopen(path, "wb");
    if (!f) {
        SH_DBG("[MCRD] TitleMeta.xbx create failed in %s", dir);
        return;
    }
    {
        unsigned char bom[2] = { 0xFF, 0xFE };
        fwrite(bom, 1, 2, f);
    }
    for (i = 0; meta[i] != '\0'; i++) {
        unsigned char wc[2];
        wc[0] = (unsigned char)meta[i];
        wc[1] = 0;
        fwrite(wc, 1, 2, f);
    }
    fclose(f);
    SH_DBG("[MCRD] wrote %s", path);
}

/* Resolve the memory-card save directory (creating it if needed) for
 * mcard_xbox.c. Primary: E:\UDATA\SH010000 — the dashboard-managed save
 * area. CXBE hardcodes TitleID 0xFFFF0002 for ALL nxdk homebrew (it has no
 * -TITLEID option; see nxdk/tools/cxbe/Xbe.cpp), so instead of that
 * collision-prone shared id we use the stable unique dir name "SH010000".
 * The dashboard doesn't cross-check dir name against the XBE's title id —
 * any UDATA dir with a TitleMeta.xbx is listed. Fallback: D:\SilentHill\save
 * next to the XBE. Returns 1 and fills `out`, or 0 if nothing is writable. */
int XboxFs_ResolveSaveDir(char* out, int outSize)
{
    if (XboxFs_MountE()) {
        if (XboxFs_EnsureDir("E:\\UDATA") && XboxFs_EnsureDir("E:\\UDATA\\SH010000")) {
            XboxFs_WriteTitleMeta("E:\\UDATA\\SH010000");
            snprintf(out, outSize, "E:\\UDATA\\SH010000");
            return 1;
        }
        SH_DBG("[MCRD] E:\\UDATA\\SH010000 create failed (err=%d); falling back to D:",
               (int)GetLastError());
    } else {
        SH_DBG("[MCRD] E: mount failed; falling back to D:");
    }

    if (XboxFs_EnsureDir("D:\\SilentHill") && XboxFs_EnsureDir("D:\\SilentHill\\save")) {
        snprintf(out, outSize, "D:\\SilentHill\\save");
        return 1;
    }
    SH_DBG("[MCRD] fallback D:\\SilentHill\\save create failed (err=%d)",
           (int)GetLastError());
    return 0;
}
