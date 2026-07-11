using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Identifies Silent Hill disc images the same way the game does
    /// (pc_port/src/main_pc.c Pc_DetectRegionFromBin): read the ISO9660 root
    /// directory out of a raw 2352-byte-sector BIN and match the boot
    /// executable's serial prefix. Keep the two implementations in agreement —
    /// a bin the launcher reports must be one the game would accept.
    /// </summary>
    public static class DiscProbe
    {
        public class Disc
        {
            public string Path;
            public string FileName;
            public string Serial;      // e.g. "SLES-01514"
            public string Region;      // "USA" / "PAL" / "JAP" (game region key)
            public string RegionLabel; // shown to the user
            public bool   Supported;   // false = recognized but the game can't load it yet
        }

        // Serial prefix -> region. Table-driven, mirroring the game-side probe
        // (Pc_DetectRegionFromBin) — keep the two in agreement.
        private static readonly object[,] RegionMap = {
            { "SLUS", "USA", "USA / NTSC-U",                  true },
            { "SLES", "PAL", "PAL / Europe (En,Fr,De,Es,It)", true },
            { "SLPS", "JAP", "Japan / NTSC-J",                true },
            { "SLPM", "JAP", "Japan / NTSC-J",                true },
            { "SIPS", "JAP", "Japan / NTSC-J",                true },
        };

        /// <summary>
        /// Probe a single .bin. Returns null when the file isn't a raw-sector
        /// PSX bin with a recognized boot serial (matching the game, which
        /// skips unknown serials during autodetect).
        /// </summary>
        public static Disc Probe(string path)
        {
            try
            {
                using (var f = File.OpenRead(path))
                {
                    var sec = new byte[2048];

                    // PVD at LBA 16 (raw 2352-byte sectors; 2048 data bytes at +24).
                    f.Seek(16L * 2352 + 24, SeekOrigin.Begin);
                    if (f.Read(sec, 0, 2048) != 2048) return null;
                    long rootLba = sec[158] | (sec[159] << 8) | (sec[160] << 16) | ((long)sec[161] << 24);

                    f.Seek(rootLba * 2352 + 24, SeekOrigin.Begin);
                    if (f.Read(sec, 0, 2048) != 2048) return null;

                    for (int o = 0; o + 33 < 2048; )
                    {
                        int recLen = sec[o];
                        if (recLen == 0) break;
                        int nameLen = sec[o + 32];
                        if (o + 33 + nameLen <= 2048 && nameLen >= 4)
                        {
                            string name = Encoding.ASCII.GetString(sec, o + 33, nameLen);
                            for (int r = 0; r < RegionMap.GetLength(0); r++)
                            {
                                if (name.StartsWith((string)RegionMap[r, 0], StringComparison.Ordinal))
                                {
                                    return new Disc
                                    {
                                        Path        = path,
                                        FileName    = System.IO.Path.GetFileName(path),
                                        Serial      = FormatSerial(name),
                                        Region      = (string)RegionMap[r, 1],
                                        RegionLabel = (string)RegionMap[r, 2],
                                        Supported   = (bool)RegionMap[r, 3],
                                    };
                                }
                            }
                        }
                        o += recLen;
                    }
                }
            }
            catch { /* unreadable / not a bin — treat as no match */ }
            return null;
        }

        /// <summary>All recognized discs in a folder (every *.bin probed).</summary>
        public static List<Disc> Scan(string folder)
        {
            var found = new List<Disc>();
            try
            {
                foreach (var f in Directory.GetFiles(folder, "*.bin"))
                {
                    var d = Probe(f);
                    if (d != null) found.Add(d);
                }
            }
            catch { }
            return found;
        }

        /// <summary>"SLES_015.14;1" -> "SLES-01514".</summary>
        private static string FormatSerial(string bootName)
        {
            int semi = bootName.IndexOf(';');
            if (semi >= 0) bootName = bootName.Substring(0, semi);
            return bootName.Replace('_', '-').Replace(".", "");
        }
    }
}
