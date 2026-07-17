using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// PSX TIM -> PNG converter. Mirrors the game's own decoder
    /// (pc_port/src/hires_override.c parse_tim_to_rgba / bgr555_to_rgba) exactly,
    /// including the codebase's alpha rule — a whole 16-bit halfword of 0x0000 is
    /// transparent, every other value is fully opaque, and the STP bit is ignored
    /// (it governs blend mode, not visibility). So a PNG this produces matches what
    /// the game draws for that texture.
    ///
    /// MULTI-PALETTE TIMs: a paletted TIM carries ONE 4-/8-bit index sheet plus N
    /// CLUT ROWS (palette variants). A model draws different regions — a monster's
    /// head vs its body — through DIFFERENT rows over the same texels, so a single
    /// row-0 PNG can only ever recolour one region. This converter can decode any
    /// row, and <see cref="ConvertFileToPngSet"/> emits one PNG per row named to
    /// match the game's per-row loose-override probe ("NAME.TIM.pNN.png", see
    /// src/main/fsqueue_3.c). Single-row / 16bpp TIMs stay a single "NAME.png".
    /// </summary>
    public static class TimConverter
    {
        public static bool IsTimFile(string path)
        {
            return Path.GetExtension(path).Equals(".tim", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>Decode a TIM blob (CLUT row 0) to a 32bpp ARGB Bitmap.</summary>
        public static Bitmap DecodeToBitmap(byte[] data, out string error)
        {
            int rows;
            return DecodeToBitmap(data, 0, out rows, out error);
        }

        /// <summary>Decode a TIM blob at <paramref name="clutRow"/> to a 32bpp ARGB
        /// Bitmap; <paramref name="clutRows"/> reports the TIM's total CLUT row count
        /// (1 for 16/24bpp). Returns null (with error) on failure.</summary>
        public static Bitmap DecodeToBitmap(byte[] data, int clutRow, out int clutRows, out string error)
        {
            byte[] rgba;
            int w, h;
            if (!DecodeToRgba(data, clutRow, out rgba, out w, out h, out clutRows, out error))
                return null;

            var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
            var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            try
            {
                // rgba is R,G,B,A; a 32bpp-ARGB bitmap's memory is little-endian B,G,R,A.
                var rowBuf = new byte[w * 4];
                for (int y = 0; y < h; y++)
                {
                    int src = y * w * 4;
                    for (int x = 0; x < w; x++)
                    {
                        rowBuf[x * 4 + 0] = rgba[src + x * 4 + 2]; // B
                        rowBuf[x * 4 + 1] = rgba[src + x * 4 + 1]; // G
                        rowBuf[x * 4 + 2] = rgba[src + x * 4 + 0]; // R
                        rowBuf[x * 4 + 3] = rgba[src + x * 4 + 3]; // A
                    }
                    Marshal.Copy(rowBuf, 0, bd.Scan0 + y * bd.Stride, w * 4);
                }
            }
            finally { bmp.UnlockBits(bd); }
            return bmp;
        }

        private static void Bgr555ToRgba(ushort cx, byte[] outp, int o)
        {
            int r5 = cx & 0x1F;
            int g5 = (cx >> 5) & 0x1F;
            int b5 = (cx >> 10) & 0x1F;
            outp[o + 0] = (byte)((r5 << 3) | (r5 >> 2));
            outp[o + 1] = (byte)((g5 << 3) | (g5 >> 2));
            outp[o + 2] = (byte)((b5 << 3) | (b5 >> 2));
            outp[o + 3] = (cx == 0) ? (byte)0 : (byte)255;
        }

        /// <summary>Number of CLUT rows (palette variants) in a paletted TIM; 1 for
        /// 16/24bpp or an unparseable header.</summary>
        public static int ClutRowCount(byte[] data)
        {
            if (data == null || data.Length < 20) return 1;
            if (data[0] != 0x10 || data[1] != 0 || data[2] != 0 || data[3] != 0) return 1;
            uint flags = (uint)data[4] | ((uint)data[5] << 8) | ((uint)data[6] << 16) | ((uint)data[7] << 24);
            bool hasClut = ((flags >> 3) & 1) != 0;
            if (!hasClut) return 1;
            int p = 8;
            if (p + 12 > data.Length) return 1;
            int clutH = data[p + 10] | (data[p + 11] << 8);
            return clutH > 0 ? clutH : 1;
        }

        /// <summary>TIM -> RGBA8 (R,G,B,A byte order), CLUT row 0. Mirrors parse_tim_to_rgba.</summary>
        public static bool DecodeToRgba(byte[] data, out byte[] rgba, out int outW, out int outH, out string error)
        {
            int rows;
            return DecodeToRgba(data, 0, out rgba, out outW, out outH, out rows, out error);
        }

        /// <summary>TIM -> RGBA8 (R,G,B,A byte order) using CLUT row
        /// <paramref name="clutRow"/> (clamped to row 0 if out of range). Mirrors
        /// parse_tim_to_rgba's clutRow handling. <paramref name="clutRows"/> reports
        /// the TIM's total CLUT row count.</summary>
        public static bool DecodeToRgba(byte[] data, int clutRow, out byte[] rgba, out int outW, out int outH, out int clutRows, out string error)
        {
            rgba = null; outW = 0; outH = 0; clutRows = 1; error = null;
            int size = data.Length;
            if (size < 12) { error = "file too small to be a TIM"; return false; }
            if (data[0] != 0x10 || data[1] != 0 || data[2] != 0 || data[3] != 0) { error = "not a TIM (bad magic)"; return false; }

            uint flags = (uint)data[4] | ((uint)data[5] << 8) | ((uint)data[6] << 16) | ((uint)data[7] << 24);
            int bppCode = (int)(flags & 0x7);
            bool hasClut = ((flags >> 3) & 1) != 0;
            int srcBpp;
            switch (bppCode) { case 0: srcBpp = 4; break; case 1: srcBpp = 8; break; case 2: srcBpp = 16; break; case 3: srcBpp = 24; break; default: error = "unsupported bit depth"; return false; }

            int p = 8;
            int clutOff = -1, clutW = 0, clutH = 0;
            if (hasClut)
            {
                if (p + 12 > size) { error = "CLUT block truncated"; return false; }
                uint blockLen = (uint)data[p] | ((uint)data[p + 1] << 8) | ((uint)data[p + 2] << 16) | ((uint)data[p + 3] << 24);
                clutW = data[p + 8] | (data[p + 9] << 8);
                clutH = data[p + 10] | (data[p + 11] << 8);
                clutOff = p + 12;
                if (blockLen < 12 || p + (long)blockLen > size) { error = "CLUT block overruns file"; return false; }
                p += (int)blockLen;
            }
            clutRows = (clutH > 0) ? clutH : 1;
            if (clutRow < 0 || clutRow >= clutRows) clutRow = 0; // clamp, like the game

            if (p + 12 > size) { error = "pixel block truncated"; return false; }
            int pixCellW = data[p + 8] | (data[p + 9] << 8);
            int pixH = data[p + 10] | (data[p + 11] << 8);
            int pix = p + 12;

            int pixW;
            switch (srcBpp)
            {
                case 4: pixW = pixCellW * 4; break;
                case 8: pixW = pixCellW * 2; break;
                case 16: pixW = pixCellW; break;
                default: pixW = (pixCellW * 2) / 3; break; // 24
            }
            if (pixW <= 0 || pixH <= 0 || pixW > 8192 || pixH > 8192) { error = "unusable dimensions"; return false; }

            var outp = new byte[pixW * pixH * 4];

            if (srcBpp == 4 || srcBpp == 8)
            {
                if (clutOff < 0) { error = "paletted TIM without a CLUT"; return false; }
                int rowClutBase = clutOff + clutRow * clutW * 2; // start of the selected palette row
                int rowBytes = pixCellW * 2;
                for (int y = 0; y < pixH; y++)
                {
                    int row = pix + y * rowBytes;
                    for (int x = 0; x < pixW; x++)
                    {
                        int idx;
                        if (srcBpp == 4)
                        {
                            int byteIdx = x >> 1;
                            if (byteIdx >= rowBytes || row + byteIdx >= size) idx = 0;
                            else { byte b = data[row + byteIdx]; idx = ((x & 1) != 0) ? ((b >> 4) & 0xF) : (b & 0xF); }
                        }
                        else
                        {
                            if (x >= rowBytes || row + x >= size) idx = 0;
                            else idx = data[row + x];
                        }
                        if (idx >= clutW) idx = 0;
                        int ce = rowClutBase + idx * 2;
                        ushort cx = (ce + 1 < size) ? (ushort)(data[ce] | (data[ce + 1] << 8)) : (ushort)0;
                        Bgr555ToRgba(cx, outp, (y * pixW + x) * 4);
                    }
                }
            }
            else if (srcBpp == 16)
            {
                for (int y = 0; y < pixH; y++)
                {
                    int row = pix + y * pixCellW * 2;
                    for (int x = 0; x < pixW; x++)
                    {
                        int pp = row + x * 2;
                        ushort cx = (pp + 1 < size) ? (ushort)(data[pp] | (data[pp + 1] << 8)) : (ushort)0;
                        Bgr555ToRgba(cx, outp, (y * pixW + x) * 4);
                    }
                }
            }
            else // 24bpp: 3 bytes/pixel, row stride pixCellW*2, stored B,G,R same as the game path.
            {
                for (int y = 0; y < pixH; y++)
                {
                    int row = pix + y * pixCellW * 2;
                    for (int x = 0; x < pixW; x++)
                    {
                        int pp = row + x * 3;
                        int o = (y * pixW + x) * 4;
                        outp[o + 0] = (pp < size) ? data[pp] : (byte)0;
                        outp[o + 1] = (pp + 1 < size) ? data[pp + 1] : (byte)0;
                        outp[o + 2] = (pp + 2 < size) ? data[pp + 2] : (byte)0;
                        outp[o + 3] = 255;
                    }
                }
            }

            rgba = outp; outW = pixW; outH = pixH; return true;
        }

        /// <summary>Convert one .TIM file to a single .png (CLUT row 0). Kept for the
        /// single-image path; multi-palette TIMs should use
        /// <see cref="ConvertFileToPngSet"/>. Returns false (with error) on failure.</summary>
        public static bool ConvertFileToPng(string timPath, string pngPath, out string error)
        {
            error = null;
            try
            {
                byte[] data = File.ReadAllBytes(timPath);
                using (var bmp = DecodeToBitmap(data, out error))
                {
                    if (bmp == null) return false;
                    bmp.Save(pngPath, ImageFormat.Png);
                }
                return true;
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }

        /// <summary>
        /// Convert one .TIM to a PNG set beside it. A single-row / 16bpp TIM becomes
        /// one "STEM.png" (e.g. DOB.png). A multi-CLUT TIM becomes one PNG per palette
        /// row, "FULLNAME.pNN.png" (e.g. DOB.TIM.p00.png … DOB.TIM.p05.png) — the exact
        /// names the game probes for per-row loose overrides, so a modder edits the
        /// palette region they want and drops the files into gamedata/load/&lt;FOLDER&gt;/.
        /// Returns the files written (empty with <paramref name="error"/> on failure).
        /// </summary>
        public static List<string> ConvertFileToPngSet(string timPath, out string error)
        {
            error = null;
            var written = new List<string>();
            try
            {
                byte[] data = File.ReadAllBytes(timPath);
                int rows = ClutRowCount(data);
                string dir = Path.GetDirectoryName(timPath);
                if (string.IsNullOrEmpty(dir)) dir = ".";

                if (rows <= 1)
                {
                    string png = Path.Combine(dir, Path.GetFileNameWithoutExtension(timPath) + ".png");
                    using (var bmp = DecodeToBitmap(data, out error))
                    {
                        if (bmp == null) return written;
                        bmp.Save(png, ImageFormat.Png);
                    }
                    written.Add(png);
                    return written;
                }

                string full = Path.GetFileName(timPath); // e.g. DOB.TIM
                int pad = (rows > 100) ? 3 : 2;
                for (int r = 0; r < rows; r++)
                {
                    string png = Path.Combine(dir, full + ".p" + r.ToString("D" + pad) + ".png");
                    int rc;
                    using (var bmp = DecodeToBitmap(data, r, out rc, out error))
                    {
                        if (bmp == null) { written.Clear(); return written; }
                        bmp.Save(png, ImageFormat.Png);
                    }
                    written.Add(png);
                }
                return written;
            }
            catch (Exception ex) { error = ex.Message; written.Clear(); return written; }
        }

        public class BulkResult
        {
            public int Converted;    // .TIM files successfully converted (any number of PNGs each)
            public int Failed;
            public int Deleted;
            public int PngsWritten;  // total PNG files emitted (multi-palette TIMs emit several)
            public readonly List<string> Failures = new List<string>();
        }

        /// <summary>
        /// Recursively convert every *.TIM under <paramref name="folder"/> to a PNG set
        /// in place (single "FOO.png", or per-palette "FOO.TIM.pNN.png" for multi-CLUT
        /// TIMs). When <paramref name="deleteOriginals"/> is set, each successfully
        /// converted .TIM is deleted.
        /// </summary>
        public static BulkResult BulkConvert(string folder, bool deleteOriginals, Action<int, int, string> report)
        {
            var res = new BulkResult();
            var tims = new List<string>();
            foreach (var f in Directory.GetFiles(folder, "*.tim", SearchOption.AllDirectories))
                if (IsTimFile(f)) tims.Add(f); // guard the 8.3 "*.tim matches .tim*" wildcard quirk

            for (int i = 0; i < tims.Count; i++)
            {
                string tim = tims[i];
                if (report != null) report(i, tims.Count, Path.GetFileName(tim));

                string err;
                var written = ConvertFileToPngSet(tim, out err);
                if (written.Count > 0)
                {
                    res.Converted++;
                    res.PngsWritten += written.Count;
                    if (deleteOriginals)
                    {
                        try { File.Delete(tim); res.Deleted++; } catch { }
                    }
                }
                else
                {
                    res.Failed++;
                    res.Failures.Add(Path.GetFileName(tim) + ": " + err);
                }
            }
            return res;
        }
    }
}
