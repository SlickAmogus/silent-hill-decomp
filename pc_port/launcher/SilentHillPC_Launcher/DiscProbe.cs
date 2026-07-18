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
            public bool   Modified;    // BODYPROG differs from the retail disc (fan translation / patch)
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

        // SHA1 over three sampled BODYPROG.BIN data sectors on the retail
        // discs: relative sector 0 (menu/loading strings), 2 (the FONT16
        // kerning table at file 0x120C) and 274 (the item name/description
        // pointer arrays at 0x89000) — the spots translation patches actually
        // edit. The XOR keystream is position-based, so only an edit inside a
        // sampled sector changes its ciphertext; a patch that avoids all three
        // (and only edits VIN overlays) is simply not flagged — the tag is
        // cosmetic, the game detects text changes itself at runtime.
        // Region -> (BODYPROG LBA from the game's file table, hash).
        private static readonly Dictionary<string, Tuple<long, string>> BodyprogMarker =
            new Dictionary<string, Tuple<long, string>> {
                { "USA", Tuple.Create(0x0CFL, "10f14642a565fb8d502397626734f71f270c4e9a") },
                { "PAL", Tuple.Create(0x0D0L, "b2f3679c645650a72805a8afb3c5d68bdde61ebe") },
                { "JAP", Tuple.Create(0x0CFL, "a467247d5f73b83a6ac424ab9c08ec50e874e85c") },
            };

        private static readonly long[] MarkerSectors = { 0, 2, 274 };

        private static bool IsBodyprogModified(FileStream f, string region)
        {
            Tuple<long, string> marker;
            if (!BodyprogMarker.TryGetValue(region, out marker)) return false;
            try
            {
                using (var sha = System.Security.Cryptography.SHA1.Create())
                {
                    var sec = new byte[2048];
                    foreach (long rel in MarkerSectors)
                    {
                        f.Seek((marker.Item1 + rel) * 2352 + 24, SeekOrigin.Begin);
                        if (f.Read(sec, 0, 2048) != 2048) return false;
                        sha.TransformBlock(sec, 0, 2048, null, 0);
                    }
                    sha.TransformFinalBlock(new byte[0], 0, 0);
                    var hex = BitConverter.ToString(sha.Hash).Replace("-", "").ToLowerInvariant();
                    return hex != marker.Item2;
                }
            }
            catch { return false; }
        }

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
                                    string region = (string)RegionMap[r, 1];
                                    return new Disc
                                    {
                                        Path        = path,
                                        FileName    = System.IO.Path.GetFileName(path),
                                        Serial      = FormatSerial(name),
                                        Region      = region,
                                        RegionLabel = (string)RegionMap[r, 2],
                                        Supported   = (bool)RegionMap[r, 3],
                                        Modified    = IsBodyprogModified(f, region),
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
