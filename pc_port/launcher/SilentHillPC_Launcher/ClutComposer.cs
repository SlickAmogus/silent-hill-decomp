using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// CLUT texture authoring for EVERY texture class — the in-process C# twin of
    /// pc_port/tools/clut_tool.py.
    ///
    /// A paletted TIM is ONE 4-bit index sheet + N CLUT rows (palettes). A model draws
    /// different regions through DIFFERENT rows over that shared sheet (DOB uses 7 rows
    /// across 278 prims, HERO 14, an Old-Town chunk sheet 14), so no single pNN.png ever
    /// looks right — each is the whole sheet tinted by one palette. Which region uses
    /// which row is baked into every model primitive's CLUT word
    /// (row = (field_2 &gt;&gt; 6) - (material.field_10 &gt;&gt; 6)), so the true in-game
    /// look is reconstructable offline from the model + the .TIM.
    ///
    /// Models live in three containers carrying the SAME s_LmHeader:
    ///     .ILM  characters/monsters   LM header at offset 0
    ///     .PLM  weapons + BG globals  LM header at offset 0
    ///     .IPD  world geometry chunk  LM header at the u32 @ offset 4
    /// Every pointer inside the LM is relative to the LM HEADER, so an .IPD needs the
    /// header offset added to all of them.
    ///
    ///   Index   : tree                          -> TIM -&gt; model reference map
    ///   Compose : TIM or model (+ index)        -> one correct reference PNG (edit this)
    ///   Split   : edited reference + the same   -> per-row NAME.TIM.pNN.png set
    ///
    /// One TIM is shared by MANY models — a BG sheet is referenced by up to 118 .IPD
    /// chunks, and for 55 sheets no single chunk uses every palette row the corpus does,
    /// so the row map has to be UNIONed over the whole tree. That is what the index is for.
    ///
    /// The loose per-row PNG path uploads each row as full RGBA (no 16-colour re-quantise,
    /// see fsqueue_3.c / hires_override.c), so a Split output is not limited to 16 colours
    /// per region — the artist paints freely. Split is a per-row slice of the edited
    /// reference, dilated 1px so no drawn texel becomes a hole where a prim samples it.
    /// </summary>
    public static class ClutComposer
    {
        // pc_port/include/hires_override.h — the runtime only keeps this many CLUT rows
        // per pool slot, so a pNN.png with N >= this is never read. Never emit one.
        public const int HiresPoolMaxRows = 16;

        private const int LmMagic   = 0x30;   // '0'
        private const int LmVersion = 6;
        private const int IpdMagic  = 20;     // IPD_HEADER_MAGIC
        private const int SlackDen  = 50;     // barycentric over-cover of 1/50 (the historical -0.02)

        public const int    IndexVersion  = 1;
        public const string IndexFileName = "clut_index.json";

        internal struct Prim
        {
            public byte U0, V0, U1, V1, U2, V2, U3, V3;
            public short Row;
            public byte Mat;
            public bool Tri;
        }

        public class Material { public string Name; public int BaseClutY; public int Tpage; }

        private class Model
        {
            public string Path;
            public byte[] Data;
            public int Base;
            public string Kind;
            public bool TriSentinel;
        }

        private static ushort U16(byte[] d, int o) { return (ushort)(d[o] | (d[o + 1] << 8)); }
        private static int U32(byte[] d, int o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24); }

        // ---- LM container parse (.ILM / .PLM / .IPD) ----------------------------

        /// <summary>Locate the s_LmHeader inside any of the three containers. .ILM/.PLM
        /// hold it at offset 0; an .IPD stores a file-relative offset to it in
        /// s_IpdHeader::lmHdr (u32 @ 4).</summary>
        private static Model OpenModel(string path, byte[] data)
        {
            if (data == null) data = File.ReadAllBytes(path);
            string ext = Path.GetExtension(path ?? "").ToUpperInvariant();
            int baseOff;
            string kind;
            if (ext == ".IPD" || (ext != ".ILM" && ext != ".PLM" && data.Length > 0 && data[0] == IpdMagic))
            {
                if (data.Length == 0 || data[0] != IpdMagic)
                    throw new InvalidDataException("not an IPD (bad magic)");
                baseOff = U32(data, 4);
                kind = "IPD";
            }
            else
            {
                baseOff = 0;
                kind = (ext == ".PLM") ? "PLM" : "ILM";
            }
            if (baseOff < 0 || baseOff + 20 > data.Length || data[baseOff] != LmMagic)
                throw new InvalidDataException(string.Format("no LM header at offset 0x{0:X}", baseOff));
            int version = data[baseOff + 1];
            if (version != LmVersion)
                // v7 = the port's wide high-poly ILM: a u32 geometry blob with a different
                // primitive layout, not something this parser can walk.
                throw new InvalidDataException(string.Format("LM version {0}, expected {1}", version, LmVersion));
            // Only .ILM uses the 0xFF 4th-vertex-index triangle sentinel. World geometry
            // (.IPD/.PLM) is 100% quads and byte prim+0xF there is a REAL vertex index —
            // testing it would drop real quads. Verified over the whole corpus: 6109 hits,
            // every one in CHARA/TEST .ILM, zero in 431185 .IPD + 10735 .PLM prims.
            return new Model { Path = path, Data = data, Base = baseOff, Kind = kind, TriSentinel = (kind == "ILM") };
        }

        /// <summary>Material table only — cheap enough to run over the whole tree for indexing.</summary>
        private static List<Material> ParseLmMaterials(Model m)
        {
            byte[] d = m.Data;
            int b = m.Base;
            int matCount = d[b + 3];
            int matPtr = b + U32(d, b + 4);
            var mats = new List<Material>(matCount);
            for (int i = 0; i < matCount; i++)
            {
                int o = matPtr + i * 24;
                int n = 0;
                while (n < 8 && d[o + n] != 0) n++;
                var sb = new StringBuilder(n);
                for (int j = 0; j < n; j++) sb.Append(d[o + j] < 0x80 ? (char)d[o + j] : '?');
                mats.Add(new Material
                {
                    Name = sb.ToString().Trim(),
                    BaseClutY = U16(d, o + 0x10) >> 6,
                    Tpage = d[o + 0xE]
                });
            }
            return mats;
        }

        private static List<Prim> ParseLmPrims(Model m, List<Material> mats)
        {
            byte[] d = m.Data;
            int b = m.Base;
            int modelCount = d[b + 8];
            int modelHdrs = b + U32(d, b + 0xC);
            var prims = new List<Prim>();
            for (int mi = 0; mi < modelCount; mi++)
            {
                int mh = modelHdrs + mi * 16;
                int meshCount = d[mh + 8];
                int meshHdrs = b + U32(d, mh + 0xC);
                for (int k = 0; k < meshCount; k++)
                {
                    int msh = meshHdrs + k * 24;
                    int pc = d[msh];
                    int pp = b + U32(d, msh + 4);
                    for (int pi = 0; pi < pc; pi++)
                    {
                        int pb = pp + pi * 20;
                        int clut = U16(d, pb + 2);
                        int mat = (U16(d, pb + 6) >> 8) & 0x7F;
                        int bcy = (mat < mats.Count) ? mats[mat].BaseClutY : 0;
                        ushort a0 = U16(d, pb + 0), a1 = U16(d, pb + 4), a2 = U16(d, pb + 8), a3 = U16(d, pb + 0xA);
                        prims.Add(new Prim
                        {
                            U0 = (byte)(a0 & 0xFF), V0 = (byte)(a0 >> 8),
                            U1 = (byte)(a1 & 0xFF), V1 = (byte)(a1 >> 8),
                            U2 = (byte)(a2 & 0xFF), V2 = (byte)(a2 >> 8),
                            U3 = (byte)(a3 & 0xFF), V3 = (byte)(a3 >> 8),
                            Row = (short)((clut >> 6) - bcy),
                            Mat = (byte)mat,
                            // Character triangle prims set the 4th vertex index (field_C[3]) to
                            // 0xFF and leave UV3 as garbage (0,0); rasterising them as a quad
                            // drags a streak to (0,0).
                            Tri = m.TriSentinel && d[pb + 0xF] == 0xFF
                        });
                    }
                }
            }
            return prims;
        }

        // ---- rasterise primitives -> per-texel palette-row map ------------------

        private static void FillRect(int[] rm, int x0, int y0, int x1, int y1, int row, int W, int H)
        {
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W - 1) x1 = W - 1;
            if (y1 > H - 1) y1 = H - 1;
            if (x1 < x0 || y1 < y0) return;
            for (int y = y0; y <= y1; y++)
            {
                int o = y * W;
                for (int x = x0; x <= x1; x++) rm[o + x] = row;
            }
        }

        /// <summary>floor(a / b) for b &gt; 0 (C# '/' truncates toward zero; Python '//' floors).</summary>
        private static long FloorDiv(long a, long b)
        {
            long q = a / b;
            if (a % b != 0 && a < 0) q--;
            return q;
        }

        /// <summary>One edge of the scanline span solve. Covered where A*x + C*dy + K &gt;= 0;
        /// narrows [lo,hi] and returns false once the span is empty.</summary>
        private static bool Edge(long A, long C, long K, long dy, ref long lo, ref long hi)
        {
            long bb = C * dy + K;
            if (A == 0)
            {
                if (bb < 0) { lo = hi + 1; return false; }
                return true;
            }
            if (A > 0) { long v = -FloorDiv(bb, A); if (v > lo) lo = v; }
            else { long v = FloorDiv(bb, -A); if (v < hi) hi = v; }
            return lo <= hi;
        }

        /// <summary>Barycentric fill with a 1/50 over-cover (closes hairline cracks between
        /// prims). Solved per scanline rather than tested per texel: every edge function is
        /// linear in x, so the covered span is an interval intersection. Kept in exact
        /// integers — "w/det &gt;= -1/50" is "50*sgn*w + |det| &gt;= 0" — so the boundary
        /// never depends on float rounding.</summary>
        private static void FillTri(int[] rm, int ax, int ay, int bx, int by, int cx, int cy, int row, int W, int H)
        {
            long det = (long)(by - cy) * (ax - cx) + (long)(cx - bx) * (ay - cy);
            if (det == 0) return;
            int miny = Math.Max(0, Math.Min(ay, Math.Min(by, cy)));
            int maxy = Math.Min(H - 1, Math.Max(ay, Math.Max(by, cy)));
            int minx = Math.Max(0, Math.Min(ax, Math.Min(bx, cx)));
            int maxx = Math.Min(W - 1, Math.Max(ax, Math.Max(bx, cx)));
            if (maxy < miny || maxx < minx) return;

            long ad = det > 0 ? det : -det;
            long k = det > 0 ? SlackDen : -SlackDen;
            // w0 = A0*x + C0*(y-cy) - A0*cx; w1 likewise; w2 = det - w0 - w1.
            long A0 = by - cy, C0 = cx - bx;
            long A1 = cy - ay, C1 = ax - cx;
            long eA0 = k * A0, eC0 = k * C0, eK0 = -k * A0 * cx + ad;
            long eA1 = k * A1, eC1 = k * C1, eK1 = -k * A1 * cx + ad;
            long eA2 = -k * (A0 + A1), eC2 = -k * (C0 + C1), eK2 = k * ((A0 + A1) * cx + det) + ad;

            for (int y = miny; y <= maxy; y++)
            {
                long dy = y - cy;
                long lo = minx, hi = maxx;
                if (!Edge(eA0, eC0, eK0, dy, ref lo, ref hi)) continue;
                if (!Edge(eA1, eC1, eK1, dy, ref lo, ref hi)) continue;
                if (!Edge(eA2, eC2, eK2, dy, ref lo, ref hi)) continue;
                int o = y * W;
                for (long x = lo; x <= hi; x++) rm[o + (int)x] = row;
            }
        }

        private static void FillPrim(int[] rm, ref Prim p, int W, int H)
        {
            if (p.Tri) { FillTri(rm, p.U0, p.V0, p.U1, p.V1, p.U2, p.V2, p.Row, W, H); return; }

            int x0 = Math.Min(Math.Min(p.U0, p.U1), Math.Min(p.U2, p.U3));
            int x1 = Math.Max(Math.Max(p.U0, p.U1), Math.Max(p.U2, p.U3));
            int y0 = Math.Min(Math.Min(p.V0, p.V1), Math.Min(p.V2, p.V3));
            int y1 = Math.Max(Math.Max(p.V0, p.V1), Math.Max(p.V2, p.V3));
            // Axis-aligned rectangle fast path — an optimisation, not a behaviour change:
            // each of the two FT4 triangles spans three corners, so its integer bounding box
            // (which clips the fill) is the whole rect, and their slack-expanded union covers
            // the rect; clipped, that is exactly the rect. Needs the STRIP pairing (uv0/uv3
            // and uv1/uv2 diagonal) — with the other pairing the triangles leave a gap.
            if (x0 != x1 && y0 != y1 && p.U0 != p.U3 && p.V0 != p.V3 && IsCornerSet(ref p, x0, y0, x1, y1))
            {
                FillRect(rm, x0, y0, x1, y1, p.Row, W, H);
                return;
            }
            FillTri(rm, p.U0, p.V0, p.U1, p.V1, p.U2, p.V2, p.Row, W, H);
            FillTri(rm, p.U1, p.V1, p.U3, p.V3, p.U2, p.V2, p.Row, W, H);  // PSX FT4 winding
        }

        /// <summary>True when the four UVs are exactly the four corners of [x0,x1]x[y0,y1]
        /// (the Python set(uv) == {corners} test: all on a corner and all four distinct).</summary>
        private static bool IsCornerSet(ref Prim p, int x0, int y0, int x1, int y1)
        {
            if (!((p.U0 == x0 || p.U0 == x1) && (p.V0 == y0 || p.V0 == y1))) return false;
            if (!((p.U1 == x0 || p.U1 == x1) && (p.V1 == y0 || p.V1 == y1))) return false;
            if (!((p.U2 == x0 || p.U2 == x1) && (p.V2 == y0 || p.V2 == y1))) return false;
            if (!((p.U3 == x0 || p.U3 == x1) && (p.V3 == y0 || p.V3 == y1))) return false;
            if (p.U0 == p.U1 && p.V0 == p.V1) return false;
            if (p.U0 == p.U2 && p.V0 == p.V2) return false;
            if (p.U0 == p.U3 && p.V0 == p.V3) return false;
            if (p.U1 == p.U2 && p.V1 == p.V2) return false;
            if (p.U1 == p.U3 && p.V1 == p.V3) return false;
            if (p.U2 == p.U3 && p.V2 == p.V3) return false;
            return true;
        }

        private static int[] BuildRowMap(List<Prim> prims, int W, int H, int dilate, out int clampedPrims)
        {
            var rm = new int[W * H];
            for (int i = 0; i < rm.Length; i++) rm[i] = -1;
            int clamped = 0;
            for (int i = 0; i < prims.Count; i++)
            {
                Prim p = prims[i];
                if (p.U0 >= W || p.U1 >= W || p.U2 >= W || p.U3 >= W ||
                    p.V0 >= H || p.V1 >= H || p.V2 >= H || p.V3 >= H)
                {
                    // Half-page materials (RSR08H, SPR07H, THR9002H, THRB502H) address past
                    // their 128-wide sheet into the neighbouring VRAM page. Clamp into the
                    // sheet and take the bounding box: the true footprint is a clip anyway,
                    // and clamping can make the triangles degenerate (which would leave holes).
                    clamped++;
                    int lim = W - 1, vlim = H - 1;
                    int u0 = Math.Min(p.U0, lim), u1 = Math.Min(p.U1, lim), u2 = Math.Min(p.U2, lim), u3 = Math.Min(p.U3, lim);
                    int v0 = Math.Min(p.V0, vlim), v1 = Math.Min(p.V1, vlim), v2 = Math.Min(p.V2, vlim), v3 = Math.Min(p.V3, vlim);
                    int mnu, mxu, mnv, mxv;
                    if (p.Tri)
                    {
                        mnu = Math.Min(u0, Math.Min(u1, u2)); mxu = Math.Max(u0, Math.Max(u1, u2));
                        mnv = Math.Min(v0, Math.Min(v1, v2)); mxv = Math.Max(v0, Math.Max(v1, v2));
                    }
                    else
                    {
                        mnu = Math.Min(Math.Min(u0, u1), Math.Min(u2, u3)); mxu = Math.Max(Math.Max(u0, u1), Math.Max(u2, u3));
                        mnv = Math.Min(Math.Min(v0, v1), Math.Min(v2, v3)); mxv = Math.Max(Math.Max(v0, v1), Math.Max(v2, v3));
                    }
                    FillRect(rm, mnu, mnv, mxu, mxv, p.Row, W, H);
                    continue;
                }
                FillPrim(rm, ref p, W, H);
            }
            for (int d = 0; d < dilate; d++) rm = Dilate(rm, W, H);
            clampedPrims = clamped;
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
            int clamped;
            int[] tru = BuildRowMap(prims, W, H, 0, out clamped);
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

        // ---- corpus index -------------------------------------------------------

        public class TimRef { public string Model; public int Mat; }

        /// <summary>The whole-tree TIM -&gt; model reference map. Same on-disk schema as
        /// clut_tool.py's clut_index.json (plus a "stamp" the Python tool ignores), so the
        /// two tools can share one file.</summary>
        public class ClutIndex
        {
            public string Root;
            public string Stamp;
            public readonly List<string> TimOrder = new List<string>();
            public readonly Dictionary<string, List<TimRef>> Tims = new Dictionary<string, List<TimRef>>(StringComparer.Ordinal);
            public readonly Dictionary<string, int> Unresolved = new Dictionary<string, int>(StringComparer.Ordinal);
            public readonly List<string[]> BadModels = new List<string[]>();
            public int Models;

            public int ReferencedCount
            {
                get { int n = 0; foreach (var kv in Tims) if (kv.Value.Count > 0) n++; return n; }
            }
        }

        private static readonly string[] ModelExts = { ".ILM", ".PLM", ".IPD" };

        private static string NormRoot(string root)
        {
            root = Path.GetFullPath(root);
            if (root.Length > 3 && (root[root.Length - 1] == Path.DirectorySeparatorChar ||
                                    root[root.Length - 1] == Path.AltDirectorySeparatorChar))
                root = root.Substring(0, root.Length - 1);
            return root;
        }

        private static string Rel(string root, string path)
        {
            string full = Path.GetFullPath(path);
            if (full.Length > root.Length + 1 && full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                full = full.Substring(root.Length + 1);
            return full.Replace('\\', '/');
        }

        private static List<string> WalkSorted(string root, string[] exts)
        {
            var hits = new List<string>();
            foreach (string f in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
            {
                string e = Path.GetExtension(f).ToUpperInvariant();
                for (int i = 0; i < exts.Length; i++)
                    if (e == exts[i]) { hits.Add(f); break; }
            }
            hits.Sort(StringComparer.Ordinal);
            return hits;
        }

        /// <summary>Walk the tree once: every model material -&gt; the .TIM its NAME names.
        /// Resolving by material name (not by the model's file stem) is what makes BIRD.ILM
        /// find REBIRD.TIM, MAN.ILM find TEST/DEV.TIM, and every weapon .PLM find
        /// CHARA/HERO.TIM — the references that carry HERO's missing palette rows.</summary>
        public static ClutIndex BuildIndex(string root, Action<int, int, string> report)
        {
            root = NormRoot(root);
            var idx = new ClutIndex { Root = root };

            var timPaths = WalkSorted(root, new[] { ".TIM" });
            var byStem = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            foreach (string tp in timPaths)
            {
                string stem = Path.GetFileNameWithoutExtension(tp).ToUpperInvariant();
                List<string> l;
                if (!byStem.TryGetValue(stem, out l)) { l = new List<string>(); byStem[stem] = l; }
                l.Add(tp);
                string rel = Rel(root, tp);
                idx.TimOrder.Add(rel);
                idx.Tims[rel] = new List<TimRef>();
            }

            var models = WalkSorted(root, ModelExts);
            idx.Models = models.Count;
            for (int i = 0; i < models.Count; i++)
            {
                if (report != null && (i & 15) == 0) report(i, models.Count, Path.GetFileName(models[i]));
                List<Material> mats;
                try { mats = ParseLmMaterials(OpenModel(models[i], null)); }
                catch (Exception ex) { idx.BadModels.Add(new[] { Rel(root, models[i]), ex.Message }); continue; }

                string mdir = Path.GetDirectoryName(models[i]);
                string mrel = Rel(root, models[i]);
                for (int mi = 0; mi < mats.Count; mi++)
                {
                    string name = mats[mi].Name.ToUpperInvariant();
                    if (name.Length == 0) continue;
                    List<string> cands;
                    if (!byStem.TryGetValue(name, out cands))
                    {
                        int c;
                        idx.Unresolved.TryGetValue(name, out c);
                        idx.Unresolved[name] = c + 1;
                        continue;
                    }
                    // Same folder wins (BLOOD.TIM and TITLE.TIM each exist in 2-3 folders).
                    string pick = null;
                    foreach (string c in cands)
                        if (string.Equals(Path.GetDirectoryName(c), mdir, StringComparison.OrdinalIgnoreCase)) { pick = c; break; }
                    if (pick == null) pick = cands[0];
                    idx.Tims[Rel(root, pick)].Add(new TimRef { Model = mrel, Mat = mi });
                }
            }
            idx.Stamp = TreeStamp(root);
            return idx;
        }

        /// <summary>Cheap signature of every model/texture file under the tree; the cached
        /// index is reused only while this still matches.</summary>
        private static string TreeStamp(string root)
        {
            long n = 0, bytes = 0, ticks = 0;
            try
            {
                foreach (var fi in new DirectoryInfo(root).EnumerateFiles("*", SearchOption.AllDirectories))
                {
                    string e = Path.GetExtension(fi.Name).ToUpperInvariant();
                    if (e != ".TIM" && e != ".ILM" && e != ".PLM" && e != ".IPD") continue;
                    n++;
                    bytes += fi.Length;
                    long t = fi.LastWriteTimeUtc.Ticks;
                    if (t > ticks) ticks = t;
                }
            }
            catch { return ""; }   // unreadable tree -> never matches, so the index rebuilds
            return n + ":" + bytes + ":" + ticks;
        }

        private static ClutIndex _cached;
        private static string _cachedRoot;

        /// <summary>The index for <paramref name="root"/>: reused from memory, else from the
        /// cached clut_index.json in the tree, else built and cached there. A read-only tree
        /// still works — the write is best-effort.</summary>
        public static ClutIndex EnsureIndex(string root, Action<int, int, string> report)
        {
            root = NormRoot(root);
            string stamp = TreeStamp(root);
            if (_cached != null && string.Equals(_cachedRoot, root, StringComparison.OrdinalIgnoreCase) &&
                _cached.Stamp == stamp && stamp.Length > 0)
                return _cached;

            string cachePath = Path.Combine(root, IndexFileName);
            if (stamp.Length > 0 && File.Exists(cachePath))
            {
                try
                {
                    var loaded = LoadIndex(cachePath);
                    if (loaded != null && loaded.Stamp == stamp)
                    {
                        loaded.Root = root;
                        _cached = loaded; _cachedRoot = root;
                        return loaded;
                    }
                }
                catch { }
            }

            var idx = BuildIndex(root, report);
            try { SaveIndex(idx, cachePath); } catch { }
            _cached = idx; _cachedRoot = root;
            return idx;
        }

        /// <summary>Drop the in-memory index (the next Reference/Rebuild re-reads or rebuilds it).</summary>
        public static void ForgetIndex() { _cached = null; _cachedRoot = null; }

        // ---- index serialisation (clut_tool.py-compatible JSON) -----------------

        public static void SaveIndex(ClutIndex idx, string path)
        {
            var sb = new StringBuilder(1 << 20);
            sb.Append("{\"version\": ").Append(IndexVersion);
            sb.Append(", \"root\": ").Append(JsonQuote(idx.Root));
            sb.Append(", \"stamp\": ").Append(JsonQuote(idx.Stamp ?? ""));
            sb.Append(", \"tims\": {");
            bool first = true;
            foreach (string rel in idx.TimOrder)
            {
                if (!first) sb.Append(", ");
                first = false;
                sb.Append(JsonQuote(rel)).Append(": {\"refs\": [");
                var refs = idx.Tims[rel];
                for (int i = 0; i < refs.Count; i++)
                {
                    if (i > 0) sb.Append(", ");
                    sb.Append('[').Append(JsonQuote(refs[i].Model)).Append(", ")
                      .Append(refs[i].Mat.ToString(CultureInfo.InvariantCulture)).Append(']');
                }
                sb.Append("]}");
            }
            sb.Append("}, \"unresolved\": {");
            first = true;
            foreach (var kv in idx.Unresolved)
            {
                if (!first) sb.Append(", ");
                first = false;
                sb.Append(JsonQuote(kv.Key)).Append(": ").Append(kv.Value.ToString(CultureInfo.InvariantCulture));
            }
            sb.Append("}, \"badModels\": [");
            for (int i = 0; i < idx.BadModels.Count; i++)
            {
                if (i > 0) sb.Append(", ");
                sb.Append('[').Append(JsonQuote(idx.BadModels[i][0])).Append(", ")
                  .Append(JsonQuote(idx.BadModels[i][1])).Append(']');
            }
            sb.Append("]}");
            File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        }

        public static ClutIndex LoadIndex(string path)
        {
            var root = JsonMini.Parse(File.ReadAllText(path)) as Dictionary<string, object>;
            if (root == null) return null;
            object v;
            if (!root.TryGetValue("version", out v) || (int)Convert.ToDouble(v) != IndexVersion) return null;

            var idx = new ClutIndex();
            idx.Root = root.TryGetValue("root", out v) ? (v as string) : null;
            idx.Stamp = root.TryGetValue("stamp", out v) ? (v as string ?? "") : "";

            var tims = root.TryGetValue("tims", out v) ? v as Dictionary<string, object> : null;
            if (tims == null) return null;
            foreach (var kv in tims)
            {
                var refs = new List<TimRef>();
                var entry = kv.Value as Dictionary<string, object>;
                object rv;
                if (entry != null && entry.TryGetValue("refs", out rv))
                {
                    var list = rv as List<object>;
                    if (list != null)
                        foreach (object o in list)
                        {
                            var pair = o as List<object>;
                            if (pair == null || pair.Count < 2) continue;
                            refs.Add(new TimRef { Model = pair[0] as string, Mat = (int)Convert.ToDouble(pair[1]) });
                        }
                }
                idx.TimOrder.Add(kv.Key);
                idx.Tims[kv.Key] = refs;
            }

            var unres = root.TryGetValue("unresolved", out v) ? v as Dictionary<string, object> : null;
            if (unres != null)
                foreach (var kv in unres) idx.Unresolved[kv.Key] = (int)Convert.ToDouble(kv.Value);

            var bad = root.TryGetValue("badModels", out v) ? v as List<object> : null;
            if (bad != null)
                foreach (object o in bad)
                {
                    var pair = o as List<object>;
                    if (pair != null && pair.Count >= 2)
                        idx.BadModels.Add(new[] { pair[0] as string, pair[1] as string });
                }
            return idx;
        }

        private static string JsonQuote(string s)
        {
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (char c in s)
            {
                if (c == '"' || c == '\\') { sb.Append('\\').Append(c); }
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c < 0x20 || c > 0x7E) sb.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
                else sb.Append(c);
            }
            sb.Append('"');
            return sb.ToString();
        }

        /// <summary>Just enough JSON to read a clut_index.json (objects, arrays, strings,
        /// numbers). Dependency-free so the index file stays interchangeable with the
        /// Python tool's.</summary>
        private static class JsonMini
        {
            public static object Parse(string s) { int p = 0; return Value(s, ref p); }

            private static void Ws(string s, ref int p)
            {
                while (p < s.Length && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
            }

            private static object Value(string s, ref int p)
            {
                Ws(s, ref p);
                char c = s[p];
                if (c == '{')
                {
                    p++;
                    var d = new Dictionary<string, object>(StringComparer.Ordinal);
                    Ws(s, ref p);
                    if (s[p] == '}') { p++; return d; }
                    while (true)
                    {
                        Ws(s, ref p);
                        string k = Str(s, ref p);
                        Ws(s, ref p);
                        p++;                       // ':'
                        d[k] = Value(s, ref p);
                        Ws(s, ref p);
                        if (s[p] == ',') { p++; continue; }
                        p++;                       // '}'
                        return d;
                    }
                }
                if (c == '[')
                {
                    p++;
                    var l = new List<object>();
                    Ws(s, ref p);
                    if (s[p] == ']') { p++; return l; }
                    while (true)
                    {
                        l.Add(Value(s, ref p));
                        Ws(s, ref p);
                        if (s[p] == ',') { p++; continue; }
                        p++;                       // ']'
                        return l;
                    }
                }
                if (c == '"') return Str(s, ref p);
                if (c == 't') { p += 4; return true; }
                if (c == 'f') { p += 5; return false; }
                if (c == 'n') { p += 4; return null; }
                int st = p;
                while (p < s.Length && "+-.eE0123456789".IndexOf(s[p]) >= 0) p++;
                return double.Parse(s.Substring(st, p - st), CultureInfo.InvariantCulture);
            }

            private static string Str(string s, ref int p)
            {
                p++;                               // opening quote
                var sb = new StringBuilder();
                while (s[p] != '"')
                {
                    char c = s[p++];
                    if (c != '\\') { sb.Append(c); continue; }
                    char e = s[p++];
                    switch (e)
                    {
                        case 'n': sb.Append('\n'); break;
                        case 'r': sb.Append('\r'); break;
                        case 't': sb.Append('\t'); break;
                        case 'b': sb.Append('\b'); break;
                        case 'f': sb.Append('\f'); break;
                        case 'u':
                            sb.Append((char)int.Parse(s.Substring(p, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture));
                            p += 4;
                            break;
                        default: sb.Append(e); break;
                    }
                }
                p++;
                return sb.ToString();
            }
        }

        // ---- prim collection ----------------------------------------------------

        /// <summary>Exact identity of a primitive for the union's duplicate drop: the eight UV
        /// bytes plus the palette row and the triangle flag.</summary>
        private struct PrimKey : IEquatable<PrimKey>
        {
            public long Uv;
            public int RowTri;
            public bool Equals(PrimKey o) { return Uv == o.Uv && RowTri == o.RowTri; }
            public override bool Equals(object o) { return o is PrimKey && Equals((PrimKey)o); }
            public override int GetHashCode() { return Uv.GetHashCode() * 31 + RowTri; }
        }

        /// <summary>UNION the primitives of every model that references <paramref name="timRel"/>.
        /// Prims keep their file order within a model and models are visited in sorted order, so
        /// the last-writer conflict resolution is deterministic; exact duplicates (the same UV rect
        /// drawn by dozens of chunk instances) are dropped, which is most of the corpus.</summary>
        private static List<Prim> CollectPrims(ClutIndex idx, string timRel, Dictionary<string, List<Prim>> cache,
                                               List<string> warnings)
        {
            var want = new SortedDictionary<string, List<int>>(StringComparer.Ordinal);
            List<TimRef> refs;
            if (!idx.Tims.TryGetValue(timRel, out refs)) return new List<Prim>();
            foreach (var r in refs)
            {
                List<int> l;
                if (!want.TryGetValue(r.Model, out l)) { l = new List<int>(); want[r.Model] = l; }
                l.Add(r.Mat);
            }

            var outp = new List<Prim>();
            var seen = new HashSet<PrimKey>();
            foreach (var kv in want)
            {
                List<Prim> prims;
                if (cache == null || !cache.TryGetValue(kv.Key, out prims))
                {
                    try
                    {
                        var mdl = OpenModel(Path.Combine(idx.Root, kv.Key.Replace('/', Path.DirectorySeparatorChar)), null);
                        prims = ParseLmPrims(mdl, ParseLmMaterials(mdl));
                    }
                    catch (Exception ex)
                    {
                        if (warnings != null) warnings.Add(kv.Key + ": " + ex.Message);
                        continue;
                    }
                    if (cache != null) cache[kv.Key] = prims;
                }
                for (int i = 0; i < prims.Count; i++)
                {
                    Prim p = prims[i];
                    if (!kv.Value.Contains(p.Mat)) continue;
                    // The row and the tri flag are part of the identity, not just the UVs.
                    var key = new PrimKey
                    {
                        Uv = ((long)p.U0 << 56) | ((long)p.V0 << 48) | ((long)p.U1 << 40) | ((long)p.V1 << 32) |
                             ((long)p.U2 << 24) | ((long)p.V2 << 16) | ((long)p.U3 << 8) | p.V3,
                        RowTri = (p.Row << 1) | (p.Tri ? 1 : 0)
                    };
                    if (!seen.Add(key)) continue;
                    outp.Add(p);
                }
            }
            return outp;
        }

        // ---- compose ------------------------------------------------------------

        public class ComposeResult
        {
            public string Path;
            public int Width, Height;
            public int Covered, Total;
            public int ClutRows;
            public int PrimCount;
            public int ClampedPrims;
            public int OutOfRangeTexels;
            public string Kind;                 // "composite" | "flat" | "flat-multirow"
            public readonly List<int> Rows = new List<int>();
            public double CoveragePct { get { return Total > 0 ? 100.0 * Covered / Total : 0.0; } }
        }

        /// <summary>Render one composite from an already-unioned prim list.</summary>
        private static ComposeResult ComposeTim(string timPath, List<Prim> prims, string outPng)
        {
            byte[] tim = File.ReadAllBytes(timPath);
            int W, H, clutRows;
            byte[] row0;
            string err;
            if (!TimConverter.DecodeToRgba(tim, 0, out row0, out W, out H, out clutRows, out err))
                throw new InvalidDataException(err);

            int clamped;
            int[] rm = BuildRowMap(prims, W, H, 0, out clamped);   // compose shows true coverage

            var res = new ComposeResult
            {
                Path = outPng, Width = W, Height = H, Total = W * H, ClutRows = clutRows,
                PrimCount = prims.Count, ClampedPrims = clamped, Kind = "composite"
            };

            var rowRgba = new Dictionary<int, byte[]>();
            rowRgba[0] = row0;
            var comp = new byte[W * H * 4];
            var used = new SortedSet<int>();
            for (int i = 0; i < W * H; i++)
            {
                int r = rm[i];
                if (r < 0) continue;
                used.Add(r);
                if (r >= clutRows) { res.OutOfRangeTexels++; continue; }  // left transparent
                res.Covered++;
                byte[] rr;
                if (!rowRgba.TryGetValue(r, out rr))
                {
                    int rw, rh, rc;
                    string e;
                    if (!TimConverter.DecodeToRgba(tim, r, out rr, out rw, out rh, out rc, out e)) continue;
                    rowRgba[r] = rr;
                }
                Buffer.BlockCopy(rr, i * 4, comp, i * 4, 4);
            }
            res.Rows.AddRange(used);

            string dir = Path.GetDirectoryName(outPng);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            using (var bmp = RgbaToBitmap(comp, W, H))
                bmp.Save(outPng, ImageFormat.Png);
            return res;
        }

        /// <summary>A TIM no model references is a flat 2D/UI sheet: its own decode IS the
        /// reference image, so emit that (row 0) and count it fully covered.</summary>
        private static ComposeResult ComposeFlat(string timPath, string outPng)
        {
            byte[] tim = File.ReadAllBytes(timPath);
            int W, H, clutRows;
            byte[] rgba;
            string err;
            if (!TimConverter.DecodeToRgba(tim, 0, out rgba, out W, out H, out clutRows, out err))
                throw new InvalidDataException(err);
            string dir = Path.GetDirectoryName(outPng);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            using (var bmp = RgbaToBitmap(rgba, W, H))
                bmp.Save(outPng, ImageFormat.Png);
            var res = new ComposeResult
            {
                Path = outPng, Width = W, Height = H, Covered = W * H, Total = W * H,
                ClutRows = clutRows, Kind = clutRows <= 1 ? "flat" : "flat-multirow"
            };
            res.Rows.Add(0);
            return res;
        }

        // ---- target resolution (a .TIM or a model file) -------------------------

        public class Target
        {
            public string TimPath;
            public string MaterialName;
            internal List<Prim> Prims;          // null => flat sheet, plain decode
            public bool IsFlat { get { return Prims == null || Prims.Count == 0; } }
        }

        /// <summary>material name -&gt; .TIM path. Sidecar folder first, then the whole index.</summary>
        private static string ResolveTimForModel(string modelPath, string name, ClutIndex idx)
        {
            string mdir = Path.GetDirectoryName(Path.GetFullPath(modelPath));
            if (string.IsNullOrEmpty(mdir)) mdir = ".";
            foreach (string ext in new[] { ".TIM", ".tim" })
            {
                string cand = Path.Combine(mdir, name + ext);
                if (File.Exists(cand)) return cand;
            }
            if (idx != null)
            {
                string up = name.ToUpperInvariant();
                foreach (string rel in idx.TimOrder)
                    if (string.Equals(Path.GetFileNameWithoutExtension(rel), up, StringComparison.OrdinalIgnoreCase))
                        return Path.Combine(idx.Root, rel.Replace('/', Path.DirectorySeparatorChar));
            }
            return null;
        }

        /// <summary>Everything a .TIM or a model file composes to. A .TIM resolves through the
        /// index (whole-corpus row union); a model resolves each of its materials to the .TIM
        /// its NAME names, preferring the index's union when that sheet is in it.</summary>
        public static List<Target> ResolveTargets(string path, ClutIndex idx, string timOverride, out string error)
        {
            error = null;
            var outp = new List<Target>();
            string ext = Path.GetExtension(path).ToUpperInvariant();

            if (ext == ".TIM")
            {
                var t = new Target { TimPath = path, MaterialName = Path.GetFileNameWithoutExtension(path) };
                if (idx != null)
                {
                    string rel = Rel(idx.Root, path);
                    if (idx.Tims.ContainsKey(rel)) t.Prims = CollectPrims(idx, rel, null, null);
                }
                outp.Add(t);                    // Prims empty => flat sheet, plain decode
                return outp;
            }

            Model mdl;
            List<Material> mats;
            List<Prim> prims;
            try
            {
                mdl = OpenModel(path, null);
                mats = ParseLmMaterials(mdl);
                prims = ParseLmPrims(mdl, mats);
            }
            catch (Exception ex) { error = ex.Message; return outp; }

            if (mats.Count == 0) { error = Path.GetFileName(path) + " declares no materials"; return outp; }

            for (int i = 0; i < mats.Count; i++)
            {
                string timPath = (!string.IsNullOrEmpty(timOverride) && mats.Count == 1)
                    ? timOverride : ResolveTimForModel(path, mats[i].Name, idx);
                if (timPath == null) continue;  // procedural material with no .TIM on disc
                var t = new Target { TimPath = timPath, MaterialName = mats[i].Name };
                if (idx != null)
                {
                    string rel = Rel(idx.Root, timPath);
                    List<TimRef> refs;
                    if (idx.Tims.TryGetValue(rel, out refs) && refs.Count > 0)
                    {
                        t.Prims = CollectPrims(idx, rel, null, null);
                        outp.Add(t);
                        continue;
                    }
                }
                var mine = new List<Prim>();
                for (int p = 0; p < prims.Count; p++) if (prims[p].Mat == i) mine.Add(prims[p]);
                t.Prims = mine;
                outp.Add(t);
            }
            if (outp.Count == 0) error = Path.GetFileName(path) + " has no material with a .TIM on disc";
            return outp;
        }

        /// <summary>Compose an already-resolved target to <paramref name="outPng"/>.</summary>
        public static ComposeResult ComposeTarget(Target t, string outPng)
        {
            return t.IsFlat ? ComposeFlat(t.TimPath, outPng) : ComposeTim(t.TimPath, t.Prims, outPng);
        }

        /// <summary>Compose one target (a .TIM or a model) to <paramref name="outPng"/>.
        /// <paramref name="idx"/> may be null — then only the given model is used, which is
        /// what the old single-.ILM path did.</summary>
        public static ComposeResult Compose(string target, ClutIndex idx, string timOverride, string outPng, out string error)
        {
            try
            {
                var targets = ResolveTargets(target, idx, timOverride, out error);
                if (targets.Count == 0) return null;
                return ComposeTarget(targets[0], outPng);
            }
            catch (Exception ex) { error = ex.Message; return null; }
        }

        /// <summary>Back-compat single-model compose (no index): model + .TIM -&gt; one PNG.</summary>
        public static bool Compose(string modelPath, string timPath, string outPng, out string error)
        {
            error = null;
            try
            {
                if (string.IsNullOrEmpty(timPath))
                {
                    timPath = Path.ChangeExtension(modelPath, ".TIM");
                    if (!File.Exists(timPath)) timPath = Path.ChangeExtension(modelPath, ".tim");
                    if (!File.Exists(timPath)) { error = "no .TIM found beside " + Path.GetFileName(modelPath); return false; }
                }
                return Compose(modelPath, null, timPath, outPng, out error) != null;
            }
            catch (Exception ex) { error = ex.Message; return false; }
        }

        // ---- split: edited reference -> per-row PNG set -------------------------

        public class SplitResult
        {
            public List<string> Written = new List<string>();
            public List<int> RowsUsed = new List<int>();
            public List<int> RowsDropped = new List<int>();
            public string Error;
            public int Width, Height;
            public string TimPath;
        }

        public static SplitResult Split(string editedPng, string target, ClutIndex idx, string timOverride, string outDir)
        {
            var res = new SplitResult();
            try
            {
                string err;
                var targets = ResolveTargets(target, idx, timOverride, out err);
                if (targets.Count == 0) { res.Error = err ?? "nothing to split"; return res; }
                if (targets.Count > 1)
                {
                    res.Error = Path.GetFileName(target) + " uses " + targets.Count +
                        " textures; split one at a time by picking the .TIM instead of the model.";
                    return res;
                }
                var t = targets[0];
                res.TimPath = t.TimPath;
                if (t.IsFlat)
                {
                    res.Error = Path.GetFileName(t.TimPath) + " is a flat 2D texture — no model draws it, so " +
                        "there are no palette rows to slice. Install your edit as a single " +
                        Path.GetFileNameWithoutExtension(t.TimPath) + ".png instead.";
                    return res;
                }

                byte[] tim = File.ReadAllBytes(t.TimPath);
                int W, H, clutRows;
                byte[] r0;
                if (!TimConverter.DecodeToRgba(tim, 0, out r0, out W, out H, out clutRows, out res.Error)) return res;

                byte[] edit;
                int ew, eh;
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

                int[] srcm;
                int[] rm = BuildRowMapBleed(t.Prims, W, H, out srcm); // dilate so no drawn texel becomes a hole

                var all = new SortedSet<int>();
                foreach (int r in rm) if (r >= 0) all.Add(r);
                // The runtime keeps HIRES_POOL_MAX_ROWS CLUT rows per slot and never probes past
                // them, so a pNN.png with N >= 16 can only ever be dead weight in a mod archive.
                foreach (int r in all)
                {
                    if (r >= HiresPoolMaxRows) res.RowsDropped.Add(r);
                    else res.RowsUsed.Add(r);
                }

                var emit = new SortedSet<int>(res.RowsUsed) { 0 };   // p00 is the runtime's per-row sentinel
                string full = Path.GetFileName(t.TimPath);
                if (!full.EndsWith(".TIM", StringComparison.OrdinalIgnoreCase))
                    full = Path.GetFileNameWithoutExtension(t.TimPath) + ".TIM";
                Directory.CreateDirectory(outDir);

                // map each edited pixel to its native texel -> row, keeping the full HD pixels
                var sxm = new int[ew]; for (int x = 0; x < ew; x++) sxm[x] = x * W / ew;
                var sym = new int[eh]; for (int y = 0; y < eh; y++) sym[y] = y * H / eh;
                // First edited pixel belonging to each native texel, so a bled texel can copy the
                // matching pixel out of the texel it borrows from (any scale, integer or not).
                var xst = new int[W]; for (int i = 0; i < W; i++) xst[i] = (i * ew + W - 1) / W;
                var yst = new int[H]; for (int i = 0; i < H; i++) yst[i] = (i * eh + H - 1) / H;
                foreach (int r in emit)
                {
                    var buf = new byte[ew * eh * 4]; // transparent by default
                    for (int y = 0; y < eh; y++)
                    {
                        int ty = sym[y];
                        int rowBase = ty * W;
                        for (int x = 0; x < ew; x++)
                        {
                            int tx = rowBase + sxm[x];
                            if (rm[tx] != r) continue;
                            int o = (y * ew + x) * 4;
                            int s = srcm[tx];
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
                    string png = Path.Combine(outDir, full + ".p" + r.ToString("D2") + ".png");
                    using (var bmp = RgbaToBitmap(buf, ew, eh))
                        bmp.Save(png, ImageFormat.Png);
                    res.Written.Add(png);
                }
                return res;
            }
            catch (Exception ex) { res.Error = ex.Message; return res; }
        }

        // ---- compose-all --------------------------------------------------------

        public class ClassStat
        {
            public int Tims, MultiClut, Full, NoRowMap;
            public long Covered, Total;
            public double CoveragePct { get { return Total > 0 ? 100.0 * Covered / Total : 0.0; } }
        }

        public class ComposeAllResult
        {
            public int Made;            // geometry-referenced composites
            public int Flat;            // plain decodes (no model references the sheet)
            public int Failed;
            public bool Cancelled;
            public readonly List<string> Failures = new List<string>();
            public readonly SortedDictionary<string, ClassStat> Classes =
                new SortedDictionary<string, ClassStat>(StringComparer.Ordinal);
            public long Covered, Total;
            public double CoveragePct { get { return Total > 0 ? 100.0 * Covered / Total : 0.0; } }
        }

        private static string ClassOf(string rel)
        {
            int i = rel.IndexOf('/');
            return i < 0 ? "." : rel.Substring(0, i);
        }

        /// <summary>Every TIM in the tree, in one pass. <paramref name="outDir"/> null writes
        /// NAME_reference.png beside each source .TIM; otherwise it mirrors the tree under
        /// outDir. Returns as soon as <paramref name="cancelled"/> goes true.</summary>
        public static ComposeAllResult ComposeAll(ClutIndex idx, string outDir,
                                                  Action<int, int, string> report, Func<bool> cancelled)
        {
            var res = new ComposeAllResult();
            var cache = new Dictionary<string, List<Prim>>(StringComparer.Ordinal);
            var rels = new List<string>(idx.TimOrder);
            rels.Sort(StringComparer.Ordinal);

            for (int i = 0; i < rels.Count; i++)
            {
                if (cancelled != null && cancelled()) { res.Cancelled = true; break; }
                string rel = rels[i];
                if (report != null && (i & 3) == 0) report(i, rels.Count, rel);

                string src = Path.Combine(idx.Root, rel.Replace('/', Path.DirectorySeparatorChar));
                string dst = (outDir == null)
                    ? Path.Combine(Path.GetDirectoryName(src), Path.GetFileNameWithoutExtension(src) + "_reference.png")
                    : Path.Combine(outDir, Path.GetDirectoryName(rel.Replace('/', Path.DirectorySeparatorChar)),
                                   Path.GetFileNameWithoutExtension(rel) + "_reference.png");

                ComposeResult r;
                try
                {
                    // A TIM no model references is a flat 2D/UI sheet: its own decode IS the
                    // reference image. Only a multi-row one is genuinely unreconstructable.
                    r = (idx.Tims[rel].Count > 0)
                        ? ComposeTim(src, CollectPrims(idx, rel, cache, res.Failures), dst)
                        : ComposeFlat(src, dst);
                }
                catch (Exception ex)
                {
                    res.Failed++;
                    res.Failures.Add(rel + ": " + ex.Message);
                    continue;
                }
                if (r.Kind == "composite") res.Made++; else res.Flat++;

                string cls = ClassOf(rel);
                ClassStat cs;
                if (!res.Classes.TryGetValue(cls, out cs)) { cs = new ClassStat(); res.Classes[cls] = cs; }
                cs.Tims++;
                cs.Covered += r.Covered;
                cs.Total += r.Total;
                if (r.Covered == r.Total) cs.Full++;
                if (r.ClutRows > 1) cs.MultiClut++;
                if (r.Kind == "flat-multirow") cs.NoRowMap++;
                res.Covered += r.Covered;
                res.Total += r.Total;
            }
            return res;
        }

        /// <summary>Back-compat: index <paramref name="root"/> and compose every texture in it,
        /// writing NAME_reference.png beside each .TIM.</summary>
        public static ComposeAllResult ComposeAll(string root, Action<int, int, string> report)
        {
            var res = new ComposeAllResult();
            try
            {
                var idx = EnsureIndex(root, report);
                return ComposeAll(idx, null, report, null);
            }
            catch (Exception ex) { res.Failures.Add(ex.Message); return res; }
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
