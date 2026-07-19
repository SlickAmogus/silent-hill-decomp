using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Character/monster CLUT texture authoring — the in-process C# twin of
    /// pc_port/tools/clut_tool.py.
    ///
    /// A paletted TIM is ONE 4-bit index sheet + N CLUT rows (palettes). A single
    /// character draws different body regions through DIFFERENT rows over that
    /// shared sheet (DOB uses 7 rows across 278 prims, HERO 7-of-15, DARIA 11), so
    /// no single pNN.png ever looks right — each is the whole sheet tinted by one
    /// palette. Which region uses which row is baked into every model primitive's
    /// CLUT word in the .ILM (row = (field_2 >> 6) - materialBaseClutY), so the true
    /// in-game look is reconstructable offline from the .ILM model + .TIM.
    ///
    ///   Compose : ILM + TIM              -> one correct reference PNG (edit this)
    ///   Split   : edited reference + ILM + TIM -> per-row NAME.TIM.pNN.png set
    ///
    /// The loose per-row PNG path uploads each row as full RGBA (no 16-colour
    /// re-quantise, see fsqueue_3.c / hires_override.c), so a Split output is not
    /// limited to 16 colours per region — the artist paints freely. Split is a
    /// per-row slice of the edited reference, dilated 1px so no drawn texel becomes
    /// a hole where a prim samples it.
    /// </summary>
    public static class ClutComposer
    {
        private struct Prim { public int[] U; public int[] V; public int Row; public bool Tri; }

        private static ushort U16(byte[] d, int o) { return (ushort)(d[o] | (d[o + 1] << 8)); }
        private static int U32(byte[] d, int o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24); }

        // ---- ILM parse: primitives with UVs + palette row -----------------------

        private static List<Prim> ParseIlm(byte[] d)
        {
            var prims = new List<Prim>();
            int matCount = d[3];
            int matPtr = U32(d, 4);
            int modelCount = d[8];
            int modelHdrs = U32(d, 0xC);

            var baseClutY = new int[Math.Max(1, matCount)];
            for (int i = 0; i < matCount; i++)
                baseClutY[i] = U16(d, matPtr + i * 24 + 0x10) >> 6;

            for (int m = 0; m < modelCount; m++)
            {
                int mh = modelHdrs + m * 16;
                int meshCount = d[mh + 8];
                int meshHdrs = U32(d, mh + 0xC);
                for (int k = 0; k < meshCount; k++)
                {
                    int msh = meshHdrs + k * 24;
                    int pc = d[msh];
                    int pp = U32(d, msh + 4);
                    for (int pi = 0; pi < pc; pi++)
                    {
                        int pb = pp + pi * 20;
                        int clut = U16(d, pb + 2);
                        int mat = (U16(d, pb + 6) >> 8) & 0x7F;
                        int bcy = (mat < baseClutY.Length) ? baseClutY[mat] : 0;
                        // Triangle prims set the 4th vertex index (field_C[3]) to 0xFF and leave
                        // UV3 garbage (0,0); rasterising them as a quad drags a streak to (0,0).
                        var pr = new Prim { U = new int[4], V = new int[4], Row = (clut >> 6) - bcy, Tri = d[pb + 0xF] == 0xFF };
                        int j = 0;
                        foreach (int o in new[] { 0, 4, 8, 0xA })
                        {
                            ushort uv = U16(d, pb + o);
                            pr.U[j] = uv & 0xFF; pr.V[j] = uv >> 8; j++;
                        }
                        prims.Add(pr);
                    }
                }
            }
            return prims;
        }

        // ---- rasterise primitives -> per-texel palette-row map ------------------

        private static void FillTri(int[] rowmap, int ax, int ay, int bx, int by, int cx, int cy, int row, int W, int H)
        {
            int minx = Math.Max(0, Math.Min(ax, Math.Min(bx, cx)));
            int maxx = Math.Min(W - 1, Math.Max(ax, Math.Max(bx, cx)));
            int miny = Math.Max(0, Math.Min(ay, Math.Min(by, cy)));
            int maxy = Math.Min(H - 1, Math.Max(ay, Math.Max(by, cy)));
            double det = (by - cy) * (double)(ax - cx) + (cx - bx) * (double)(ay - cy);
            if (det == 0) return;
            for (int y = miny; y <= maxy; y++)
                for (int x = minx; x <= maxx; x++)
                {
                    double w0 = ((by - cy) * (double)(x - cx) + (cx - bx) * (double)(y - cy)) / det;
                    double w1 = ((cy - ay) * (double)(x - cx) + (ax - cx) * (double)(y - cy)) / det;
                    double w2 = 1.0 - w0 - w1;
                    if (w0 >= -0.02 && w1 >= -0.02 && w2 >= -0.02) // slight over-cover closes cracks
                        rowmap[y * W + x] = row;
                }
        }

        private static int[] BuildRowMap(List<Prim> prims, int W, int H, int dilate)
        {
            var rm = new int[W * H];
            for (int i = 0; i < rm.Length; i++) rm[i] = -1;
            foreach (var p in prims)
            {
                FillTri(rm, p.U[0], p.V[0], p.U[1], p.V[1], p.U[2], p.V[2], p.Row, W, H);
                if (!p.Tri) // quad: second triangle, PSX FT4 winding v0v1v2 / v1v3v2
                    FillTri(rm, p.U[1], p.V[1], p.U[3], p.V[3], p.U[2], p.V[2], p.Row, W, H);
            }
            for (int d = 0; d < dilate; d++) rm = Dilate(rm, W, H);
            return rm;
        }

        /// <summary>Dilated row-map plus, for each texel the dilation ADDED, the covered texel it
        /// should borrow its colour from (-1 when the texel is genuinely covered).
        /// Straight dilation claims a texel just outside the UV island and then copies whatever the
        /// edited image holds there — editors and AI upscalers leave that opaque BLACK (an RGB export
        /// has no alpha at all), which ringed every island in black. Borrowing the adjacent covered
        /// texel extends the art outward instead: normal UV edge padding, which is what the dilation
        /// was always meant to achieve.</summary>
        private static int[] BuildRowMapBleed(List<Prim> prims, int W, int H, out int[] src)
        {
            int[] tru = BuildRowMap(prims, W, H, 0);
            var outp = (int[])tru.Clone();
            src = new int[W * H];
            for (int i = 0; i < src.Length; i++) src[i] = -1;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    int i = y * W + x;
                    if (tru[i] != -1) continue;
                    int s = -1;
                    if (x + 1 < W && tru[i + 1] != -1) s = i + 1;
                    else if (x > 0 && tru[i - 1] != -1) s = i - 1;
                    else if (y + 1 < H && tru[i + W] != -1) s = i + W;
                    else if (y > 0 && tru[i - W] != -1) s = i - W;
                    if (s >= 0) { outp[i] = tru[s]; src[i] = s; }
                }
            return outp;
        }

        private static int[] Dilate(int[] rm, int W, int H)
        {
            var outp = (int[])rm.Clone();
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                {
                    if (rm[y * W + x] != -1) continue;
                    if (x + 1 < W && rm[y * W + x + 1] != -1) { outp[y * W + x] = rm[y * W + x + 1]; continue; }
                    if (x > 0 && rm[y * W + x - 1] != -1) { outp[y * W + x] = rm[y * W + x - 1]; continue; }
                    if (y + 1 < H && rm[(y + 1) * W + x] != -1) { outp[y * W + x] = rm[(y + 1) * W + x]; continue; }
                    if (y > 0 && rm[(y - 1) * W + x] != -1) { outp[y * W + x] = rm[(y - 1) * W + x]; }
                }
            return outp;
        }

        private static string ResolveTim(string ilmPath, string timPath)
        {
            if (!string.IsNullOrEmpty(timPath)) return timPath;
            string cand = Path.ChangeExtension(ilmPath, ".TIM");
            if (File.Exists(cand)) return cand;
            cand = Path.ChangeExtension(ilmPath, ".tim");
            return File.Exists(cand) ? cand : null;
        }

        // ---- compose: ILM + TIM -> one correct reference PNG --------------------

        public static bool Compose(string ilmPath, string timPath, string outPng, out string error)
        {
            error = null;
            try
            {
                timPath = ResolveTim(ilmPath, timPath);
                if (timPath == null) { error = "no .TIM found beside " + Path.GetFileName(ilmPath); return false; }
                byte[] tim = File.ReadAllBytes(timPath);
                byte[] ilm = File.ReadAllBytes(ilmPath);

                int W, H, clutRows;
                byte[] row0;
                if (!TimConverter.DecodeToRgba(tim, 0, out row0, out W, out H, out clutRows, out error)) return false;

                var prims = ParseIlm(ilm);
                int[] rm = BuildRowMap(prims, W, H, 0);

                var rowRgba = new Dictionary<int, byte[]>();
                rowRgba[0] = row0;
                var comp = new byte[W * H * 4]; // R,G,B,A
                for (int i = 0; i < W * H; i++)
                {
                    int r = rm[i];
                    if (r < 0 || r >= clutRows) continue; // uncovered padding -> transparent
                    byte[] rr;
                    if (!rowRgba.TryGetValue(r, out rr))
                    {
                        int rw, rh, rc; string e;
                        if (!TimConverter.DecodeToRgba(tim, r, out rr, out rw, out rh, out rc, out e)) continue;
                        rowRgba[r] = rr;
                    }
                    Buffer.BlockCopy(rr, i * 4, comp, i * 4, 4);
                }

                using (var bmp = RgbaToBitmap(comp, W, H))
                    bmp.Save(outPng, ImageFormat.Png);
                return true;
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }

        // ---- split: edited reference + ILM + TIM -> per-row PNG set -------------

        public class SplitResult
        {
            public List<string> Written = new List<string>();
            public List<int> RowsUsed = new List<int>();
            public string Error;
            public int Width, Height;
        }

        public static SplitResult Split(string editedPng, string ilmPath, string timPath, string outDir)
        {
            var res = new SplitResult();
            try
            {
                timPath = ResolveTim(ilmPath, timPath);
                if (timPath == null) { res.Error = "no .TIM found beside " + Path.GetFileName(ilmPath); return res; }
                byte[] tim = File.ReadAllBytes(timPath);
                byte[] ilm = File.ReadAllBytes(ilmPath);

                int W, H, clutRows; byte[] r0;
                if (!TimConverter.DecodeToRgba(tim, 0, out r0, out W, out H, out clutRows, out res.Error)) return res;

                byte[] edit; int ew, eh;
                if (!LoadRgba(editedPng, out edit, out ew, out eh, out res.Error)) return res;
                res.Width = ew; res.Height = eh;
                // Native size OR an HD upscale of the same aspect ratio; the runtime scales each
                // per-row PNG, so HD detail is preserved. Exact multiples (2x/4x/8x) look sharpest.
                if (ew < W || eh < H || Math.Abs((long)ew * H - (long)eh * W) > (long)ew * H / 25)
                {
                    res.Error = string.Format(
                        "Edited image is {0}x{1}. It must be the sheet's native {2}x{3} or an upscale " +
                        "keeping that aspect ratio (e.g. {4}x{5} or {6}x{7}). For sharpest region edges " +
                        "use an exact multiple.", ew, eh, W, H, W * 2, H * 2, W * 4, H * 4);
                    return res;
                }

                var prims = ParseIlm(ilm);
                int[] srcm;
                int[] rm = BuildRowMapBleed(prims, W, H, out srcm); // dilate so no drawn texel becomes a hole

                var used = new SortedSet<int>();
                foreach (int r in rm) if (r >= 0) used.Add(r);
                res.RowsUsed = new List<int>(used);

                var emit = new SortedSet<int>(used) { 0 }; // p00 is the runtime's per-row sentinel
                string full = Path.GetFileName(timPath);
                if (!full.EndsWith(".TIM", StringComparison.OrdinalIgnoreCase))
                    full = Path.GetFileNameWithoutExtension(timPath) + ".TIM";
                int pad = (clutRows > 100) ? 3 : 2;
                Directory.CreateDirectory(outDir);

                // map each edited pixel to its native texel -> row, keeping the full HD pixels
                var sxm = new int[ew]; for (int x = 0; x < ew; x++) sxm[x] = x * W / ew;
                var sym = new int[eh]; for (int y = 0; y < eh; y++) sym[y] = y * H / eh;
                // First edited pixel belonging to each native texel, so a bled texel can copy the
                // matching pixel out of the texel it borrows from (any scale, integer or not).
                var xst = new int[W]; for (int t = 0; t < W; t++) xst[t] = (t * ew + W - 1) / W;
                var yst = new int[H]; for (int t = 0; t < H; t++) yst[t] = (t * eh + H - 1) / H;
                foreach (int r in emit)
                {
                    var buf = new byte[ew * eh * 4]; // transparent by default
                    for (int y = 0; y < eh; y++)
                    {
                        int ty = sym[y];
                        int rowBase = ty * W;
                        for (int x = 0; x < ew; x++)
                        {
                            int t = rowBase + sxm[x];
                            if (rm[t] != r) continue;
                            int o = (y * ew + x) * 4;
                            int s = srcm[t];
                            if (s < 0) { Buffer.BlockCopy(edit, o, buf, o, 4); continue; }
                            // Padding texel: take the pixel at the same offset inside the covered
                            // texel we borrow from, never the (usually black) background here.
                            int ux = xst[s % W] + (x - xst[sxm[x]]);
                            int uy = yst[s / W] + (y - yst[ty]);
                            if (ux >= ew) ux = ew - 1; if (ux < 0) ux = 0;
                            if (uy >= eh) uy = eh - 1; if (uy < 0) uy = 0;
                            Buffer.BlockCopy(edit, (uy * ew + ux) * 4, buf, o, 4);
                        }
                    }
                    string png = Path.Combine(outDir, full + ".p" + r.ToString("D" + pad) + ".png");
                    using (var bmp = RgbaToBitmap(buf, ew, eh))
                        bmp.Save(png, ImageFormat.Png);
                    res.Written.Add(png);
                }
                return res;
            }
            catch (Exception ex) { res.Error = ex.Message; return res; }
        }

        // ---- batch: build every character's reference composite under a folder --

        public class ComposeAllResult
        {
            public int Made;
            public int Failed;
            public readonly List<string> Failures = new List<string>();
        }

        /// <summary>Walk <paramref name="root"/> for every .ILM that has a matching .TIM beside it
        /// and write NAME_reference.png next to it. ILMs with no texture are skipped silently.</summary>
        public static ComposeAllResult ComposeAll(string root, Action<int, int, string> report)
        {
            var res = new ComposeAllResult();
            string[] ilms;
            try { ilms = Directory.GetFiles(root, "*.ilm", SearchOption.AllDirectories); }
            catch (Exception ex) { res.Failures.Add(ex.Message); return res; }
            for (int i = 0; i < ilms.Length; i++)
            {
                string ilm = ilms[i];
                if (!ilm.EndsWith(".ilm", StringComparison.OrdinalIgnoreCase)) continue; // 8.3 wildcard guard
                if (report != null) report(i, ilms.Length, Path.GetFileName(ilm));
                string tim = ResolveTim(ilm, null);
                if (tim == null) continue; // no texture beside it -> not a composable character
                string outPng = Path.Combine(Path.GetDirectoryName(ilm),
                    Path.GetFileNameWithoutExtension(ilm) + "_reference.png");
                string err;
                if (Compose(ilm, tim, outPng, out err)) res.Made++;
                else { res.Failed++; res.Failures.Add(Path.GetFileName(ilm) + ": " + err); }
            }
            return res;
        }

        // ---- bitmap <-> RGBA helpers (R,G,B,A byte order) -----------------------

        private static Bitmap RgbaToBitmap(byte[] rgba, int W, int H)
        {
            var bmp = new Bitmap(W, H, PixelFormat.Format32bppArgb);
            var bd = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            try
            {
                var rowBuf = new byte[W * 4];
                for (int y = 0; y < H; y++)
                {
                    int src = y * W * 4;
                    for (int x = 0; x < W; x++)
                    {
                        rowBuf[x * 4 + 0] = rgba[src + x * 4 + 2]; // B
                        rowBuf[x * 4 + 1] = rgba[src + x * 4 + 1]; // G
                        rowBuf[x * 4 + 2] = rgba[src + x * 4 + 0]; // R
                        rowBuf[x * 4 + 3] = rgba[src + x * 4 + 3]; // A
                    }
                    Marshal.Copy(rowBuf, 0, bd.Scan0 + y * bd.Stride, W * 4);
                }
            }
            finally { bmp.UnlockBits(bd); }
            return bmp;
        }

        private static bool LoadRgba(string path, out byte[] rgba, out int W, out int H, out string error)
        {
            rgba = null; W = 0; H = 0; error = null;
            try
            {
                using (var src = new Bitmap(path))
                using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(bmp)) g.DrawImageUnscaled(src, 0, 0);
                    W = bmp.Width; H = bmp.Height;
                    var bd = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                    try
                    {
                        rgba = new byte[W * H * 4];
                        var rowBuf = new byte[W * 4];
                        for (int y = 0; y < H; y++)
                        {
                            Marshal.Copy(bd.Scan0 + y * bd.Stride, rowBuf, 0, W * 4);
                            int dst = y * W * 4;
                            for (int x = 0; x < W; x++)
                            {
                                rgba[dst + x * 4 + 0] = rowBuf[x * 4 + 2]; // R (from B,G,R,A)
                                rgba[dst + x * 4 + 1] = rowBuf[x * 4 + 1]; // G
                                rgba[dst + x * 4 + 2] = rowBuf[x * 4 + 0]; // B
                                rgba[dst + x * 4 + 3] = rowBuf[x * 4 + 3]; // A
                            }
                        }
                    }
                    finally { bmp.UnlockBits(bd); }
                }
                return true;
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }
    }
}
