// SPDX-License-Identifier: GPL-3.0-or-later
//
// Silent Hill memory card converter core: PSX / DuckStation / real card images
// <-> PC port gamedata/save/N.MCD cards. See pc_port/docs/Save_Format_And_Converter.md.
//
// Kept to C# 5 so Windows PowerShell 5.1's Add-Type can compile it at runtime
// (the drag-and-drop .bat) and the .NET 4.7.2 launcher can include it as-is.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace ShSaveCard
{
    public enum Region { Usa, Eur, Jpn }

    public class CardFile
    {
        public string Name;
        public byte[] Data;
        public int Blocks { get { return Data.Length / PsxCard.BlockSize; } }
    }

    public class PsxCard
    {
        public const int FrameSize = 128;
        public const int BlockSize = 8192;
        public const int CardSize = 16 * BlockSize;
        public const int DirCount = 15;
        const int GmeHeaderSize = 0xF40;
        const int VmpHeaderSize = 0x80;

        public readonly List<CardFile> Files = new List<CardFile>();
        public readonly List<string> Warnings = new List<string>();

        public static PsxCard Load(byte[] raw)
        {
            byte[] img = ExtractImage(raw);
            if (img == null)
                throw new InvalidDataException("Not a PlayStation memory card image (expected 128 KB raw .mcd/.mcr/.mc, or DexDrive .gme / PSP .vmp).");
            if (img[0] != 'M' || img[1] != 'C')
                throw new InvalidDataException("Memory card header \"MC\" missing; the image is unformatted or not a memory card.");

            PsxCard card = new PsxCard();
            for (int i = 1; i <= DirCount; i++)
            {
                int ofs = i * FrameSize;
                int attr = img[ofs];
                if (attr != 0x51) continue;
                string name = ReadName(img, ofs + 10);
                if (name.Length == 0) continue;

                // Chains: "next" holds the next block index minus one, 0xFFFF ends it.
                // PC-port cards write 0 here for single-block files, so a size
                // of one block ends the chain regardless of the link.
                int size = BitConverter.ToInt32(img, ofs + 4);
                int blocks = Math.Max(1, (size + BlockSize - 1) / BlockSize);
                List<int> chain = new List<int>();
                int cur = i;
                while (chain.Count < blocks && cur >= 1 && cur <= DirCount && !chain.Contains(cur))
                {
                    chain.Add(cur);
                    int next = BitConverter.ToUInt16(img, cur * FrameSize + 8);
                    cur = (next == 0xFFFF) ? -1 : next + 1;
                }
                if (chain.Count < blocks)
                {
                    card.Warnings.Add("File " + name + ": block chain is broken (" + chain.Count + " of " + blocks + " blocks), skipped.");
                    continue;
                }
                byte[] data = new byte[blocks * BlockSize];
                for (int b = 0; b < blocks; b++)
                    Buffer.BlockCopy(img, chain[b] * BlockSize, data, b * BlockSize, BlockSize);
                card.Files.Add(new CardFile { Name = name, Data = data });
            }
            return card;
        }

        static byte[] ExtractImage(byte[] raw)
        {
            int ofs;
            if (raw.Length == CardSize) ofs = 0;
            else if (raw.Length == CardSize + GmeHeaderSize && raw[0] == '1' && raw[1] == '2' && raw[2] == '3') ofs = GmeHeaderSize;
            else if (raw.Length == CardSize + VmpHeaderSize && raw[1] == 'P' && raw[2] == 'M' && raw[3] == 'V') ofs = VmpHeaderSize;
            else return null;
            byte[] img = new byte[CardSize];
            Buffer.BlockCopy(raw, ofs, img, 0, CardSize);
            return img;
        }

        static string ReadName(byte[] buf, int ofs)
        {
            int len = 0;
            while (len < 20 && buf[ofs + len] != 0) len++;
            return Encoding.ASCII.GetString(buf, ofs, len);
        }

        public int FreeBlocks
        {
            get
            {
                int used = 0;
                foreach (CardFile f in Files) used += f.Blocks;
                return DirCount - used;
            }
        }

        public CardFile Find(string name)
        {
            foreach (CardFile f in Files)
                if (f.Name == name) return f;
            return null;
        }

        /// Adds or replaces a file. Returns false when the card lacks free blocks.
        public bool Put(string name, byte[] data)
        {
            CardFile existing = Find(name);
            int need = data.Length / BlockSize;
            int avail = FreeBlocks + (existing != null ? existing.Blocks : 0);
            if (need > avail) return false;
            if (existing != null) existing.Data = data;
            else Files.Add(new CardFile { Name = name, Data = data });
            return true;
        }

        public void Remove(string name)
        {
            Files.RemoveAll(delegate(CardFile f) { return f.Name == name; });
        }

        /// Serialises a standard, fully checksummed card that real hardware,
        /// DuckStation and the PC port all read. Files are laid out contiguously
        /// from block 1, which the PC port requires (it reads a file's data at
        /// the block matching its directory slot).
        public byte[] ToImage()
        {
            byte[] img = new byte[CardSize];

            img[0] = (byte)'M';
            img[1] = (byte)'C';
            SealFrame(img, 0);

            int blk = 1;
            foreach (CardFile f in Files)
            {
                int n = f.Blocks;
                for (int b = 0; b < n; b++, blk++)
                {
                    int ofs = blk * FrameSize;
                    img[ofs] = (byte)(n == 1 || b == 0 ? 0x51 : (b == n - 1 ? 0x53 : 0x52));
                    if (b == 0) WriteU32(img, ofs + 4, (uint)(n * BlockSize));
                    WriteU16(img, ofs + 8, (ushort)(b == n - 1 ? 0xFFFF : blk));
                    if (b == 0)
                    {
                        byte[] nm = Encoding.ASCII.GetBytes(f.Name);
                        Buffer.BlockCopy(nm, 0, img, ofs + 10, Math.Min(nm.Length, 20));
                    }
                    SealFrame(img, blk);
                    Buffer.BlockCopy(f.Data, b * BlockSize, img, blk * BlockSize, BlockSize);
                }
            }
            for (; blk <= DirCount; blk++)
            {
                int ofs = blk * FrameSize;
                img[ofs] = 0xA0;
                WriteU16(img, ofs + 8, 0xFFFF);
                SealFrame(img, blk);
            }
            // Frames 16..35: empty broken-sector (relocation) list.
            for (int fr = 16; fr < 36; fr++)
            {
                int ofs = fr * FrameSize;
                WriteU32(img, ofs, 0xFFFFFFFF);
                WriteU16(img, ofs + 8, 0xFFFF);
                SealFrame(img, fr);
            }
            // Frames 36..62: relocation data, unused. Frame 63 mirrors frame 0 on
            // cards formatted by the console BIOS.
            for (int i = 36 * FrameSize; i < 63 * FrameSize; i++) img[i] = 0xFF;
            Buffer.BlockCopy(img, 0, img, 63 * FrameSize, FrameSize);
            return img;
        }

        static void SealFrame(byte[] img, int frame)
        {
            int ofs = frame * FrameSize;
            byte x = 0;
            for (int i = 0; i < FrameSize - 1; i++) x ^= img[ofs + i];
            img[ofs + FrameSize - 1] = x;
        }

        static void WriteU32(byte[] b, int o, uint v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); b[o + 2] = (byte)(v >> 16); b[o + 3] = (byte)(v >> 24); }
        static void WriteU16(byte[] b, int o, ushort v) { b[o] = (byte)v; b[o + 1] = (byte)(v >> 8); }
    }

    public class SaveSlot
    {
        public int Index;
        public bool Used;
        public bool Valid;
        public int TotalSaveCount;
        public int SaveCount;
        public int LocationId;
        public int Hours, Minutes, Seconds;
        public bool NextFear;

        static readonly string[] Locations = {
            "Anywhere", "Cafe", "Bus", "Store", "Infirmary", "Doghouse", "Gordon", "Church", "Garage",
            "Police", "Reception", "Room 302", "Director's Office", "Jewelry Shop", "Pool Hall",
            "Antique Shop", "Theme Park", "Boat", "Bridge", "Motel", "Lighthouse", "Sewer", "Nowhere",
            "Child's Room", "Next Fear" };

        public string LocationName
        {
            get { return LocationId >= 0 && LocationId < Locations.Length ? Locations[LocationId] : ("Location " + LocationId); }
        }

        public override string ToString()
        {
            return string.Format("slot {0,2}: save #{1,-3} {2,-18} {3}:{4:00}:{5:00}{6}{7}",
                Index + 1, SaveCount, LocationName, Hours, Minutes, Seconds,
                NextFear ? "  [Next Fear]" : "", Valid ? "" : "  [CHECKSUM BAD]");
        }
    }

    /// One Silent Hill save file (one 8 KB block). Layout, all little-endian:
    ///   0x000 PSX "SC" title block (title Shift-JIS, 16x16 4bpp icon)
    ///   0x200 s_MemCard_SaveHeader (256 B): per-slot metadata for 11 slots
    ///   0x300 s_Savegame_OptionsConfig (128 B)
    ///   0x380 11 x s_Savegame_Container (640 B each)
    /// Each 256/128/640 B record ends in {u8 xor, u8 xor, u16 0xDCDC}.
    public static class ShFile
    {
        public const int SlotCount = 11;
        public const int FileCountMax = 15;
        const int HeaderOfs = 0x200, ConfigOfs = 0x300, SlotsOfs = 0x380, SlotSize = 640;

        public static string Prefix(Region r)
        {
            switch (r)
            {
                case Region.Eur: return "BESLES-01514SILENT";
                case Region.Jpn: return "BISLPM-86192SILENT";
                default: return "BASLUS-00707SILENT";
            }
        }

        public static string RegionLabel(Region r)
        {
            switch (r)
            {
                case Region.Eur: return "Europe";
                case Region.Jpn: return "Japan";
                default: return "USA";
            }
        }

        /// Returns true and the region + file index (0..14) for Silent Hill save names.
        public static bool Parse(string name, out Region region, out int index)
        {
            region = Region.Usa;
            index = -1;
            foreach (Region r in new[] { Region.Usa, Region.Eur, Region.Jpn })
            {
                string p = Prefix(r);
                if (name.Length == p.Length + 2 && name.StartsWith(p, StringComparison.Ordinal)
                    && char.IsDigit(name[p.Length]) && char.IsDigit(name[p.Length + 1]))
                {
                    region = r;
                    index = (name[p.Length] - '0') * 10 + (name[p.Length + 1] - '0');
                    return index < FileCountMax;
                }
            }
            return false;
        }

        public static string MakeName(Region r, int index)
        {
            return Prefix(r) + index.ToString("00");
        }

        public static List<SaveSlot> ReadSlots(byte[] blk)
        {
            List<SaveSlot> list = new List<SaveSlot>();
            for (int s = 0; s < SlotCount; s++)
            {
                int m = HeaderOfs + 4 + s * 12;
                SaveSlot slot = new SaveSlot();
                slot.Index = s;
                slot.TotalSaveCount = BitConverter.ToInt32(blk, m);
                slot.Used = slot.TotalSaveCount != 0;
                uint timer = BitConverter.ToUInt32(blk, m + 4);
                slot.SaveCount = BitConverter.ToUInt16(blk, m + 8);
                slot.LocationId = blk[m + 10];
                slot.NextFear = (blk[m + 11] & 1) != 0;
                long secs = (timer >> 12) + (long)((blk[m + 11] >> 1) & 3) * 290 * 3600;
                slot.Hours = (int)(secs / 3600);
                slot.Minutes = (int)(secs / 60 % 60);
                slot.Seconds = (int)(secs % 60);
                slot.Valid = RecordValid(blk, SlotsOfs + s * SlotSize, SlotSize);
                if (slot.Used) list.Add(slot);
            }
            return list;
        }

        public static bool HeaderValid(byte[] blk) { return RecordValid(blk, HeaderOfs, 256); }
        public static bool ConfigValid(byte[] blk) { return RecordValid(blk, ConfigOfs, 128); }

        static bool RecordValid(byte[] blk, int ofs, int size)
        {
            int f = ofs + size - 4;
            if (blk[f + 2] != 0xDC || blk[f + 3] != 0xDC) return false;
            byte x = 0;
            // The game XORs the whole record with the checksum bytes zeroed.
            for (int i = ofs; i < ofs + size; i++)
                if (i != f && i != f + 1) x ^= blk[i];
            return x == blk[f];
        }

        /// Rewrites the PSX title block for the given region/index, and restores the
        /// icon when it is blank. Older PC builds wrote the title as UTF-8 (garbled
        /// on a console) and skipped the icon copy.
        public static void NormaliseTitleBlock(byte[] blk, Region r, int index)
        {
            blk[0] = (byte)'S';
            blk[1] = (byte)'C';
            blk[2] = 0x11;
            blk[3] = 1;
            for (int i = 4; i < 0x60; i++) blk[i] = 0;
            byte[] title = Title(r, index);
            Buffer.BlockCopy(title, 0, blk, 4, title.Length);

            bool blankIcon = true;
            for (int i = 0x60; i < 0x100; i++)
                if (blk[i] != 0) { blankIcon = false; break; }
            if (blankIcon)
                Buffer.BlockCopy(Icon, 0, blk, 0x60, Icon.Length);
        }

        static byte[] Title(Region r, int index)
        {
            List<byte> b = new List<byte>();
            if (r == Region.Jpn)
            {
                // サイレントヒル　ファイル
                b.AddRange(new byte[] { 0x83, 0x54, 0x83, 0x43, 0x83, 0x8C, 0x83, 0x93, 0x83, 0x67, 0x83, 0x71, 0x83, 0x8B,
                                        0x81, 0x40, 0x83, 0x74, 0x83, 0x40, 0x83, 0x43, 0x83, 0x8B });
            }
            else
            {
                foreach (char c in "SILENT HILL  FILE")
                {
                    if (c == ' ') { b.Add(0x81); b.Add(0x40); }
                    else { b.Add(0x82); b.Add((byte)(0x60 + (c - 'A'))); }
                }
            }
            int n = index + 1;
            b.Add(0x82); b.Add((byte)(0x4F + n / 10));
            b.Add(0x82); b.Add((byte)(0x4F + n % 10));
            return b.ToArray();
        }

        // Save icon from the retail disc (16-colour CLUT + 16x16 4bpp bitmap).
        static readonly byte[] Icon = HexBytes(
            "00804384628c6588a498a78cc89407a1ea9427a92c996ca18fa5f3a916ae37ae" +
            "0033556536110000103311011131030010c6ac581121130000fceeff1a112200" +
            "00eddefe3d10220010fdefee6d11220010a53a638a15220000a51c00a55c7400" +
            "00ecdecbde7cbb0000ecdddebc794b0000a8c8ac789904000085ca8b76990400" +
            "008155657777090000a0aa5844940b0000506a2422b46c000000300131cadc06");

        static byte[] HexBytes(string hex)
        {
            byte[] b = new byte[hex.Length / 2];
            for (int i = 0; i < b.Length; i++) b[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
            return b;
        }
    }

    public class ConvertResult
    {
        public PsxCard Card = new PsxCard();
        public readonly List<string> Log = new List<string>();
    }

    public static class Converter
    {
        /// PC port card slot for a PSX port/multitap position: 0..3 = port 1 A-D, 8..11 = port 2 A-D.
        public static readonly int[] PcCardNumbers = { 0, 1, 2, 3, 8, 9, 10, 11 };

        public static string PcCardLabel(int pcNumber)
        {
            int port = pcNumber >= 8 ? 2 : 1;
            char tap = (char)('A' + (pcNumber & 3));
            return (pcNumber & 3) == 0 ? ("Memory Card " + port) : ("Memory Card " + port + "-" + tap + " (multitap)");
        }

        public static bool IsPcCardFileName(string fileName)
        {
            string n = Path.GetFileName(fileName);
            if (!n.EndsWith(".MCD", StringComparison.OrdinalIgnoreCase)) return false;
            string stem = n.Substring(0, n.Length - 4);
            int v;
            return int.TryParse(stem, out v) && stem == v.ToString() && Array.IndexOf(PcCardNumbers, v) >= 0;
        }

        /// Collects the Silent Hill files of one or more source cards into a fresh card
        /// named for `target` region. Other games' files are ignored. Files keep their
        /// FILE number unless two collide, in which case later ones take the next free number.
        public static ConvertResult Gather(PsxCard[] sources, string[] sourceLabels, Region target, bool normaliseTitles)
        {
            ConvertResult res = new ConvertResult();
            bool[] taken = new bool[ShFile.FileCountMax];
            List<KeyValuePair<int, byte[]>> picked = new List<KeyValuePair<int, byte[]>>();

            for (int c = 0; c < sources.Length; c++)
            {
                PsxCard src = sources[c];
                foreach (string w in src.Warnings) res.Log.Add("  ! " + w);
                int other = 0;
                foreach (CardFile f in src.Files)
                {
                    Region r;
                    int idx;
                    if (!ShFile.Parse(f.Name, out r, out idx) || f.Blocks != 1) { other++; continue; }

                    int dst = idx;
                    if (taken[dst])
                    {
                        dst = Array.IndexOf(taken, false);
                        if (dst < 0)
                        {
                            res.Log.Add("  ! " + f.Name + " skipped: all 15 Silent Hill file numbers are already used on the output card.");
                            continue;
                        }
                    }
                    taken[dst] = true;

                    byte[] blk = (byte[])f.Data.Clone();
                    if (normaliseTitles || dst != idx || r != target)
                        ShFile.NormaliseTitleBlock(blk, target, dst);
                    picked.Add(new KeyValuePair<int, byte[]>(dst, blk));

                    string note = "";
                    if (r != target) note += " (" + ShFile.RegionLabel(r) + " save renamed for " + ShFile.RegionLabel(target) + ")";
                    if (dst != idx) note += " (FILE" + (idx + 1).ToString("00") + " renumbered to FILE" + (dst + 1).ToString("00") + ")";
                    res.Log.Add(string.Format("  {0}: {1} -> FILE{2:00}{3}", sourceLabels[c], f.Name, dst + 1, note));
                    if (!ShFile.HeaderValid(blk)) res.Log.Add("    ! save header checksum is bad; the game will report this file as damaged");
                    foreach (SaveSlot s in ShFile.ReadSlots(blk)) res.Log.Add("    " + s);
                }
                if (other > 0) res.Log.Add(string.Format("  {0}: {1} file(s) from other games left out", sourceLabels[c], other));
            }

            picked.Sort(delegate(KeyValuePair<int, byte[]> a, KeyValuePair<int, byte[]> b) { return a.Key.CompareTo(b.Key); });
            foreach (KeyValuePair<int, byte[]> p in picked)
                res.Card.Put(ShFile.MakeName(target, p.Key), p.Value);
            return res;
        }

        /// Adds the Silent Hill files of `adds` to a copy of `baseCard`. The base card's
        /// own saves (any game) are all kept; incoming Silent Hill files that collide
        /// with an existing FILE number are renumbered rather than overwriting it.
        public static ConvertResult Merge(PsxCard baseCard, string baseLabel, PsxCard[] adds, string[] addLabels, Region target, bool normaliseTitles)
        {
            List<PsxCard> all = new List<PsxCard>();
            List<string> labels = new List<string>();
            all.Add(baseCard);
            labels.Add(baseLabel);
            all.AddRange(adds);
            labels.AddRange(addLabels);
            ConvertResult sh = Gather(all.ToArray(), labels.ToArray(), target, normaliseTitles);

            ConvertResult res = new ConvertResult();
            res.Log.AddRange(sh.Log);
            int kept = 0;
            foreach (CardFile f in baseCard.Files)
            {
                Region r;
                int idx;
                if (ShFile.Parse(f.Name, out r, out idx) && f.Blocks == 1) continue;
                res.Card.Files.Add(new CardFile { Name = f.Name, Data = f.Data });
                kept++;
            }
            if (kept > 0) res.Log.Add(string.Format("  {0}: kept {1} file(s) from other games", baseLabel, kept));
            foreach (CardFile f in sh.Card.Files)
                if (!res.Card.Put(f.Name, f.Data))
                    res.Log.Add("  ! no room for " + f.Name + ": the card is full (15 blocks)");
            return res;
        }
    }
}
