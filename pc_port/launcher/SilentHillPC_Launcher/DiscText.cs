using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The script text of a disc image, in any language it carries, keyed like the
    /// language packs ("MAP1_S00.23", "COMMON.5") so the Voices tool can show a
    /// line's words in the language a dub is being made for.
    ///
    /// Read the way the game reads it (lang_text.c): each map overlay's message
    /// table is the pointer word at file offset 0x34, linked for the region's PSX
    /// load base; the pointers are walked until one leaves the file. PAL carries
    /// German/French/Spanish/Italian in VIN2..VIN5 with the language digit in the
    /// overlay name. PAL pages that retail split are joined back onto the US index
    /// (the compiled map code numbers messages the US way), Japanese tables go
    /// through the same index maps the game uses, and a fan-patched disc gets its
    /// link base detected and its text decoded through the known Russian atlases.
    /// </summary>
    internal static class DiscText
    {
        public sealed class Language
        {
            public string Label;
            public int    Slot;      // 0 = VIN, 1..4 = VIN2..VIN5 (PAL); -1 = PC-side pack
            public string PackPath;  // when Slot == -1
            public override string ToString() { return Label; }
        }

        private const uint USA_OVL_BASE = 0x800C9578u;
        private const uint EUR_OVL_BASE = 0x800CB370u;
        private const uint JPN_OVL_BASE = 0x800CBBD0u;
        private const int  MSG_COUNT_MAX = 176;
        private const int  COMMON_COUNT = 15;

        // e_MapIdx order (lang_pack.c s_MapNames).
        public static readonly string[] MapNames = {
            "MAP0_S00", "MAP0_S01", "MAP0_S02",
            "MAP1_S00", "MAP1_S01", "MAP1_S02", "MAP1_S03", "MAP1_S04", "MAP1_S05", "MAP1_S06",
            "MAP2_S00", "MAP2_S01", "MAP2_S02", "MAP2_S03", "MAP2_S04",
            "MAP3_S00", "MAP3_S01", "MAP3_S02", "MAP3_S03", "MAP3_S04", "MAP3_S05", "MAP3_S06",
            "MAP4_S00", "MAP4_S01", "MAP4_S02", "MAP4_S03", "MAP4_S04", "MAP4_S05", "MAP4_S06",
            "MAP5_S00", "MAP5_S01", "MAP5_S02", "MAP5_S03",
            "MAP6_S00", "MAP6_S01", "MAP6_S02", "MAP6_S03", "MAP6_S04", "MAP6_S05",
            "MAP7_S00", "MAP7_S01", "MAP7_S02", "MAP7_S03",
            "MAPT_S00", "MAPX_S00"
        };

        // PAL pages retail split that the game joins back onto one US index
        // (lang_text.c s_MsgSplits): map name, slot, US index, parts.
        private static readonly object[,] Splits = {
            { "MAP1_S01", 1, 23, 2 }, { "MAP1_S01", 2, 23, 2 }, { "MAP1_S01", 4, 23, 3 },
            { "MAP1_S01", 4, 24, 2 }, { "MAP1_S01", 4, 25, 2 }, { "MAP1_S03", 3, 22, 2 },
            { "MAP5_S02", 4, 43, 2 },
        };

        private static readonly string[] PalLabels = { "English", "German", "French", "Spanish", "Italian" };

        /// <summary>The languages this disc can show: its own (one for USA/JAP and
        /// for fan patches, five for retail PAL) plus every PC-side pack in
        /// gamedata\lang.</summary>
        public static List<Language> LanguagesFor(DiscProbe.Disc disc, string gameRoot)
        {
            var list = new List<Language>();
            if (disc != null)
            {
                // A Russian repaint can leave the probe's sampled sectors alone, so the
                // atlas hash decides before the "modified" flag does.
                if (IsRussianPatch(disc.Path))
                {
                    list.Add(new Language { Label = "Russian (fan translation)", Slot = 0 });
                }
                else if (disc.Region == "PAL" && !disc.Modified)
                {
                    for (int i = 0; i < 5; i++) list.Add(new Language { Label = PalLabels[i], Slot = i });
                }
                else if (disc.Modified)
                {
                    list.Add(new Language { Label = "Disc text (fan translation)", Slot = 0 });
                }
                else
                {
                    list.Add(new Language { Label = disc.Region == "JAP" ? "Japanese" : "English", Slot = 0 });
                }
            }
            try
            {
                string dir = Path.Combine(Path.Combine(gameRoot, "gamedata"), "lang");
                if (Directory.Exists(dir))
                {
                    foreach (var p in Directory.GetFiles(dir, "*.lang"))
                    {
                        string name = Path.GetFileNameWithoutExtension(p);
                        foreach (var line in File.ReadLines(p))
                        {
                            if (line.StartsWith("!menu=", StringComparison.Ordinal)) { name = line.Substring(6).Trim(); break; }
                            if (line.Length > 0 && line[0] != '#' && line[0] != '!') break;
                        }
                        list.Add(new Language { Label = name + " (pack)", Slot = -1, PackPath = p });
                    }
                }
            }
            catch { }
            return list;
        }

        /// <summary>Key -> readable text for the language. Never throws: an
        /// unreadable disc yields an empty dictionary and an error message.</summary>
        public static Dictionary<string, string> Load(string binPath, DiscProbe.Disc disc, Language lang, out string error)
        {
            error = null;
            var texts = new Dictionary<string, string>();
            try
            {
                if (lang.Slot < 0) { LoadPack(lang.PackPath, texts); return texts; }
                LoadDisc(binPath, disc, lang.Slot, texts, out error);
            }
            catch (Exception ex)
            {
                error = ex.Message;
            }
            return texts;
        }

        // ---- PC-side pack (gamedata/lang/*.lang: KEY=engine text) ----------------

        private static void LoadPack(string path, Dictionary<string, string> texts)
        {
            foreach (var line in File.ReadLines(path, Encoding.UTF8))
            {
                if (line.Length == 0 || line[0] == '#' || line[0] == '!') continue;
                int eq = line.IndexOf('=');
                if (eq <= 0) continue;
                texts[line.Substring(0, eq)] = CleanEngine(line.Substring(eq + 1));
            }
        }

        /// <summary>Engine-format text (US overlays, packs): '_' is a space, ~X codes
        /// carry layout and timing.</summary>
        public static string CleanEngine(string t)
        {
            t = Regex.Replace(t, @"~J\d\([\d.]+\)", "");
            t = t.Replace("~N", " ");
            t = Regex.Replace(t, @"~[A-Za-z]\d*", "");
            t = t.Replace("\t", "").Replace("\n", " ").Replace('_', ' ');
            t = Regex.Replace(t, @"\s+", " ").Trim();
            return Regex.Replace(t, @" ([.,!?])", "$1");
        }

        /// <summary>PAL overlay text: codes in braces, real spaces, tabs as indent art.</summary>
        private static string CleanPal(string t)
        {
            t = Regex.Replace(t, @"\{[^}]*\}", " ");
            t = t.Replace("\t", "").Replace("\n", " ");
            t = Regex.Replace(t, @"\s+", " ").Trim();
            return Regex.Replace(t, @" ([.,!?])", "$1");
        }

        // ---- disc ------------------------------------------------------------------

        private static void LoadDisc(string binPath, DiscProbe.Disc disc, int slot, Dictionary<string, string> texts, out string error)
        {
            error = null;
            string tableErr;
            var files = BinExtractor.ReadFileTable(binPath, out tableErr);
            if (files == null) { error = tableErr; return; }
            var byPath = new Dictionary<string, BinExtractor.DiscFile>(StringComparer.OrdinalIgnoreCase);
            foreach (var f in files) byPath[f.FullPath] = f;

            bool isPal = disc.Region == "PAL";
            bool isJpn = disc.Region == "JAP";
            uint baseDefault = isJpn ? JPN_OVL_BASE : isPal ? EUR_OVL_BASE : USA_OVL_BASE;
            uint detectedBase = 0;
            Encoding latin = Encoding.GetEncoding(1252);
            Encoding sjis = null;
            if (isJpn) { try { sjis = Encoding.GetEncoding(932); } catch { sjis = null; } }
            RuCharsets.Charset? ru = DetectRussian(binPath, byPath);

            int mapsRead = 0;
            for (int mapIdx = 0; mapIdx < MapNames.Length; mapIdx++)
            {
                string name = MapNames[mapIdx];
                string path;
                if (slot > 0)
                    path = "VIN" + (slot + 1) + "/" + name.Substring(0, 6) + slot + name.Substring(7) + ".BIN";
                else
                    path = "VIN/" + name + ".BIN";
                BinExtractor.DiscFile entry;
                if (!byPath.TryGetValue(path, out entry)) continue;
                byte[] ovl = BinExtractor.ReadDiscFile(binPath, entry);
                if (ovl == null || ovl.Length < 0x40) continue;

                uint tablePsx = LE32(ovl, 0x34);
                uint ovlBase = baseDefault;
                if (CleanRun(ovl, tablePsx, ovlBase) < 8)
                {
                    if (detectedBase != 0 && CleanRun(ovl, tablePsx, detectedBase) >= 8)
                        ovlBase = detectedBase;
                    else
                    {
                        int bestRun = 0; uint best = ovlBase;
                        for (uint b = baseDefault - 0x2000; b <= baseDefault + 0x400; b += 4)
                        {
                            int run = CleanRun(ovl, tablePsx, b);
                            if (run > bestRun) { bestRun = run; best = b; }
                        }
                        if (bestRun >= 8) { ovlBase = best; detectedBase = best; }
                    }
                }
                if (tablePsx <= ovlBase || tablePsx - ovlBase >= (uint)ovl.Length) continue;
                uint tableOff = tablePsx - ovlBase;

                var src = new List<int>();
                for (int i = 0; i < MSG_COUNT_MAX + 8; i++)
                {
                    if (tableOff + (uint)(i + 1) * 4 > (uint)ovl.Length) break;
                    uint ptr = LE32(ovl, (int)tableOff + i * 4);
                    if (ptr <= ovlBase || ptr - ovlBase >= (uint)ovl.Length) break;
                    src.Add((int)(ptr - ovlBase));
                }
                if (src.Count < 4) continue;
                mapsRead++;

                string[] usText = new string[MSG_COUNT_MAX];
                if (isJpn)
                {
                    short[] idxMap;
                    JpnMsgMap.Maps.TryGetValue(name, out idxMap);
                    for (int us = 0; us < MSG_COUNT_MAX; us++)
                    {
                        int jp = idxMap != null ? (us < idxMap.Length ? idxMap[us] : -2) : (us < src.Count ? us : -2);
                        if (jp < 0 || jp >= src.Count) continue;
                        usText[us] = CleanEngine(DecodeBytes(ovl, src[jp], sjis, null));
                    }
                }
                else if (isPal)
                {
                    int s = 0;
                    for (int us = 0; us < MSG_COUNT_MAX && s < src.Count; us++)
                    {
                        int parts = us == 2 ? 2 : SplitParts(name, slot, us);
                        var sb = new StringBuilder();
                        for (int p = 0; p < parts && s < src.Count; p++, s++)
                        {
                            if (p > 0) sb.Append(' ');
                            sb.Append(CleanPal(DecodeBytes(ovl, src[s], ru != null ? null : latin, ru)));
                        }
                        usText[us] = sb.ToString();
                    }
                }
                else
                {
                    for (int us = 0; us < MSG_COUNT_MAX && us < src.Count; us++)
                        usText[us] = CleanEngine(DecodeBytes(ovl, src[us], ru != null ? null : latin, ru));
                }

                for (int us = 0; us < MSG_COUNT_MAX; us++)
                {
                    if (usText[us] == null) continue;
                    if (us < COMMON_COUNT)
                    {
                        if (mapIdx == 0) texts["COMMON." + us] = usText[us];
                    }
                    else texts[name + "." + us] = usText[us];
                }
            }
            if (mapsRead == 0) error = "No map text could be read from this disc.";
        }

        private static int SplitParts(string map, int slot, int usIdx)
        {
            for (int i = 0; i < Splits.GetLength(0); i++)
                if ((string)Splits[i, 0] == map && (int)Splits[i, 1] == slot && (int)Splits[i, 2] == usIdx)
                    return (int)Splits[i, 3];
            return 1;
        }

        private static uint LE32(byte[] b, int o)
        {
            return (uint)b[o] | ((uint)b[o + 1] << 8) | ((uint)b[o + 2] << 16) | ((uint)b[o + 3] << 24);
        }

        /// <summary>Consecutive clean messages the table resolves to at this base;
        /// the game's own test for a relinked (rebuilt) disc.</summary>
        private static int CleanRun(byte[] ovl, uint tablePsx, uint ovlBase)
        {
            if (tablePsx <= ovlBase || tablePsx - ovlBase >= (uint)ovl.Length) return 0;
            uint tableOff = tablePsx - ovlBase;
            int i;
            for (i = 0; i < 250; i++)
            {
                if (tableOff + (uint)(i + 1) * 4 > (uint)ovl.Length) break;
                uint ptr = LE32(ovl, (int)tableOff + i * 4);
                if (ptr <= ovlBase || ptr - ovlBase >= (uint)ovl.Length) break;
                int off = (int)(ptr - ovlBase);
                if (off != 0 && ovl[off - 1] != 0) break;
                int len = 0;
                bool clean = true;
                for (int p = off; p < ovl.Length && ovl[p] != 0; p++, len++)
                {
                    byte c = ovl[p];
                    if (c != 9 && c != 10 && c < 0x20) { clean = false; break; }
                }
                if (!clean || len < 1 || len > 600) break;
                if (i < 2 && len > 6) return 0;
            }
            return i;
        }

        /// <summary>One NUL-terminated overlay string as text: a Russian atlas
        /// mapping when the disc is a known patch, else the given encoding.</summary>
        private static string DecodeBytes(byte[] ovl, int off, Encoding enc, RuCharsets.Charset? ru)
        {
            int end = off;
            while (end < ovl.Length && ovl[end] != 0) end++;
            if (ru.HasValue)
            {
                var cs = ru.Value;
                var sb = new StringBuilder(end - off);
                for (int p = off; p < end; p++)
                {
                    byte c = ovl[p];
                    // Control codes stay Latin: "~J0(2.5)" up to the next blank, "{E}" to
                    // the brace, or their letters would decode as Cyrillic and survive
                    // the code stripper.
                    if (c == (byte)'~')
                    {
                        while (p < end && ovl[p] != (byte)' ' && ovl[p] != (byte)'\t' && ovl[p] != (byte)'\n') sb.Append((char)ovl[p++]);
                        p--;
                        continue;
                    }
                    if (c == (byte)'{')
                    {
                        while (p < end && ovl[p] != (byte)'}') sb.Append((char)ovl[p++]);
                        if (p < end) sb.Append('}');
                        continue;
                    }
                    if (c == (byte)'_' || c < 0x20 || c == (byte)' ')
                    {
                        sb.Append((char)c);
                        continue;
                    }
                    int hit = -1;
                    for (int i = 0; i < 33 && hit < 0; i++) if (cs.Lo[i] == c) hit = 33 + i;
                    for (int i = 0; i < 33 && hit < 0; i++) if (cs.Up[i] == c) hit = i;
                    if (hit < 0) sb.Append((char)c);
                    else sb.Append(hit < 33 ? RuCharsets.Letters[hit] : char.ToLowerInvariant(RuCharsets.Letters[hit - 33]));
                }
                return sb.ToString();
            }
            if (enc == null) enc = Encoding.GetEncoding(1252);
            return enc.GetString(ovl, off, end - off);
        }

        private static readonly Dictionary<string, bool> s_ruByDisc = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);

        /// <summary>Whether the image's FONT16.TIM is one of the known Russian repaints
        /// (one file-table read per image, remembered).</summary>
        public static bool IsRussianPatch(string binPath)
        {
            bool known;
            if (binPath == null) return false;
            if (s_ruByDisc.TryGetValue(binPath, out known)) return known;
            known = false;
            try
            {
                string err;
                var files = BinExtractor.ReadFileTable(binPath, out err);
                if (files != null)
                {
                    var byPath = new Dictionary<string, BinExtractor.DiscFile>(StringComparer.OrdinalIgnoreCase);
                    foreach (var f in files) byPath[f.FullPath] = f;
                    known = DetectRussian(binPath, byPath).HasValue;
                }
            }
            catch { }
            s_ruByDisc[binPath] = known;
            return known;
        }

        /// <summary>FNV-1a of the disc's FONT16.TIM against the known Russian repaints.</summary>
        private static RuCharsets.Charset? DetectRussian(string binPath, Dictionary<string, BinExtractor.DiscFile> byPath)
        {
            try
            {
                BinExtractor.DiscFile font;
                if (!byPath.TryGetValue("1ST/FONT16.TIM", out font)) return null;
                byte[] data = BinExtractor.ReadDiscFile(binPath, font);
                if (data == null) return null;
                uint h = 2166136261u;
                for (int i = 0; i < data.Length; i++) { h ^= data[i]; h *= 16777619u; }
                foreach (var cs in RuCharsets.All) if (cs.FontHash == h) return cs;
            }
            catch { }
            return null;
        }
    }
}
