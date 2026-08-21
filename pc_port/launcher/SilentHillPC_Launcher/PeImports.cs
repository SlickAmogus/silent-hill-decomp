using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Minimal PE import-table reader for install-time screening of DLL mods.
    /// Mirrors the game's dll_security.c fingerprint: edited game code (map
    /// overlay DLLs) imports only the game executable and the C runtime, so
    /// anything else is worth naming in the install warning. This is a lint
    /// for the consent dialog, not a safety proof.
    /// </summary>
    public static class PeImports
    {
        static readonly string[] AllowedForGameCode =
        {
            "silenthillpc.exe", "kernel32.dll", "msvcrt.dll", "ucrtbase.dll",
            "libgcc_s_seh-1.dll", "libwinpthread-1.dll", "libssp-0.dll"
        };

        static bool IsAllowed(string dll)
        {
            string d = dll.ToLowerInvariant();
            if (AllowedForGameCode.Contains(d)) return true;
            return d.StartsWith("api-ms-win-crt-");
        }

        /// <summary>Imported DLL names of a PE image, or null if not parseable
        /// as a 64-bit PE (a non-PE or 32-bit file reports null and the caller
        /// treats it as suspicious in itself).</summary>
        public static List<string> GetImportedDlls(byte[] img)
        {
            try
            {
                if (img == null || img.Length < 0x40 || img[0] != 'M' || img[1] != 'Z') return null;
                int e_lfanew = BitConverter.ToInt32(img, 0x3C);
                if (e_lfanew <= 0 || e_lfanew + 0x108 > img.Length) return null;
                if (BitConverter.ToUInt32(img, e_lfanew) != 0x4550) return null; // "PE\0\0"

                int fileHdr = e_lfanew + 4;
                ushort numSections = BitConverter.ToUInt16(img, fileHdr + 2);
                ushort optSize     = BitConverter.ToUInt16(img, fileHdr + 16);
                int optHdr = fileHdr + 20;
                ushort magic = BitConverter.ToUInt16(img, optHdr);
                if (magic != 0x20B) return null; // PE32+ only, like the game

                // Import directory = entry 1
                int ddBase = optHdr + 112;
                uint importRva = BitConverter.ToUInt32(img, ddBase + 8);
                if (importRva == 0) return new List<string>();

                // Section headers follow the optional header
                int sec = optHdr + optSize;
                var sections = new List<Tuple<uint, uint, uint>>(); // va, size, raw
                for (int i = 0; i < numSections && sec + 40 <= img.Length; i++, sec += 40)
                {
                    uint vsz = BitConverter.ToUInt32(img, sec + 8);
                    uint va  = BitConverter.ToUInt32(img, sec + 12);
                    uint rsz = BitConverter.ToUInt32(img, sec + 16);
                    uint raw = BitConverter.ToUInt32(img, sec + 20);
                    sections.Add(Tuple.Create(va, Math.Max(vsz, rsz), raw));
                }
                Func<uint, int> rvaToOff = rva =>
                {
                    foreach (var t in sections)
                        if (rva >= t.Item1 && rva < t.Item1 + t.Item2)
                            return (int)(t.Item3 + (rva - t.Item1));
                    return -1;
                };

                var result = new List<string>();
                int desc = rvaToOff(importRva);
                for (int i = 0; i < 256 && desc >= 0 && desc + 20 <= img.Length; i++, desc += 20)
                {
                    uint nameRva = BitConverter.ToUInt32(img, desc + 12);
                    if (nameRva == 0) break;
                    int off = rvaToOff(nameRva);
                    if (off < 0) continue;
                    int end = off;
                    while (end < img.Length && img[end] != 0 && end - off < 127) end++;
                    result.Add(System.Text.Encoding.ASCII.GetString(img, off, end - off));
                }
                return result;
            }
            catch { return null; }
        }

        /// <summary>Scan every DLL inside a dropped mod (folder or .zip) and
        /// report those that don't look like edited game code, as
        /// "name.dll (imports: user32.dll, ws2_32.dll)" lines. .rar/.7z can't
        /// be peeked here; those get a generic note from the caller.</summary>
        public static List<string> ScanModForSuspiciousDlls(string path)
        {
            var findings = new List<string>();
            try
            {
                if (Directory.Exists(path))
                {
                    foreach (var f in Directory.GetFiles(path, "*.dll", SearchOption.AllDirectories))
                        Check(Path.GetFileName(f), File.ReadAllBytes(f), findings);
                }
                else if (File.Exists(path) && Path.GetExtension(path).Equals(".zip", StringComparison.OrdinalIgnoreCase))
                {
                    using (var za = ZipFile.OpenRead(path))
                    {
                        foreach (var en in za.Entries)
                        {
                            if (!en.FullName.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) continue;
                            if (en.Length > 64 * 1024 * 1024) { findings.Add(en.FullName + " (oversized DLL)"); continue; }
                            using (var ms = new MemoryStream())
                            using (var es = en.Open())
                            {
                                es.CopyTo(ms);
                                Check(en.FullName, ms.ToArray(), findings);
                            }
                        }
                    }
                }
            }
            catch { }
            return findings;
        }

        static void Check(string name, byte[] img, List<string> findings)
        {
            var imports = GetImportedDlls(img);
            if (imports == null)
            {
                findings.Add(name + " (not a valid 64-bit DLL)");
                return;
            }
            var odd = imports.Where(d => !IsAllowed(d)).Distinct().ToList();
            if (odd.Count > 0)
                findings.Add(name + " (imports: " + string.Join(", ", odd) + ")");
        }
    }
}
