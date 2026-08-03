using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// PSX TMD model parser for SH1's item models (ITEM/IT_00x.TMD, ITEM/UNQ*.TMD,
    /// ITEM/FOOK.TMD, TEST/*.TMD).
    ///
    /// TMD is the stock PsyQ 3D format, but SH1 ships only a narrow slice of it, so
    /// this parser is strict about exactly that slice — a file that deviates fails
    /// with a located error instead of quietly rendering garbage. Everything below
    /// was verified against every shipped TMD (90 files, 40 703 primitives):
    ///
    ///   * id 0x41 (0x40 accepted — same layout, older PsyQ id), FIXP flag 0, and
    ///     every section offset in the object table is relative to the OBJECT TABLE
    ///     at 0xC — not to the file start and not to the section itself.
    ///   * Exactly five primitive-header combos occur, flag byte always 0:
    ///       mode 0x30 ilen 4 (olen  6)  untextured gouraud tri     x   880
    ///       mode 0x34 ilen 6 (olen  9)  textured gouraud tri       x 36582
    ///       mode 0x36 ilen 6 (olen  9)  0x34 + semi-transparency   x  2376
    ///       mode 0x38 ilen 5 (olen  8)  untextured gouraud quad    x   268
    ///       mode 0x3C ilen 8 (olen 12)  textured gouraud quad      x   597
    ///   * UNTEXTURED payload (0x30/0x38) is {u8 r,g,b, u8 code} then 3 or 4 x
    ///     {u16 normIdx, u16 vertIdx}. Both the (norm, vert) ORDER and the meaning
    ///     of the 4th byte were confirmed empirically: read this way, every index
    ///     pair in the corpus stays inside its object's normal/vertex counts (the
    ///     swapped reading does not survive that test), and the code byte is always
    ///     the mode value repeated (0x30 / 0x38) — the PsyQ convention of embedding
    ///     the GPU command byte in the packet.
    ///   * TEXTURED payload is per-corner {u8 tu, u8 tv, u16 word} where corner 0's
    ///     word is the CLUT id and corner 1's the tpage id (remaining words are pad,
    ///     all 0 in shipped data), followed by the same {u16 norm, u16 vert} pairs.
    /// </summary>
    public sealed class TmdFile
    {
        public int Id;
        public TmdObject[] Objects;
        public int ObjectCount { get { return Objects != null ? Objects.Length : 0; } }

        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        // The object table starts here; all vertTop/normTop/primTop offsets are
        // relative to this, per the FIXP=0 TMD convention.
        private const int TableBase = 0xC;
        private const int ObjectEntrySize = 28;

        public static TmdFile Load(string path, out string error)
        {
            error = null;
            byte[] d;
            try { d = File.ReadAllBytes(path); }
            catch (Exception ex) { error = ex.Message; return null; }
            var tmd = Parse(d, out error);
            if (tmd == null && error != null)
                error = Path.GetFileName(path) + ": " + error;
            return tmd;
        }

        public static TmdFile Parse(byte[] d, out string error)
        {
            error = null;
            if (d == null || d.Length < TableBase)
            {
                error = "file too small to be a TMD (" + (d == null ? 0 : d.Length).ToString(Inv) + " bytes)";
                return null;
            }

            uint id = U32(d, 0);
            if (id != 0x41 && id != 0x40)
            {
                error = "not a TMD (id 0x" + id.ToString("X", Inv) + ", expected 0x41)";
                return null;
            }
            uint fixp = U32(d, 4);
            if (fixp != 0)
            {
                // FIXP=1 means the offsets were patched into absolute RAM addresses at
                // link time; no shipped SH1 TMD uses it and decoding one would need the
                // load address it was linked for, so refuse rather than guess.
                error = "TMD FIXP flag is " + fixp.ToString(Inv) + " (pre-relocated addresses) — unsupported";
                return null;
            }
            uint nobj = U32(d, 8);
            if (nobj == 0 || nobj > 4096)
            {
                error = "implausible object count " + nobj.ToString(Inv);
                return null;
            }
            if (TableBase + (long)nobj * ObjectEntrySize > d.Length)
            {
                error = "object table (" + nobj.ToString(Inv) + " entries) overruns the file";
                return null;
            }

            var tmd = new TmdFile { Id = (int)id, Objects = new TmdObject[nobj] };
            for (int i = 0; i < nobj; i++)
            {
                tmd.Objects[i] = ParseObject(d, i, out error);
                if (tmd.Objects[i] == null) return null;
            }
            return tmd;
        }

        private static TmdObject ParseObject(byte[] d, int index, out string error)
        {
            error = null;
            int t = TableBase + index * ObjectEntrySize;
            uint vertTop = U32(d, t + 0), vertCount = U32(d, t + 4);
            uint normTop = U32(d, t + 8), normCount = U32(d, t + 12);
            uint primTop = U32(d, t + 16), primCount = U32(d, t + 20);
            int scale = (int)U32(d, t + 24);

            string where = "object " + index.ToString(Inv);
            if (vertCount > 65535 || normCount > 65535 || primCount > 1000000)
            {
                error = where + ": implausible counts (verts " + vertCount.ToString(Inv)
                      + ", normals " + normCount.ToString(Inv) + ", prims " + primCount.ToString(Inv) + ")";
                return null;
            }
            long vo = TableBase + (long)vertTop, no = TableBase + (long)normTop, po = TableBase + (long)primTop;
            if (vo + vertCount * 8 > d.Length) { error = where + ": vertex block overruns the file"; return null; }
            if (no + normCount * 8 > d.Length) { error = where + ": normal block overruns the file"; return null; }
            if (po > d.Length) { error = where + ": primitive block starts past the end of the file"; return null; }

            var obj = new TmdObject
            {
                X = new short[vertCount], Y = new short[vertCount], Z = new short[vertCount],
                Nx = new short[normCount], Ny = new short[normCount], Nz = new short[normCount],
                Prims = new TmdPrim[primCount],
                Scale = scale,
            };
            for (int j = 0; j < vertCount; j++)
            {
                int o = (int)vo + j * 8;
                obj.X[j] = S16(d, o); obj.Y[j] = S16(d, o + 2); obj.Z[j] = S16(d, o + 4);
            }
            for (int j = 0; j < normCount; j++)
            {
                int o = (int)no + j * 8;
                obj.Nx[j] = S16(d, o); obj.Ny[j] = S16(d, o + 2); obj.Nz[j] = S16(d, o + 4);
            }

            int p = (int)po;
            for (int k = 0; k < primCount; k++)
            {
                string pwhere = where + " primitive " + k.ToString(Inv) + " at 0x" + p.ToString("X", Inv);
                if (p + 4 > d.Length) { error = pwhere + ": header overruns the file"; return null; }
                int olen = d[p], ilen = d[p + 1], flag = d[p + 2], mode = d[p + 3];
                int payload = p + 4, end = payload + ilen * 4;
                if (end > d.Length) { error = pwhere + ": payload (ilen " + ilen.ToString(Inv) + ") overruns the file"; return null; }

                // Bit 1 of the mode is ABE (semi-transparency): it changes blending
                // only, never the packet layout, so 0x36 parses as 0x34 (and a
                // hypothetical 0x32/0x3A/0x3E would parse as its opaque twin).
                int corners, needIlen;
                bool textured;
                switch (mode & ~0x02)
                {
                    case 0x30: corners = 3; needIlen = 4; textured = false; break;
                    case 0x34: corners = 3; needIlen = 6; textured = true; break;
                    case 0x38: corners = 4; needIlen = 5; textured = false; break;
                    case 0x3C: corners = 4; needIlen = 8; textured = true; break;
                    default:
                        error = pwhere + ": unsupported mode 0x" + mode.ToString("X2", Inv)
                              + " (SH1 ships only 0x30/0x34/0x36/0x38/0x3C)";
                        return null;
                }
                if (ilen != needIlen)
                {
                    error = pwhere + ": mode 0x" + mode.ToString("X2", Inv) + " with ilen "
                          + ilen.ToString(Inv) + " (expected " + needIlen.ToString(Inv) + ")";
                    return null;
                }

                var pr = new TmdPrim
                {
                    Mode = mode, Flag = flag, Olen = olen, Ilen = ilen,
                    Textured = textured, Quad = corners == 4,
                    SemiTransparent = (mode & 0x02) != 0,
                    DoubleSided = (flag & 0x02) != 0,
                    Vert = new int[corners], Norm = new int[corners],
                };

                int idx;
                if (textured)
                {
                    pr.Tu = new int[corners];
                    pr.Tv = new int[corners];
                    for (int c = 0; c < corners; c++)
                    {
                        pr.Tu[c] = d[payload + c * 4];
                        pr.Tv[c] = d[payload + c * 4 + 1];
                        int word = U16(d, payload + c * 4 + 2);
                        if (c == 0) pr.Clut = word;
                        else if (c == 1) pr.Tpage = word;
                    }
                    idx = payload + corners * 4;
                }
                else
                {
                    // Byte 3 is the GPU code (always the mode repeated in shipped
                    // data); see the class doc for how this layout was verified.
                    pr.R = d[payload]; pr.G = d[payload + 1]; pr.B = d[payload + 2];
                    idx = payload + 4;
                }
                for (int c = 0; c < corners; c++)
                {
                    int nrm = U16(d, idx + c * 4), vtx = U16(d, idx + c * 4 + 2);
                    if (nrm >= normCount)
                    {
                        error = pwhere + ": normal index " + nrm.ToString(Inv) + " >= count " + normCount.ToString(Inv);
                        return null;
                    }
                    if (vtx >= vertCount)
                    {
                        error = pwhere + ": vertex index " + vtx.ToString(Inv) + " >= count " + vertCount.ToString(Inv);
                        return null;
                    }
                    pr.Norm[c] = nrm;
                    pr.Vert[c] = vtx;
                }

                obj.Prims[k] = pr;
                p = end;
            }
            return obj;
        }

        private static uint U32(byte[] d, int o)
        {
            return (uint)d[o] | ((uint)d[o + 1] << 8) | ((uint)d[o + 2] << 16) | ((uint)d[o + 3] << 24);
        }
        private static int U16(byte[] d, int o) { return d[o] | (d[o + 1] << 8); }
        private static short S16(byte[] d, int o) { return (short)(d[o] | (d[o + 1] << 8)); }
    }

    /// <summary>One TMD object: a rigid model (UNQ*.TMD hold one, the IT_00x banks
    /// hold up to 40 independent item models sharing nothing but the file).</summary>
    public sealed class TmdObject
    {
        public short[] X, Y, Z;    // vertex positions, raw PSX units
        public short[] Nx, Ny, Nz; // normals, q4.12
        public TmdPrim[] Prims;
        public int Scale;          // 2^scale object scale; 0 in every shipped file
        public int VertexCount { get { return X != null ? X.Length : 0; } }
        public int NormalCount { get { return Nx != null ? Nx.Length : 0; } }
    }

    /// <summary>One TMD primitive (tri or quad), decoded but unresolved — indices
    /// still point into the owning object's vertex/normal arrays.</summary>
    public sealed class TmdPrim
    {
        public int Mode, Flag;
        public int Olen, Ilen;     // raw header lengths, kept for diagnostics
        public bool Textured, SemiTransparent, DoubleSided, Quad;
        public int[] Vert, Norm;   // 3 or 4 entries
        public int[] Tu, Tv;       // textured prims only, else null
        public int Clut, Tpage;    // textured prims only, else 0
        public int R, G, B;        // untextured prims only, else 0

        /// <summary>CLUT x in VRAM halfwords, from the CLUT id's packed x/16.</summary>
        public int ClutX { get { return (Clut & 0x3F) * 16; } }
        /// <summary>CLUT row = the CLUT's absolute VRAM y line.</summary>
        public int ClutRow { get { return Clut >> 6; } }
    }

    /// <summary>Renderer-ready form of a TMD: flat triangle list over shared float
    /// vertex arrays plus one ARGB texture atlas. Field shapes deliberately mirror
    /// IlmViewScene (ModelViewerForm.cs) so the same rasterizer conventions apply —
    /// in particular V is OBJ-style flipped, so sampling tx = u * TexW,
    /// ty = (1 - v) * TexH lands on the exact texel the PSX would fetch.</summary>
    public sealed class TmdViewScene
    {
        public struct Tri
        {
            public int V0, V1, V2;
            public float U0, Vv0, U1, Vv1, U2, Vv2;
            public bool Textured, SemiTransparent;
            public int R, G, B; // fill color when not textured
        }

        /// <summary>One 256x256 page of the atlas: a (tpage, CLUT row) combo and the
        /// vertical offset its texels occupy in TexPix.</summary>
        public sealed class AtlasPage
        {
            public int Tpage, ClutRow, YOffset;
            public string TimName;
        }

        public string Title;
        public float[] Vx, Vy, Vz;
        public readonly List<Tri> Tris = new List<Tri>();
        public int[] TexPix; // ARGB; null when no page could be textured
        public int TexW, TexH;
        public readonly List<AtlasPage> Pages = new List<AtlasPage>();
        public readonly List<string> Warnings = new List<string>();
        public float Cx, Cy, Cz, Radius;
        public bool HasTexture { get { return TexPix != null; } }
    }

    /// <summary>
    /// Builds a <see cref="TmdViewScene"/> from a parsed TMD.
    ///
    /// A TMD names no texture files: its prims carry VRAM tpage/CLUT ids and the
    /// GAME hard-codes which TIM it uploads to each page. The table here mirrors
    /// those fixed upload rects. The TIM headers cannot be trusted for this —
    /// TIM01.TIM's own header claims VRAM x=0 while the game uploads it to page 14 —
    /// so the mapping is a table, not derived from the files:
    ///   tpage 15 -> ITEM/TIM00.TIM   (CLUT column 176)
    ///   tpage 14 -> ITEM/TIM01..06.TIM (CLUT column 160, chosen PER MAP at runtime,
    ///               so it is ambiguous from the TMD alone — see tpage14Tim)
    ///   tpage 13 -> ITEM/TIM07.TIM   (CLUT column 240)
    ///   tpage  5 -> ITEM/FOOK.TIM
    /// CLUT ids are absolute VRAM rows; the TIM's own CLUT block y gives the row
    /// its palettes load at (0 for TIM00..07, 8 for FOOK.TIM), so the TIM-relative
    /// row is (Clut >> 6) - clutBaseY.
    /// </summary>
    public static class TmdViewSceneBuilder
    {
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        /// <summary>The TIMs the game rotates through VRAM page 14 (one per map);
        /// a UI can offer these as a dropdown for the tpage14Tim parameter.</summary>
        public static readonly string[] Tpage14Candidates =
        {
            "TIM01.TIM", "TIM02.TIM", "TIM03.TIM", "TIM04.TIM", "TIM05.TIM", "TIM06.TIM",
        };

        // A PSX texture page is 256x256 texels; every atlas page keeps that size so
        // (tu, tv) index it directly even when the mapped TIM is smaller (TIM07 is
        // 128 wide, FOOK 128x128) — the uncovered area stays transparent.
        private const int PageSize = 256;

        private sealed class TimEntry
        {
            public byte[] Data;
            public int ClutY, ClutRows;
        }

        /// <summary>Build the view scene. <paramref name="tmdPath"/> locates the
        /// TIMs (the TMD's own directory, then a sibling ITEM directory);
        /// <paramref name="objectIndex"/> selects one object from a bank file like
        /// IT_000.TMD, or -1 for all. Missing/unmappable textures degrade to
        /// untextured tris plus a warning — never a failure — so the scene always
        /// renders even with no TIM present at all.</summary>
        public static TmdViewScene Build(TmdFile tmd, string tmdPath, out string error,
                                         string tpage14Tim = "TIM01.TIM", int objectIndex = -1)
        {
            error = null;
            if (tmd == null || tmd.ObjectCount == 0) { error = "no TMD objects to build a scene from"; return null; }
            if (objectIndex < -1 || objectIndex >= tmd.ObjectCount)
            {
                error = "object index " + objectIndex.ToString(Inv) + " out of range 0.."
                      + (tmd.ObjectCount - 1).ToString(Inv);
                return null;
            }
            if (string.IsNullOrEmpty(tpage14Tim)) tpage14Tim = Tpage14Candidates[0];

            var sc = new TmdViewScene();
            sc.Title = string.IsNullOrEmpty(tmdPath) ? "TMD" : Path.GetFileName(tmdPath);
            int first = objectIndex < 0 ? 0 : objectIndex;
            int last = objectIndex < 0 ? tmd.ObjectCount - 1 : objectIndex;

            // Pass 1: one atlas page per unique (tpage, CLUT row) combo actually used.
            var pageByCombo = new Dictionary<int, TmdViewScene.AtlasPage>();
            var pagePixels = new List<int[]>();
            var timCache = new Dictionary<string, TimEntry>(StringComparer.OrdinalIgnoreCase);
            var warned = new HashSet<string>();
            for (int i = first; i <= last; i++)
            {
                foreach (var pr in tmd.Objects[i].Prims)
                {
                    if (!pr.Textured) continue;
                    int key = (pr.Tpage << 16) | pr.ClutRow;
                    if (pageByCombo.ContainsKey(key)) continue;
                    var page = BuildPage(pr.Tpage, pr.ClutRow, tmdPath, tpage14Tim,
                                         timCache, pagePixels, warned, sc.Warnings);
                    pageByCombo[key] = page;
                    if (page != null) sc.Pages.Add(page);
                }
            }
            if (pagePixels.Count > 0)
            {
                sc.TexW = PageSize;
                sc.TexH = PageSize * pagePixels.Count;
                sc.TexPix = new int[sc.TexW * sc.TexH];
                for (int pg = 0; pg < pagePixels.Count; pg++)
                    Array.Copy(pagePixels[pg], 0, sc.TexPix, pg * PageSize * PageSize, PageSize * PageSize);
            }

            // Pass 2: flatten vertices and emit triangles.
            int total = 0;
            for (int i = first; i <= last; i++) total += tmd.Objects[i].VertexCount;
            sc.Vx = new float[total]; sc.Vy = new float[total]; sc.Vz = new float[total];
            int vbase = 0;
            for (int i = first; i <= last; i++)
            {
                var obj = tmd.Objects[i];
                for (int j = 0; j < obj.VertexCount; j++)
                {
                    sc.Vx[vbase + j] = obj.X[j];
                    sc.Vy[vbase + j] = obj.Y[j];
                    sc.Vz[vbase + j] = obj.Z[j];
                }
                foreach (var pr in obj.Prims)
                {
                    TmdViewScene.AtlasPage page = null;
                    if (pr.Textured)
                        pageByCombo.TryGetValue((pr.Tpage << 16) | pr.ClutRow, out page);
                    EmitTri(sc, pr, page, vbase, 0, 1, 2);
                    if (pr.Quad) EmitTri(sc, pr, page, vbase, 1, 3, 2);
                }
                vbase += obj.VertexCount;
            }
            if (sc.Tris.Count == 0) { error = "no primitives in the selected object(s)"; return null; }

            ComputeBounds(sc);
            return sc;
        }

        private static void EmitTri(TmdViewScene sc, TmdPrim pr, TmdViewScene.AtlasPage page,
                                    int vbase, int c0, int c1, int c2)
        {
            var t = new TmdViewScene.Tri
            {
                V0 = vbase + pr.Vert[c0],
                V1 = vbase + pr.Vert[c1],
                V2 = vbase + pr.Vert[c2],
                SemiTransparent = pr.SemiTransparent,
            };
            if (page != null)
            {
                t.Textured = true;
                float ah = sc.TexH;
                t.U0 = pr.Tu[c0] / 256f; t.Vv0 = 1f - (page.YOffset + pr.Tv[c0]) / ah;
                t.U1 = pr.Tu[c1] / 256f; t.Vv1 = 1f - (page.YOffset + pr.Tv[c1]) / ah;
                t.U2 = pr.Tu[c2] / 256f; t.Vv2 = 1f - (page.YOffset + pr.Tv[c2]) / ah;
            }
            else if (!pr.Textured)
            {
                t.R = pr.R; t.G = pr.G; t.B = pr.B;
            }
            else
            {
                // Textured prim whose TIM is missing/unmapped: it carries no color of
                // its own, so fall back to neutral grey rather than black-on-black.
                t.R = 128; t.G = 128; t.B = 128;
            }
            sc.Tris.Add(t);
        }

        /// <summary>Decode one (tpage, CLUT row) combo into a 256x256 ARGB page and
        /// append it; returns null (leaving the combo untextured) on any miss.</summary>
        private static TmdViewScene.AtlasPage BuildPage(int tpage, int clutRow, string tmdPath, string tpage14Tim,
                                                        Dictionary<string, TimEntry> timCache, List<int[]> pagePixels,
                                                        HashSet<string> warned, List<string> warnings)
        {
            string timName = StockTimForTpage(tpage, tpage14Tim);
            if (timName == null)
            {
                string w = "tpage 0x" + tpage.ToString("X4", Inv) + ": no stock SH1 item VRAM mapping — prims left untextured";
                if (warned.Add(w)) warnings.Add(w);
                return null;
            }

            TimEntry entry;
            if (!timCache.TryGetValue(timName, out entry))
            {
                entry = LoadTim(timName, tmdPath, warned, warnings);
                timCache[timName] = entry; // cache the miss too, so we warn only once
            }
            if (entry == null) return null;

            int relRow = clutRow - entry.ClutY;
            if (relRow < 0 || relRow >= entry.ClutRows)
            {
                // Likely the wrong TIM was chosen for the ambiguous page 14; decode
                // with row 0 (the game's own clamp) so the shape is still visible.
                warnings.Add(timName + ": CLUT row " + clutRow.ToString(Inv) + " is outside its rows "
                           + entry.ClutY.ToString(Inv) + ".." + (entry.ClutY + entry.ClutRows - 1).ToString(Inv)
                           + " — using row 0 (wrong TIM for tpage 14?)");
                relRow = 0;
            }

            byte[] rgba;
            int tw, th, rows;
            string terr;
            if (!TimConverter.DecodeToRgba(entry.Data, relRow, out rgba, out tw, out th, out rows, out terr))
            {
                string w = timName + ": TIM decode failed (" + terr + ") — prims left untextured";
                if (warned.Add(w)) warnings.Add(w);
                return null;
            }

            var pix = new int[PageSize * PageSize];
            int cw = Math.Min(tw, PageSize), ch = Math.Min(th, PageSize);
            for (int y = 0; y < ch; y++)
            {
                int src = y * tw * 4, dst = y * PageSize;
                for (int x = 0; x < cw; x++, src += 4)
                    pix[dst + x] = (rgba[src + 3] << 24) | (rgba[src] << 16) | (rgba[src + 1] << 8) | rgba[src + 2];
            }
            var page = new TmdViewScene.AtlasPage
            {
                Tpage = tpage, ClutRow = clutRow, TimName = timName,
                YOffset = pagePixels.Count * PageSize,
            };
            pagePixels.Add(pix);
            return page;
        }

        private static string StockTimForTpage(int tpage, string tpage14Tim)
        {
            // tpage id: bits 0-3 page x, bit 4 page y, bits 7-8 texel depth. The item
            // TIMs all live on the top VRAM row in 4bpp, so anything else (TEST room
            // textures are 8bpp, some on the bottom row) is outside the stock table.
            int pageX = tpage & 0xF, pageY = (tpage >> 4) & 1, depth = (tpage >> 7) & 3;
            if (pageY != 0 || depth != 0) return null;
            switch (pageX)
            {
                case 15: return "TIM00.TIM";
                case 14: return tpage14Tim;
                case 13: return "TIM07.TIM";
                case 5: return "FOOK.TIM";
                default: return null;
            }
        }

        private static TimEntry LoadTim(string timName, string tmdPath, HashSet<string> warned, List<string> warnings)
        {
            string resolved = ResolveTim(timName, tmdPath);
            if (resolved == null)
            {
                string w = timName + " not found beside the TMD or in a sibling ITEM directory — prims left untextured";
                if (warned.Add(w)) warnings.Add(w);
                return null;
            }
            byte[] data;
            try { data = File.ReadAllBytes(resolved); }
            catch (Exception ex)
            {
                string w = timName + ": " + ex.Message + " — prims left untextured";
                if (warned.Add(w)) warnings.Add(w);
                return null;
            }

            var e = new TimEntry { Data = data, ClutY = 0, ClutRows = 1 };
            // Peek the CLUT block's VRAM y/h; TimConverter exposes neither and they
            // are what turns the TMD's absolute CLUT row into a TIM row (FOOK.TIM
            // loads its palettes at VRAM y=8, so its rows are 8..9, not 0..1).
            if (data.Length >= 20 && data[0] == 0x10 && data[1] == 0 && data[2] == 0 && data[3] == 0
                && (data[4] & 0x08) != 0)
            {
                e.ClutY = data[14] | (data[15] << 8);
                int h = data[18] | (data[19] << 8);
                e.ClutRows = h > 0 ? h : 1;
            }
            return e;
        }

        private static string ResolveTim(string timName, string tmdPath)
        {
            if (string.IsNullOrEmpty(tmdPath)) return null;
            try
            {
                string dir = Path.GetDirectoryName(Path.GetFullPath(tmdPath));
                if (string.IsNullOrEmpty(dir)) return null;
                string cand = Path.Combine(dir, timName);
                if (File.Exists(cand)) return cand;
                cand = Path.GetFullPath(Path.Combine(dir, "..", "ITEM", timName));
                if (File.Exists(cand)) return cand;
            }
            catch (Exception) { }
            return null;
        }

        private static void ComputeBounds(TmdViewScene sc)
        {
            float minx = float.MaxValue, miny = float.MaxValue, minz = float.MaxValue;
            float maxx = float.MinValue, maxy = float.MinValue, maxz = float.MinValue;
            for (int i = 0; i < sc.Vx.Length; i++)
            {
                if (sc.Vx[i] < minx) minx = sc.Vx[i];
                if (sc.Vx[i] > maxx) maxx = sc.Vx[i];
                if (sc.Vy[i] < miny) miny = sc.Vy[i];
                if (sc.Vy[i] > maxy) maxy = sc.Vy[i];
                if (sc.Vz[i] < minz) minz = sc.Vz[i];
                if (sc.Vz[i] > maxz) maxz = sc.Vz[i];
            }
            if (sc.Vx.Length == 0) { minx = maxx = miny = maxy = minz = maxz = 0; }
            sc.Cx = (minx + maxx) * 0.5f; sc.Cy = (miny + maxy) * 0.5f; sc.Cz = (minz + maxz) * 0.5f;
            float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
            sc.Radius = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
            if (sc.Radius < 1e-3f) sc.Radius = 1f;
        }
    }
}
