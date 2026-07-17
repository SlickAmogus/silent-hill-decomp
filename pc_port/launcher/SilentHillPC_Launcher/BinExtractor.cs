using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Unpacks a Silent Hill PSX disc image (.bin) into the loose
    /// &lt;FOLDER&gt;/&lt;NAME&gt;.&lt;TYPE&gt; asset tree — the same output as the repo's two-stage
    /// dumpsxiso + tools/silentassets/extract.py, done in-process so end users need
    /// neither tool nor Python.
    ///
    /// Stage 1 (ISO split) reuses DiscProbe's raw-2352-byte-sector ISO9660 reader to
    /// pull the boot executable, SILENT. and HILL. straight out of the bin. Stage 2
    /// is a faithful port of extract.py: CRC32 the exe's first 4096 bytes to identify
    /// the release, parse its embedded file table, then slice each file out of the
    /// archives (SILENT. = 2048-byte Mode2/Form1 sectors; HILL. = 2336-byte XA
    /// sectors), applying the disc's oddities — XOR-encrypted 1ST/*.BIN overlays,
    /// LZSS-compressed HP_SAFE1/S__SAFE2, and .CMP -> .dec sidecars.
    ///
    /// The decomp-only filetable.c.inc / fileenum.h.inc outputs are intentionally
    /// omitted; a modder never needs them.
    /// </summary>
    public static class BinExtractor
    {
        // Release flags (extract.py Flags).
        private const uint FLAG_ENCRYPTED_1ST = 1;
        private const uint FLAG_NO_XA         = 2;
        private const uint FLAG_ALT_STRUCT    = 4;

        private const int FILESIZE_STEP = 256;

        private sealed class Release
        {
            public string Id;
            public uint Checksum;
            public int TocOffset;
            public int FileCount;
            public string[] Dirs;
            public string[] Types;
            public uint Flags;
            public Release(string id, uint checksum, int toc, int count, string[] dirs, string[] types, uint flags)
            { Id = id; Checksum = checksum; TocOffset = toc; FileCount = count; Dirs = dirs; Types = types; Flags = flags; }
        }

        private static readonly string[] FILE_TYPES = {
            "TIM", "VAB", "BIN", "DMS", "ANM", "PLM", "IPD", "ILM",
            "TMD", "DAT", "KDT", "CMP", "TXT", "UU1", "UU2", ""
        };
        private static readonly string[] FILE_TYPES_DEMO = {
            "TIM", "VAB", "BIN", "ANM", "DMS", "PLM", "IPD", "ILM",
            "TMD", "DAT", "KDT", "CMP", "TXT", "UU1", "UU2", ""
        };
        private static readonly string[] FILE_TYPES_OPM16 = {
            "TIM", "VAB", "BIN", "ANM", "DMS", "PLM", "IPD", "ILM",
            "TMD", "KDT", "CMP", "UU1", "UU2", "UU3", "UU4", ""
        };

        private static readonly string[] DIRS_NTSC = {
            "1ST", "ANIM", "BG", "CHARA", "ITEM", "MISC", "SND", "TEST",
            "TIM", "VIN", "XA"
        };
        private static readonly string[] DIRS_PAL = {
            "1ST", "ANIM", "BG", "CHARA", "ITEM", "MISC", "SND", "TEST",
            "TIM", "VIN", "VIN2", "VIN3", "VIN4", "VIN5", "XA"
        };
        private static readonly string[] DIRS_DEMO = {
            "1ST", "ANIM", "BG", "CHARA", "ITEM", "MISC", "SND", "TIM",
            "VIN", "XA"
        };
        private static readonly string[] DIRS_OPM16 = {
            "1ST", "ANIM", "BG", "CHARA", "ITEM", "SND", "TEST", "TIM",
            "VIN", "XA"
        };

        // Ported verbatim from extract.py RELEASES (checksum = CRC32 of the exe's first 4096 bytes).
        private static readonly Release[] RELEASES = {
            new Release("NTSC OPM16 Demo 98-10-19 (SCUS-94278)", 0x2534D4BE, 0xAA90,  886, DIRS_OPM16, FILE_TYPES_OPM16, FLAG_ALT_STRUCT),
            new Release("NTSC-J Demo 98-11-06 (SLPM-80366)",     0x1BCE20E4, 0xB9B0,  849, DIRS_DEMO,  FILE_TYPES_DEMO,  FLAG_ALT_STRUCT),
            new Release("NTSC-J Demo 98-11-06 (SLPM-80363)",     0xBE8E95CF, 0xB780,  849, DIRS_DEMO,  FILE_TYPES_DEMO,  FLAG_ALT_STRUCT),
            new Release("PAL Demo 98-12-16 (SLED-01735)",        0x9AE067D4, 0xB648,  850, DIRS_DEMO,  FILE_TYPES_DEMO,  0),
            new Release("NTSC Demo 99-01-16 (SLUS-90050)",       0x55E85F78, 0xB648,  849, DIRS_DEMO,  FILE_TYPES_DEMO,  0),
            new Release("PAL Demo 99-06-08 (SLED-02190)",        0x08E0B752, 0xC8FC, 1015, DIRS_PAL,   FILE_TYPES,       FLAG_ENCRYPTED_1ST),
            new Release("PAL Demo 99-06-16 (SLED-02186)",        0x1782AA0A, 0xB8FC, 1015, DIRS_PAL,   FILE_TYPES,       FLAG_ENCRYPTED_1ST),
            new Release("NTSC Preview 98-11-24 (SLUS-45678)",    0x8B88326C, 0xB71C, 1926, DIRS_NTSC,  FILE_TYPES,       FLAG_NO_XA),
            new Release("NTSC Review 99-01-07 (SLUS-00707)",     0xCC454534, 0xB7B8, 2076, DIRS_NTSC,  FILE_TYPES,       FLAG_NO_XA),
            new Release("NTSC Trade Demo 99-01-17 (SLUS-80707)", 0xC9FFEF2A, 0xB850, 2040, DIRS_NTSC,  FILE_TYPES,       0),
            new Release("NTSC Beta 99-01-22 (SLUS-00707)",       0x84AB9750, 0xB850, 2072, DIRS_NTSC,  FILE_TYPES,       0),
            new Release("NTSC-J Rev 0 99-01-26 (SLPM-86192)",    0x1532C55C, 0xB91C, 2074, DIRS_NTSC,  FILE_TYPES,       FLAG_ENCRYPTED_1ST),
            new Release("NTSC 1.1 99-02-10 (SLUS-00707)",        0xCF9CD8E5, 0xB91C, 2074, DIRS_NTSC,  FILE_TYPES,       FLAG_ENCRYPTED_1ST),
            new Release("NTSC-J Rev 1/2 99-06-02 (SLPM-86192)",  0xEB733CAA, 0xB91C, 2074, DIRS_NTSC,  FILE_TYPES,       FLAG_ENCRYPTED_1ST),
            new Release("PAL 99-06-07 (SLES-01514)",             0x337E4A60, 0xB8FC, 2310, DIRS_PAL,   FILE_TYPES,       FLAG_ENCRYPTED_1ST),
        };

        public sealed class ExtractResult
        {
            public bool Ok;
            public string Error;
            public string ReleaseId;
            public int Files;
            public int Textures;         // TIMs converted to PNG (0 unless convertTimToPng)
            public int TexturesDeleted;  // original TIMs removed after conversion (0 unless deleteTimAfterConvert)
            public readonly List<string> Warnings = new List<string>();
        }

        private struct TableEntry
        {
            public string FullPath;   // "DIR/NAME.TYPE" (or "DIR/NAME" for XA)
            public string Type;       // "TIM", "BIN", ..., "" for XA
            public int Size;          // in 256-byte units (0 = size unknown)
            public int Lba;           // relative sector offset within the archive
        }

        // --- ISO9660 (raw 2352-byte sectors, 2048 data bytes at +24; matches DiscProbe / the game) ---

        private sealed class IsoFile
        {
            public long ExtentLba;
            public long DataLen;      // bytes, as declared in the directory record
        }

        private static uint LE32(byte[] b, int o)
        {
            return (uint)b[o] | ((uint)b[o + 1] << 8) | ((uint)b[o + 2] << 16) | ((uint)b[o + 3] << 24);
        }

        // Read one 2048-byte ISO logical sector (Mode2/Form1 user data lives at raw +24).
        private static bool ReadIsoSector(FileStream f, long lba, byte[] dst)
        {
            f.Seek(lba * 2352 + 24, SeekOrigin.Begin);
            return f.Read(dst, 0, 2048) == 2048;
        }

        // Walk the root directory, returning the first file whose (version-stripped,
        // upper-cased) name satisfies `match`.
        private static IsoFile FindRootFile(FileStream f, Func<string, bool> match)
        {
            var sec = new byte[2048];
            if (!ReadIsoSector(f, 16, sec)) return null; // PVD at LBA 16
            long rootLba = LE32(sec, 158);
            long rootLen = LE32(sec, 166);
            if (rootLba <= 0 || rootLen <= 0) return null;

            long sectors = (rootLen + 2047) / 2048;
            for (long s = 0; s < sectors; s++)
            {
                if (!ReadIsoSector(f, rootLba + s, sec)) break;
                for (int o = 0; o + 33 < 2048; )
                {
                    int recLen = sec[o];
                    if (recLen == 0) break; // no more records in this sector
                    int nameLen = sec[o + 32];
                    if (o + 33 + nameLen <= 2048 && nameLen >= 1)
                    {
                        string raw = Encoding.ASCII.GetString(sec, o + 33, nameLen);
                        string name = StripVersion(raw).ToUpperInvariant();
                        if (match(name))
                            return new IsoFile { ExtentLba = LE32(sec, o + 2), DataLen = LE32(sec, o + 10) };
                    }
                    o += recLen;
                }
            }
            return null;
        }

        private static string StripVersion(string n)
        {
            int semi = n.IndexOf(';');
            return semi >= 0 ? n.Substring(0, semi) : n;
        }

        // The boot exe is the ISO file named like a PSX serial: XXXX_NNN.NN.
        private static bool IsExeName(string n)
        {
            if (n.Length < 11 || n[4] != '_' || n[8] != '.') return false;
            for (int i = 0; i < 4; i++) if (n[i] < 'A' || n[i] > 'Z') return false;
            return char.IsDigit(n[5]) && char.IsDigit(n[6]) && char.IsDigit(n[7])
                && char.IsDigit(n[9]) && char.IsDigit(n[10]);
        }

        // Reads a file stored contiguously in the ISO as a logical stream, hopping
        // the per-sector header gaps: SILENT. = 2048 data at raw +24, HILL. = 2336
        // data (XA subheader..EDC) at raw +16.
        private sealed class ArchiveReader
        {
            private readonly FileStream _f;
            private readonly long _extentLba;
            private readonly int _dataOff;
            private readonly int _dataSize;
            public readonly long TotalLogical;

            public ArchiveReader(FileStream f, long extentLba, long isoDataLen, int dataOff, int dataSize)
            {
                _f = f; _extentLba = extentLba; _dataOff = dataOff; _dataSize = dataSize;
                // ISO records the size in 2048-byte blocks regardless of the sector's
                // real payload, so the raw sector count is isoDataLen/2048 (rounded up),
                // and the logical size is that many `dataSize`-byte sectors.
                long sectors = (isoDataLen + 2047) / 2048;
                TotalLogical = sectors * _dataSize;
            }

            public byte[] Read(long logicalPos, long count)
            {
                if (count < 0) count = 0;
                var buf = new byte[count];
                long done = 0;
                while (done < count)
                {
                    long lp = logicalPos + done;
                    long sector = lp / _dataSize;
                    int within = (int)(lp % _dataSize);
                    long rawOff = (_extentLba + sector) * 2352 + _dataOff + within;
                    int chunk = (int)Math.Min(_dataSize - within, count - done);
                    _f.Seek(rawOff, SeekOrigin.Begin);
                    int rd = _f.Read(buf, (int)done, chunk);
                    if (rd <= 0) break; // past end of image
                    done += rd;
                }
                if (done < count) Array.Resize(ref buf, (int)done);
                return buf;
            }
        }

        // --- CRC32 (zlib / CRC-32/ISO-HDLC, matching extract.py's zlib.crc32) ---

        private static readonly uint[] CrcTable = BuildCrcTable();
        private static uint[] BuildCrcTable()
        {
            var t = new uint[256];
            for (uint i = 0; i < 256; i++)
            {
                uint c = i;
                for (int k = 0; k < 8; k++)
                    c = ((c & 1) != 0) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            return t;
        }
        private static uint Crc32(byte[] data, int count)
        {
            uint c = 0xFFFFFFFF;
            int n = Math.Min(count, data.Length);
            for (int i = 0; i < n; i++)
                c = CrcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
            return c ^ 0xFFFFFFFF;
        }

        // --- disc oddities (ported from extract.py) ---

        private static byte[] DecryptOverlay(byte[] data)
        {
            var outp = (byte[])data.Clone();
            int n = outp.Length & ~3; // whole uint32 words only (sector data is always a multiple of 4)
            uint seed = 0;
            for (int i = 0; i < n; i += 4)
            {
                unchecked { seed = seed + 0x01309125u; seed = seed * 0x03a452f7u; }
                uint w = (uint)outp[i] | ((uint)outp[i + 1] << 8) | ((uint)outp[i + 2] << 16) | ((uint)outp[i + 3] << 24);
                w ^= seed;
                outp[i] = (byte)w; outp[i + 1] = (byte)(w >> 8); outp[i + 2] = (byte)(w >> 16); outp[i + 3] = (byte)(w >> 24);
            }
            return outp;
        }

        // LZSS (JP1.0 0x800CC924), used for HP_SAFE1/S__SAFE2 and .CMP files.
        private static byte[] DecompressLzss(byte[] data)
        {
            if (data.Length < 4) return new byte[0];
            long expected = LE32(data, 0); // unsigned; widened so a >2GB header can't go negative
            int inp = 4;
            var outp = new List<byte>(expected > 0 && expected < int.MaxValue ? (int)expected : 0);
            var window = new byte[4096];
            int wptr = 0xFEE;
            int tag = 0;

            while (inp < data.Length && expected > outp.Count)
            {
                tag >>= 1;
                if ((tag & 0x100) == 0)
                {
                    if (inp >= data.Length) break;
                    tag = data[inp] | 0xFF00;
                    inp++;
                }

                if ((tag & 1) != 0)
                {
                    if (inp >= data.Length) break;
                    byte b = data[inp]; inp++;
                    outp.Add(b);
                    window[wptr] = b; wptr = (wptr + 1) & 0xFFF;
                }
                else
                {
                    if (inp + 1 >= data.Length) break;
                    byte b1 = data[inp], b2 = data[inp + 1]; inp += 2;
                    int offset = b1 | ((b2 & 0xF0) << 4);
                    int length = (b2 & 0x0F) + 3;
                    for (int k = 0; k < length; k++)
                    {
                        byte b = window[offset];
                        outp.Add(b);
                        window[wptr] = b; wptr = (wptr + 1) & 0xFFF;
                        offset = (offset + 1) & 0xFFF;
                    }
                }
            }
            return outp.ToArray();
        }

        // --- file-table entry decode (ported from extract.py _parseEntry) ---

        private static bool DecodeEntry(uint meta, uint file1, uint file2, Release rel, out TableEntry entry)
        {
            entry = new TableEntry();
            int size, lba, dirIdx, typeIdx;
            string name;

            if ((rel.Flags & FLAG_ALT_STRUCT) == 0)
            {
                var sb = new StringBuilder(8);
                for (int shift = 4; shift < 28; shift += 6) sb.Append((char)(32 + ((file1 >> shift) & 63)));
                for (int shift = 0; shift < 24; shift += 6) sb.Append((char)(32 + ((file2 >> shift) & 63)));
                name = sb.ToString().Trim();
                size = (int)(meta >> 19);
                lba = (int)(meta & 0x7FFFF);
                dirIdx = (int)(file1 & 15);
                typeIdx = (int)((file2 >> 24) & 15);
            }
            else
            {
                int dir0 = (int)((meta >> 31) & 0x1);
                int dir1 = (int)(file1 & 0x7);
                ulong name0 = (file1 >> 3) & 0x1FFFFFFF;
                ulong name1 = file2 & 0x7FFFF;
                ulong nameBits = (name1 << 29) | name0;
                var sb = new StringBuilder(8);
                for (int shift = 0; shift < 48; shift += 6) sb.Append((char)(32 + (int)((nameBits >> shift) & 0x3F)));
                name = sb.ToString().Trim();
                size = (int)((meta >> 19) & 0xFFF);
                lba = (int)(meta & 0x7FFFF);
                dirIdx = (dir1 << 1) | dir0;
                typeIdx = (int)((file2 >> 19) & 0xFF);
            }

            if (dirIdx < 0 || dirIdx >= rel.Dirs.Length) return false;   // malformed / out of table
            if (typeIdx < 0 || typeIdx >= rel.Types.Length) return false;

            string dir = rel.Dirs[dirIdx];
            string type = rel.Types[typeIdx];
            string full = (type.Length == 0) ? dir + "/" + name : dir + "/" + name + "." + type;

            entry.FullPath = full;
            entry.Type = type;
            entry.Size = size;
            entry.Lba = lba;
            return true;
        }

        // --- public entry point ---

        /// <summary>
        /// Extract <paramref name="binPath"/> into <paramref name="outDir"/>. When
        /// <paramref name="convertTimToPng"/> is set, every extracted TIM also gets a
        /// same-named .png beside it; if <paramref name="deleteTimAfterConvert"/> is also
        /// set, the original .TIM is removed once its .png is written. <paramref name="report"/>
        /// receives (done, total, name).
        /// </summary>
        public static ExtractResult Extract(string binPath, string outDir, bool convertTimToPng, bool deleteTimAfterConvert, Action<int, int, string> report)
        {
            var res = new ExtractResult();
            try
            {
                using (var f = new FileStream(binPath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    IsoFile exeRec = FindRootFile(f, IsExeName);
                    if (exeRec == null) { res.Error = "No PSX boot executable found — is this a raw-sector Silent Hill .bin?"; return res; }
                    IsoFile silentRec = FindRootFile(f, n => n.StartsWith("SILENT", StringComparison.Ordinal));
                    IsoFile hillRec = FindRootFile(f, n => n.StartsWith("HILL", StringComparison.Ordinal));
                    if (silentRec == null) { res.Error = "SILENT. archive not found in the disc image."; return res; }

                    // Whole exe (small) — for CRC detection and the file-table parse.
                    var exeReader = new ArchiveReader(f, exeRec.ExtentLba, exeRec.DataLen, 24, 2048);
                    byte[] exe = exeReader.Read(0, exeRec.DataLen);

                    Release rel = Detect(Crc32(exe, 4096));
                    if (rel == null) { res.Error = string.Format("Unrecognized executable checksum ({0:X8}) — not a supported Silent Hill disc.", Crc32(exe, 4096)); return res; }
                    res.ReleaseId = rel.Id;

                    // Parse the table into SILENT. (all non-XA) and HILL. (XA) lists.
                    var silent = new List<TableEntry>();
                    var hill = new List<TableEntry>();
                    int pos = rel.TocOffset;
                    for (int i = 0; i < rel.FileCount; i++)
                    {
                        if (pos + 12 > exe.Length) break;
                        uint meta = LE32(exe, pos), file1 = LE32(exe, pos + 4), file2 = LE32(exe, pos + 8);
                        pos += 12;
                        TableEntry e;
                        if (!DecodeEntry(meta, file1, file2, rel, out e))
                        {
                            res.Warnings.Add(string.Format("Skipped malformed table entry #{0}", i));
                            continue;
                        }
                        if (e.FullPath.StartsWith("XA/", StringComparison.Ordinal)) hill.Add(e);
                        else silent.Add(e);
                    }

                    int total = silent.Count + hill.Count;
                    int done = 0;

                    var silentReader = new ArchiveReader(f, silentRec.ExtentLba, silentRec.DataLen, 24, 2048);
                    ExtractList(silent, outDir, silentReader, 2048, rel.Flags, convertTimToPng, deleteTimAfterConvert, res, ref done, total, report);

                    if ((rel.Flags & FLAG_NO_XA) == 0 && hillRec != null && hill.Count > 0)
                    {
                        var hillReader = new ArchiveReader(f, hillRec.ExtentLba, hillRec.DataLen, 16, 2336);
                        ExtractList(hill, outDir, hillReader, 2336, rel.Flags, false, false, res, ref done, total, report);
                    }
                    else if (hill.Count > 0)
                    {
                        res.Warnings.Add("XA audio tracks were not extracted (this release has no XA container).");
                    }
                }
                res.Ok = true;
            }
            catch (Exception ex)
            {
                res.Ok = false;
                res.Error = ex.Message;
            }
            return res;
        }

        private static Release Detect(uint checksum)
        {
            foreach (var r in RELEASES) if (r.Checksum == checksum) return r;
            return null;
        }

        private static void ExtractList(List<TableEntry> entries, string outDir, ArchiveReader reader,
                                        int sectorSize, uint flags, bool convertTimToPng, bool deleteTimAfterConvert,
                                        ExtractResult res, ref int done, int total, Action<int, int, string> report)
        {
            if (entries.Count == 0) return;
            int baseLba = entries[0].Lba;

            for (int index = 0; index < entries.Count; index++)
            {
                TableEntry e = entries[index];
                long logicalOff = (long)(e.Lba - baseLba) * sectorSize;

                long readSize;
                if (e.Size != 0 && e.Type.Length != 0)
                    readSize = (long)e.Size * FILESIZE_STEP;
                else if (index + 1 < entries.Count)
                    readSize = (long)(entries[index + 1].Lba - e.Lba) * sectorSize;
                else
                    readSize = reader.TotalLogical - logicalOff; // last file: to end of archive

                if (readSize < 0) readSize = 0;
                byte[] data = reader.Read(logicalOff, readSize);

                string outPath = Path.Combine(outDir, e.FullPath.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(outPath));

                if (e.Type == "BIN" && (flags & FLAG_ENCRYPTED_1ST) != 0)
                {
                    if (e.FullPath.StartsWith("1ST", StringComparison.Ordinal))
                        data = DecryptOverlay(data);
                    else if (e.FullPath.IndexOf("HP_SAFE1", StringComparison.Ordinal) >= 0 ||
                             e.FullPath.IndexOf("S__SAFE2", StringComparison.Ordinal) >= 0)
                        data = DecryptOverlay(DecompressLzss(data));
                }
                else if (e.Type == "CMP")
                {
                    try { File.WriteAllBytes(outPath + ".dec", DecompressLzss(data)); }
                    catch (Exception ex) { res.Warnings.Add(e.FullPath + " (.dec): " + ex.Message); }
                }

                File.WriteAllBytes(outPath, data);
                res.Files++;

                if (convertTimToPng && e.Type == "TIM")
                {
                    // Multi-CLUT TIMs (monsters/chara) emit one PNG per palette row
                    // (FOO.TIM.pNN.png); single-palette TIMs emit one FOO.png.
                    string err;
                    var pngs = TimConverter.ConvertFileToPngSet(outPath, out err);
                    if (pngs.Count > 0)
                    {
                        res.Textures++;
                        if (deleteTimAfterConvert)
                        {
                            try { File.Delete(outPath); res.TexturesDeleted++; } catch { }
                        }
                    }
                    else res.Warnings.Add(e.FullPath + " (PNG): " + err);
                }

                done++;
                if (report != null) report(done, total, e.FullPath);
            }
        }
    }
}
