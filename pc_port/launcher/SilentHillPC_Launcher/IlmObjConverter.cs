using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// ILM &lt;-&gt; OBJ model conversion — the in-process C# twin of pc_port/tools/ilm_obj.py.
    ///
    /// An SH1 .ILM holds several "models" that are really rigid BODY PARTS (CAT.ILM:
    /// 01BODY_T, 02FRLEG1 ... 18TAIL2). Animation transforms each part as a unit, so a
    /// part is exported as one OBJ object (`o NAME`) and import matches parts BY NAME.
    /// The first two ASCII characters of that name, parsed as decimal, ARE the bone
    /// index (bodyprog_bone_80044F14.c:185-197), so renaming, adding or deleting an
    /// object breaks animation silently — import refuses to do it.
    ///
    ///   Export : ILM                  -> OBJ + MTL + .ilmmeta.json
    ///   Import : edited OBJ + the ORIGINAL ILM -> patched ILM
    ///
    /// Import always needs the original ILM as a template: several fields have no known
    /// meaning (MeshHeader.unkPtr_14/unkCount_3, Material.field_14/16, the ModelHeader
    /// bitfield at 0xB) and are copied through verbatim rather than guessed at.
    ///
    /// Geometry facts this relies on, all verified against real files:
    ///   * Primitive vertex/normal indices are ABSOLUTE slots in a shared scratch pool,
    ///     NOT indices into the mesh's own arrays — see ResolvePool.
    ///   * A vertex is (verticesXy[i].vx, verticesXy[i].vy, verticesZ[i]) - XY and Z
    ///     live in two separate arrays.
    ///   * A TRIANGLE sets the 4th vertex index (byte at prim+0xF) to 0xFF and leaves
    ///     the 4th UV garbage. A QUAD is two triangles in PSX FT4 winding.
    ///   * UVs are u8 per axis packed as u16, U in the low byte, V in the high byte.
    ///   * Pointers in the file are file-relative offsets.
    /// </summary>
    public static class IlmObjConverter
    {
        private const int TriSentinel = 0xFF;

        // Corner c's UV word sits at prim + UvOffsets[c] — the table is NOT uniform.
        private static readonly int[] UvOffsets = { 0, 4, 8, 0xA };

        // Python writes all three text files in text mode, so every '\n' becomes CRLF on
        // Windows; matching that keeps the C# output byte-diffable against the tool's.
        private const string Nl = "\r\n";

        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        private class IlmException : Exception { public IlmException(string m) : base(m) { } }

        // PSX Y grows downward; OBJ/Blender expect Y up, so Y is negated in both
        // directions. Values are small, so the s16 range is never at risk.
        private static double YOut(double v) { return -v; }
        private static double YIn(double v) { return -v; }

        private static ushort U16(byte[] d, int o) { return (ushort)(d[o] | (d[o + 1] << 8)); }
        private static int U32(byte[] d, int o) { return d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (d[o + 3] << 24); }
        private static short S16(byte[] d, int o) { return (short)(d[o] | (d[o + 1] << 8)); }
        private static sbyte S8(byte[] d, int o) { return (sbyte)d[o]; }

        private static void W16(byte[] d, int o, int v) { d[o] = (byte)v; d[o + 1] = (byte)(v >> 8); }

        // ---- results ------------------------------------------------------------

        public class ExportResult
        {
            public string ObjPath, MtlPath, MetaPath;
            public int Parts, Vertices, Prims, Materials;
            public int Dangling;
            public string Error;
        }

        public class ImportResult
        {
            public string IlmPath;
            public int Parts, Vertices, Normals, Prims;
            public string Error;
        }

        // ---- ILM model ----------------------------------------------------------

        private struct PoolRef { public bool Valid; public int Model, Mesh, Local; }

        private class Prim
        {
            public int Off;
            public byte[] Raw;
            public int[] U, V;
            public int Clut, Flags, MaterialIdx;
            public bool IsTransparent, Tri;
            public byte[] Vtx, Nrm;
            public PoolRef[] VRef, NRef;
        }

        private class Mesh
        {
            public int PrimCount, VertexCount, NormalCount, UnkCount3;
            public int PrimsP, XyP, ZP, NormP;
            public short[] Vx, Vy, Vz;
            public sbyte[] Nx, Ny, Nz;
            public byte[] NCount;
            public Prim[] Prims;
        }

        private class Model
        {
            public int Idx, MeshCount, VertexOffset, NormalOffset;
            public string Name;
            public Mesh[] Meshes;
        }

        private class Ilm
        {
            public byte[] D;
            public int MatCount, MatsP, ModelCount, ModelHdrsP, ModelOrderP;
            public int[] BaseClutY;
            public Model[] Models;
        }

        /// <summary>Python's bytes.decode('ascii', 'replace'): every byte &gt;= 0x80 becomes U+FFFD.
        /// Encoding.ASCII.GetString would emit '?' instead, which then fails the import name
        /// check on a file that round-trips fine through the Python tool.</summary>
        private static string Name8(byte[] d, int o)
        {
            int n = 0;
            while (n < 8 && d[o + n] != 0) n++;
            var sb = new StringBuilder(n);
            for (int i = 0; i < n; i++) sb.Append(d[o + i] < 0x80 ? (char)d[o + i] : (char)0xFFFD);
            return sb.ToString();
        }

        private static Ilm ParseIlm(byte[] d)
        {
            if (d.Length < 0x14 || d[0] != (byte)'0' || d[1] != 6)
            {
                char magic = d.Length > 0 ? (char)d[0] : '\0';
                int ver = d.Length > 1 ? d[1] : 0;
                throw new IlmException("not an ILM (magic '" + magic + "' version " + ver.ToString(Inv) + ")");
            }

            var ilm = new Ilm();
            ilm.D = d;
            ilm.MatCount = d[3];
            ilm.MatsP = U32(d, 4);
            ilm.ModelCount = d[8];
            ilm.ModelHdrsP = U32(d, 0xC);
            ilm.ModelOrderP = U32(d, 0x10);

            ilm.BaseClutY = new int[ilm.MatCount];
            for (int i = 0; i < ilm.MatCount; i++)
                ilm.BaseClutY[i] = U16(d, ilm.MatsP + i * 24 + 0x10) >> 6;

            ilm.Models = new Model[ilm.ModelCount];
            for (int i = 0; i < ilm.ModelCount; i++)
            {
                int b = ilm.ModelHdrsP + i * 16;
                var m = new Model();
                m.Idx = i;
                m.Name = Name8(d, b);
                m.MeshCount = d[b + 8];
                m.VertexOffset = d[b + 9];
                m.NormalOffset = d[b + 0xA];
                int meshHdrsP = U32(d, b + 0xC);
                m.Meshes = new Mesh[m.MeshCount];
                for (int k = 0; k < m.MeshCount; k++)
                    m.Meshes[k] = ParseMesh(d, meshHdrsP + k * 24);
                ilm.Models[i] = m;
            }
            return ilm;
        }

        private static Mesh ParseMesh(byte[] d, int mb)
        {
            var me = new Mesh();
            me.PrimCount = d[mb];
            me.VertexCount = d[mb + 1];
            me.NormalCount = d[mb + 2];
            me.UnkCount3 = d[mb + 3];
            me.PrimsP = U32(d, mb + 4);
            me.XyP = U32(d, mb + 8);
            me.ZP = U32(d, mb + 0xC);
            me.NormP = U32(d, mb + 0x10);

            me.Vx = new short[me.VertexCount];
            me.Vy = new short[me.VertexCount];
            me.Vz = new short[me.VertexCount];
            for (int v = 0; v < me.VertexCount; v++)
            {
                me.Vx[v] = S16(d, me.XyP + v * 4);
                me.Vy[v] = S16(d, me.XyP + v * 4 + 2);
                me.Vz[v] = S16(d, me.ZP + v * 2);
            }

            me.Nx = new sbyte[me.NormalCount];
            me.Ny = new sbyte[me.NormalCount];
            me.Nz = new sbyte[me.NormalCount];
            me.NCount = new byte[me.NormalCount];
            for (int n = 0; n < me.NormalCount; n++)
            {
                me.Nx[n] = S8(d, me.NormP + n * 4);
                me.Ny[n] = S8(d, me.NormP + n * 4 + 1);
                me.Nz[n] = S8(d, me.NormP + n * 4 + 2);
                me.NCount[n] = d[me.NormP + n * 4 + 3];
            }

            me.Prims = new Prim[me.PrimCount];
            for (int p = 0; p < me.PrimCount; p++)
                me.Prims[p] = ParsePrim(d, me.PrimsP + p * 20);
            return me;
        }

        private static Prim ParsePrim(byte[] d, int pb)
        {
            var pr = new Prim();
            pr.Off = pb;
            pr.Raw = new byte[20];
            Buffer.BlockCopy(d, pb, pr.Raw, 0, 20);
            pr.Flags = U16(d, pb + 6);
            pr.Clut = U16(d, pb + 2);
            // field_6's materialIdx is declared s8:7 in the decomp, so 127 means -1 "no
            // material" (DUMMY.ILM). Never normalise it to 0 — that binds a real material.
            pr.MaterialIdx = (pr.Flags >> 8) & 0x7F;
            pr.IsTransparent = ((pr.Flags >> 15) & 1) != 0;
            pr.Vtx = new byte[4];
            pr.Nrm = new byte[4];
            pr.U = new int[4];
            pr.V = new int[4];
            for (int i = 0; i < 4; i++)
            {
                pr.Vtx[i] = d[pb + 0xC + i];
                pr.Nrm[i] = d[pb + 0x10 + i];
                ushort w = U16(d, pb + UvOffsets[i]);
                pr.U[i] = w & 0xFF;
                pr.V[i] = (w >> 8) & 0xFF;
            }
            pr.Tri = pr.Vtx[3] == TriSentinel;
            pr.VRef = new PoolRef[4];
            pr.NRef = new PoolRef[4];
            return pr;
        }

        /// <summary>Map every primitive index to the (model, mesh, local index) it actually reads.
        ///
        /// Primitives do NOT index their own mesh: func_8005759C copies each part's vertices
        /// into a per-character scratch pool at ModelHeader.vertexOffset (screenXy_0[vertOffset])
        /// and the prims index that POOL. Parts are processed in LmHeader.modelOrder, and their
        /// pool ranges overlap on purpose — a part reads vertices an earlier part left in the
        /// pool wherever they meet, which is how joint seams stay welded (CAT 01BODY_T reads
        /// pool 16, written by 10HIP_TC). Resolving therefore means replaying the pool in draw
        /// order; subtracting vertexOffset instead yields negative indices on the seam prims.</summary>
        private static int[] ResolvePool(Ilm ilm)
        {
            var order = new int[ilm.ModelCount];
            // Python reads modelOrder with a slice, which truncates at EOF instead of raising;
            // the short list then fails the permutation test below and falls back to file
            // order. Indexing D directly would throw IndexOutOfRangeException on a corrupt
            // modelOrderP where the tool still exports.
            if (ilm.ModelOrderP >= 0 && ilm.ModelOrderP + ilm.ModelCount <= ilm.D.Length)
                for (int i = 0; i < ilm.ModelCount; i++) order[i] = ilm.D[ilm.ModelOrderP + i];

            var sorted = (int[])order.Clone();
            Array.Sort(sorted);
            for (int i = 0; i < sorted.Length; i++)
                if (sorted[i] != i)
                {
                    for (int j = 0; j < order.Length; j++) order[j] = j; // not a permutation: fall back to file order
                    break;
                }

            var vpool = new Dictionary<int, PoolRef>();
            var npool = new Dictionary<int, PoolRef>();
            foreach (int mi in order)
            {
                Model model = ilm.Models[mi];
                for (int k = 0; k < model.MeshCount; k++)
                {
                    Mesh mesh = model.Meshes[k];
                    for (int j = 0; j < mesh.VertexCount; j++)
                        vpool[model.VertexOffset + j] = new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j };
                    for (int j = 0; j < mesh.NormalCount; j++)
                        npool[model.NormalOffset + j] = new PoolRef { Valid = true, Model = mi, Mesh = k, Local = j };
                    foreach (Prim p in mesh.Prims)
                    {
                        int n = p.Tri ? 3 : 4;
                        for (int i = 0; i < n; i++)
                        {
                            PoolRef r;
                            p.VRef[i] = vpool.TryGetValue(p.Vtx[i], out r) ? r : default(PoolRef);
                            p.NRef[i] = npool.TryGetValue(p.Nrm[i], out r) ? r : default(PoolRef);
                        }
                    }
                }
            }
            return order;
        }

        /// <summary>Palette row a prim samples: (clut &gt;&gt; 6) - (material.field_10 &gt;&gt; 6).</summary>
        private static int ClutRow(Prim p, int[] baseClutY)
        {
            int mi = p.MaterialIdx;
            int b = (mi >= 0 && mi < baseClutY.Length) ? baseClutY[mi] : 0;
            return (p.Clut >> 6) - b;
        }

        /// <summary>C's "%02d" pads to width 2 INCLUDING the sign, so -1 formats as "-1".
        /// ToString("D2") would give "-01" and break the CLUT-row name contract shared with
        /// ClutComposer / clut_tool.py.</summary>
        private static string Pad2(int v) { return v.ToString(Inv).PadLeft(2, '0'); }

        private static string StripExt(string p)
        {
            string dir = Path.GetDirectoryName(p);
            string stem = Path.GetFileNameWithoutExtension(p);
            return string.IsNullOrEmpty(dir) ? stem : Path.Combine(dir, stem);
        }

        private static void WriteText(string path, string text)
        {
            File.WriteAllText(path, text, new UTF8Encoding(false)); // a BOM would make the OBJ's first byte non-ASCII
        }

        // ---- export -------------------------------------------------------------

        public static ExportResult Export(string ilmPath, string outObjPath)
        {
            var res = new ExportResult();
            try { ExportCore(ilmPath, outObjPath, res); }
            catch (Exception ex) { res.Error = ex.Message; }
            return res;
        }

        public static bool Export(string ilmPath, string outObjPath, out string error)
        {
            ExportResult res = Export(ilmPath, outObjPath);
            error = res.Error;
            return res.Error == null;
        }

        private static void ExportCore(string ilmPath, string outObjPath, ExportResult res)
        {
            Ilm ilm = ParseIlm(File.ReadAllBytes(ilmPath));
            int[] order = ResolvePool(ilm);

            // OBJ vertex/normal numbering: parts are emitted in FILE order, each mesh
            // contiguously, so (model, mesh, local) -> a stable 1-based OBJ index. Faces are
            // written per part but indices are global, so a face legitimately forward-
            // references a `v` that appears later in the file wherever the pool overlaps.
            var vidx = new Dictionary<int, int>();
            var nidx = new Dictionary<int, int>();
            int vn = 1, nn = 1;
            foreach (Model model in ilm.Models)
                for (int k = 0; k < model.MeshCount; k++)
                {
                    Mesh mesh = model.Meshes[k];
                    for (int j = 0; j < mesh.VertexCount; j++) vidx[RefKey(model.Idx, k, j)] = vn++;
                    for (int j = 0; j < mesh.NormalCount; j++) nidx[RefKey(model.Idx, k, j)] = nn++;
                }

            if (string.IsNullOrEmpty(outObjPath)) outObjPath = StripExt(ilmPath) + ".obj";
            string mtlPath = StripExt(outObjPath) + ".mtl";
            string metaPath = StripExt(outObjPath) + ".ilmmeta.json";

            var obj = new List<string>();
            obj.Add("# Silent Hill ILM export: " + Path.GetFileName(ilmPath));
            obj.Add("# Each 'o' is a rigid animated body part - do NOT rename/add/remove them.");
            obj.Add("mtllib " + Path.GetFileName(mtlPath));
            obj.Add("");

            int tbase = 1;
            var matNames = new List<string>();
            var matAlpha = new Dictionary<string, bool>();
            int dangling = 0;

            var js = new StringBuilder();
            js.Append('{').Append(Nl);
            js.Append(' ').Append(JStr("source")).Append(": ").Append(JStr(Path.GetFileName(ilmPath))).Append(',').Append(Nl);
            js.Append(' ').Append(JStr("drawOrder")).Append(": ");
            JIntArray(js, order, 1);
            js.Append(',').Append(Nl);
            js.Append(' ').Append(JStr("models")).Append(": ");
            JOpenArray(js, ilm.Models.Length);

            for (int modelI = 0; modelI < ilm.Models.Length; modelI++)
            {
                Model model = ilm.Models[modelI];
                obj.Add("o " + model.Name);

                Ind(js, 2).Append('{').Append(Nl);
                Ind(js, 3).Append(JStr("name")).Append(": ").Append(JStr(model.Name)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("vertexOffset")).Append(": ").Append(model.VertexOffset.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("normalOffset")).Append(": ").Append(model.NormalOffset.ToString(Inv)).Append(',').Append(Nl);
                Ind(js, 3).Append(JStr("meshes")).Append(": ");
                JOpenArray(js, model.MeshCount);

                for (int mi = 0; mi < model.MeshCount; mi++)
                {
                    Mesh mesh = model.Meshes[mi];
                    for (int j = 0; j < mesh.VertexCount; j++)
                        obj.Add("v " + mesh.Vx[j].ToString(Inv) + " " + ((int)YOut(mesh.Vy[j])).ToString(Inv) + " " + mesh.Vz[j].ToString(Inv));
                    for (int j = 0; j < mesh.NormalCount; j++)
                        obj.Add("vn " + mesh.Nx[j].ToString(Inv) + " " + ((int)YOut(mesh.Ny[j])).ToString(Inv) + " " + mesh.Nz[j].ToString(Inv));

                    // UVs are per prim-corner, so emit one vt per corner in prim order. This is a
                    // separate pass before the face pass so the vt numbering stays contiguous.
                    foreach (Prim p in mesh.Prims)
                    {
                        int cn = p.Tri ? 3 : 4;
                        for (int c = 0; c < cn; c++)
                        {
                            // PSX texel centre -> OBJ [0,1], V flipped (OBJ origin is bottom-left).
                            // 256 is the PSX page width, not the texture width.
                            double u = (p.U[c] + 0.5) / 256.0;
                            double v = 1.0 - (p.V[c] + 0.5) / 256.0;
                            obj.Add("vt " + u.ToString("F6", Inv) + " " + v.ToString("F6", Inv));
                        }
                    }

                    int t = tbase;
                    Ind(js, 4).Append('{').Append(Nl);
                    Ind(js, 5).Append(JStr("vertexCount")).Append(": ").Append(mesh.VertexCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("normalCount")).Append(": ").Append(mesh.NormalCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("primCount")).Append(": ").Append(mesh.PrimCount.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("unkCount_3")).Append(": ").Append(mesh.UnkCount3.ToString(Inv)).Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("normalCounts")).Append(": ");
                    JByteArray(js, mesh.NCount, 5);
                    js.Append(',').Append(Nl);
                    Ind(js, 5).Append(JStr("prims")).Append(": ");
                    JOpenArray(js, mesh.PrimCount);

                    for (int pi = 0; pi < mesh.PrimCount; pi++)
                    {
                        Prim p = mesh.Prims[pi];
                        int row = ClutRow(p, ilm.BaseClutY);
                        string mname = "mat" + Pad2(p.MaterialIdx) + "_row" + Pad2(row) + (p.IsTransparent ? "_alpha" : "");
                        if (!matAlpha.ContainsKey(mname)) { matAlpha[mname] = p.IsTransparent; matNames.Add(mname); }
                        obj.Add("usemtl " + mname);

                        int n = p.Tri ? 3 : 4;
                        var face = new StringBuilder("f");
                        for (int i = 0; i < n; i++)
                        {
                            PoolRef vr = p.VRef[i], nr = p.NRef[i];
                            if (!vr.Valid || !nr.Valid)
                            {
                                dangling++;
                                // Substitute local 0 of THIS mesh (mi is the mesh index here).
                                if (!vr.Valid) vr = new PoolRef { Valid = true, Model = model.Idx, Mesh = mi, Local = 0 };
                                if (!nr.Valid) nr = new PoolRef { Valid = true, Model = model.Idx, Mesh = mi, Local = 0 };
                            }
                            face.Append(' ').Append(Lookup(vidx, vr, "vertex").ToString(Inv))
                                .Append('/').Append((t + i).ToString(Inv))
                                .Append('/').Append(Lookup(nidx, nr, "normal").ToString(Inv));
                        }
                        obj.Add(face.ToString());
                        t += n;

                        Ind(js, 6).Append('{').Append(Nl);
                        Ind(js, 7).Append(JStr("tri")).Append(": ").Append(p.Tri ? "true" : "false").Append(',').Append(Nl);
                        Ind(js, 7).Append(JStr("raw")).Append(": ").Append(JStr(Hex(p.Raw))).Append(Nl);
                        Ind(js, 6).Append('}').Append(pi + 1 < mesh.PrimCount ? "," : "").Append(Nl);
                    }

                    JCloseArray(js, mesh.PrimCount, 5);
                    js.Append(Nl);
                    Ind(js, 4).Append('}').Append(mi + 1 < model.MeshCount ? "," : "").Append(Nl);
                    tbase = t;
                }

                JCloseArray(js, model.MeshCount, 3);
                js.Append(Nl);
                Ind(js, 2).Append('}').Append(modelI + 1 < ilm.Models.Length ? "," : "").Append(Nl);
                obj.Add("");
            }

            JCloseArray(js, ilm.Models.Length, 1);
            js.Append(Nl).Append('}');

            var mtl = new StringBuilder();
            mtl.Append("# CLUT palette row is encoded in the material NAME (rowNN) - keep it.").Append(Nl);
            matNames.Sort(StringComparer.Ordinal); // Python sorted() is by code point; culture-aware sorts reorder '_' vs '-'
            foreach (string name in matNames)
            {
                mtl.Append(Nl).Append("newmtl ").Append(name).Append(Nl).Append("Kd 1.0 1.0 1.0").Append(Nl);
                if (matAlpha[name]) mtl.Append("d 0.5").Append(Nl);
            }

            WriteText(outObjPath, string.Join(Nl, obj.ToArray()));
            WriteText(mtlPath, mtl.ToString());
            WriteText(metaPath, js.ToString());

            int nv = 0, npr = 0;
            foreach (Model m in ilm.Models)
                foreach (Mesh me in m.Meshes) { nv += me.VertexCount; npr += me.PrimCount; }

            res.ObjPath = outObjPath;
            res.MtlPath = mtlPath;
            res.MetaPath = metaPath;
            res.Parts = ilm.Models.Length;
            res.Vertices = nv;
            res.Prims = npr;
            res.Materials = matNames.Count;
            res.Dangling = dangling;
        }

        private static int RefKey(int model, int mesh, int local) { return (model << 16) | (mesh << 8) | local; }

        private static int Lookup(Dictionary<int, int> map, PoolRef r, string what)
        {
            int v;
            if (!map.TryGetValue(RefKey(r.Model, r.Mesh, r.Local), out v))
                throw new IlmException("primitive references a " + what + " slot no mesh owns (model " +
                    r.Model.ToString(Inv) + " mesh " + r.Mesh.ToString(Inv) + " local " + r.Local.ToString(Inv) + ")");
            return v;
        }

        private static string Hex(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            foreach (byte x in b) sb.Append(x.ToString("x2", Inv));
            return sb.ToString();
        }

        // ---- .ilmmeta.json writer (json.dump indent=1, ensure_ascii) -------------

        private static StringBuilder Ind(StringBuilder sb, int n) { return sb.Append(' ', n); }

        private static void JOpenArray(StringBuilder sb, int count)
        {
            if (count == 0) sb.Append("[]");
            else sb.Append('[').Append(Nl);
        }

        private static void JCloseArray(StringBuilder sb, int count, int indent)
        {
            if (count != 0) Ind(sb, indent).Append(']');
        }

        private static void JIntArray(StringBuilder sb, int[] vals, int indent)
        {
            if (vals.Length == 0) { sb.Append("[]"); return; }
            sb.Append('[').Append(Nl);
            for (int i = 0; i < vals.Length; i++)
                Ind(sb, indent + 1).Append(vals[i].ToString(Inv)).Append(i + 1 < vals.Length ? "," : "").Append(Nl);
            Ind(sb, indent).Append(']');
        }

        private static void JByteArray(StringBuilder sb, byte[] vals, int indent)
        {
            if (vals.Length == 0) { sb.Append("[]"); return; }
            sb.Append('[').Append(Nl);
            for (int i = 0; i < vals.Length; i++)
                Ind(sb, indent + 1).Append(((int)vals[i]).ToString(Inv)).Append(i + 1 < vals.Length ? "," : "").Append(Nl);
            Ind(sb, indent).Append(']');
        }

        private static string JStr(string s)
        {
            var sb = new StringBuilder(s.Length + 2);
            sb.Append('"');
            foreach (char c in s)
            {
                if (c == '"') sb.Append("\\\"");
                else if (c == '\\') sb.Append("\\\\");
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c == '\b') sb.Append("\\b");
                else if (c == '\f') sb.Append("\\f");
                else if (c < 0x20 || c > 0x7E) sb.Append("\\u").Append(((int)c).ToString("x4", Inv));
                else sb.Append(c);
            }
            sb.Append('"');
            return sb.ToString();
        }

        // ---- .ilmmeta.json validity check ---------------------------------------

        /// <summary>The meta is a presence-and-validity check only: it exists so the user still
        /// holds the bytes whose meaning is unknown. The Python loads it and never reads a field,
        /// so this parses for well-formedness and discards the result.</summary>
        /// <summary>The descent is recursive on fully untrusted input (a .ilmmeta.json arrives
        /// inside a downloaded mod archive), and a StackOverflowException cannot be caught on
        /// .NET 2.0+ — it kills the launcher with no message. The exporter's own meta nests 7
        /// deep, so any real file is far below this.</summary>
        private const int JsonMaxDepth = 200;

        private static void ValidateJson(string s, string what)
        {
            int i = 0;
            try
            {
                JSkipWs(s, ref i);
                JValue(s, ref i, 0);
                JSkipWs(s, ref i);
                if (i != s.Length) throw new IlmException("trailing data");
            }
            catch (Exception ex) { throw new IlmException(what + " is not valid JSON (" + ex.Message + ")"); }
        }

        private static void JSkipWs(string s, ref int i)
        {
            while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        }

        private static void JValue(string s, ref int i, int depth)
        {
            if (i >= s.Length) throw new IlmException("unexpected end of input");
            if (depth > JsonMaxDepth) throw new IlmException("nesting too deep");
            char c = s[i];
            if (c == '{') { JObject(s, ref i, depth + 1); return; }
            if (c == '[') { JArray(s, ref i, depth + 1); return; }
            if (c == '"') { JString(s, ref i); return; }
            if (JLiteral(s, ref i, "true") || JLiteral(s, ref i, "false") || JLiteral(s, ref i, "null")) return;
            JNumber(s, ref i);
        }

        private static bool JLiteral(string s, ref int i, string lit)
        {
            if (i + lit.Length > s.Length || string.CompareOrdinal(s, i, lit, 0, lit.Length) != 0) return false;
            i += lit.Length;
            return true;
        }

        private static void JObject(string s, ref int i, int depth)
        {
            i++;
            JSkipWs(s, ref i);
            if (i < s.Length && s[i] == '}') { i++; return; }
            for (; ; )
            {
                JSkipWs(s, ref i);
                if (i >= s.Length || s[i] != '"') throw new IlmException("expected a key at offset " + i.ToString(Inv));
                JString(s, ref i);
                JSkipWs(s, ref i);
                if (i >= s.Length || s[i] != ':') throw new IlmException("expected ':' at offset " + i.ToString(Inv));
                i++;
                JSkipWs(s, ref i);
                JValue(s, ref i, depth);
                JSkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == '}') { i++; return; }
                throw new IlmException("expected ',' or '}' at offset " + i.ToString(Inv));
            }
        }

        private static void JArray(string s, ref int i, int depth)
        {
            i++;
            JSkipWs(s, ref i);
            if (i < s.Length && s[i] == ']') { i++; return; }
            for (; ; )
            {
                JSkipWs(s, ref i);
                JValue(s, ref i, depth);
                JSkipWs(s, ref i);
                if (i < s.Length && s[i] == ',') { i++; continue; }
                if (i < s.Length && s[i] == ']') { i++; return; }
                throw new IlmException("expected ',' or ']' at offset " + i.ToString(Inv));
            }
        }

        private static void JString(string s, ref int i)
        {
            i++;
            while (i < s.Length)
            {
                char c = s[i++];
                if (c == '"') return;
                if (c != '\\') continue;
                if (i >= s.Length) break;
                char e = s[i++];
                if (e == 'u')
                {
                    if (i + 4 > s.Length) throw new IlmException("truncated \\u escape");
                    i += 4;
                }
                else if ("\"\\/bfnrt".IndexOf(e) < 0) throw new IlmException("bad escape '\\" + e + "'");
            }
            throw new IlmException("unterminated string");
        }

        private static void JNumber(string s, ref int i)
        {
            int start = i;
            if (i < s.Length && (s[i] == '-' || s[i] == '+')) i++;
            while (i < s.Length && (char.IsDigit(s[i]) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                                    ((s[i] == '-' || s[i] == '+') && (s[i - 1] == 'e' || s[i - 1] == 'E')))) i++;
            if (i == start) throw new IlmException("unexpected character '" + s[start] + "' at offset " + start.ToString(Inv));
        }

        // ---- OBJ parsing --------------------------------------------------------

        private class ObjObject
        {
            public string Name;
            public readonly List<int[][]> Faces = new List<int[][]>();
        }

        private class ObjFile
        {
            public readonly List<double[]> Verts = new List<double[]>();
            public readonly List<double[]> Norms = new List<double[]>();
            public readonly List<double[]> Uvs = new List<double[]>();
            public readonly List<ObjObject> Objs = new List<ObjObject>();
        }

        /// <summary>Minimal OBJ reader: objects in file order, each with its faces. v/vn/vt are
        /// GLOBAL file-order lists shared by every object, exactly as the Python has them.
        /// Splitting the keyword off at the first space only (never a tab) is deliberate — it
        /// mirrors the Python, which silently ignores a tab-separated OBJ.</summary>
        private static ObjFile ParseObj(string path)
        {
            var f = new ObjFile();
            ObjObject cur = null;
            foreach (string raw in File.ReadLines(path))
            {
                string ln = raw.Trim();
                if (ln.Length == 0 || ln[0] == '#') continue;
                int sp = ln.IndexOf(' ');
                string k = sp < 0 ? ln : ln.Substring(0, sp);
                string rest = sp < 0 ? "" : ln.Substring(sp + 1);

                if (k == "o")
                {
                    cur = new ObjObject { Name = rest.Trim() };
                    f.Objs.Add(cur);
                }
                else if (k == "v") f.Verts.Add(Floats(rest, 3, "v"));
                else if (k == "vn") f.Norms.Add(Floats(rest, 3, "vn"));
                else if (k == "vt") f.Uvs.Add(Floats(rest, 2, "vt"));
                else if (k == "f")
                {
                    if (cur == null)
                    {
                        cur = new ObjObject { Name = "" };
                        f.Objs.Add(cur);
                    }
                    string[] toks = rest.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    var corners = new int[toks.Length][];
                    for (int i = 0; i < toks.Length; i++)
                    {
                        string[] parts = toks[i].Split('/');
                        corners[i] = new[] { Component(parts, 0), Component(parts, 1), Component(parts, 2) };
                    }
                    cur.Faces.Add(corners);
                }
            }
            return f;
        }

        private static double[] Floats(string rest, int want, string kw)
        {
            string[] toks = rest.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
            if (toks.Length < want)
                throw new IlmException("'" + kw + "' line has " + toks.Length.ToString(Inv) + " values, expected at least " + want.ToString(Inv));
            var v = new double[want];
            for (int i = 0; i < want; i++)
            {
                // A comma decimal separator would silently corrupt every coordinate.
                if (!double.TryParse(toks[i], NumberStyles.Float, Inv, out v[i]))
                    throw new IlmException("'" + kw + "' line has a non-numeric value: " + toks[i]);
            }
            return v;
        }

        private static int Component(string[] parts, int i)
        {
            if (i >= parts.Length || parts[i].Length == 0) return 0;
            int v;
            if (!int.TryParse(parts[i], NumberStyles.Integer, Inv, out v))
                throw new IlmException("face index is not an integer: " + parts[i]);
            return v;
        }

        // ---- import -------------------------------------------------------------

        /// <summary>OBJ [0,1] -> PSX u8 texel. floor(), never round(): round() banker-rounds
        /// (u+0.5) up for odd u and drifts the texel by one every other coordinate, which
        /// accumulates into a visible texture shift across round-trips. Clamped to 255, the PSX
        /// page limit - NOT to the texture width, since a 128-wide TIM sits inside a 256-wide
        /// page and real UVs do reach 255.</summary>
        private static int UvByte(double f)
        {
            double s = f * 256.0;
            // Python's int(f * 256.0) raises on NaN/inf rather than writing a texel, and the
            // clamps below cannot stand in: every comparison against NaN is false, so NaN
            // would silently land on texel 0. Testing the product also catches a finite f
            // whose scaling overflows, which is exactly where Python raises OverflowError.
            if (double.IsNaN(s) || double.IsInfinity(s))
                throw new IlmException("UV coordinate is not a finite number: " + f.ToString("R", Inv));
            if (!(s > 0.0)) return 0;
            if (s >= 255.0) return 255;
            return (int)s;
        }

        /// <summary>Python 3 round(): half-to-even. Math.Round(v, MidpointRounding.ToEven) is not
        /// a substitute — it proves a midpoint by testing floor(v + 0.5) == v + 0.5, and for
        /// v = 0.5000000000000001 that sum rounds up to exactly 1.0, so the odd-value correction
        /// misfires and yields 0 where Python gives 1. Negative values are unaffected.</summary>
        private static double RoundHalfEven(double v)
        {
            double fl = Math.Floor(v + 0.5);
            if (fl - v == 0.5 && fl % 2.0 != 0.0) fl -= 1.0;
            return fl;
        }

        private static short S16Checked(double v, string what)
        {
            double r = RoundHalfEven(v);
            if (double.IsNaN(r) || r < -32768.0 || r > 32767.0)
            {
                string shown = (r >= -9.0e18 && r <= 9.0e18) ? ((long)r).ToString(Inv) : r.ToString("R", Inv);
                throw new IlmException(what + " out of s16 range: " + shown + " (models use roughly -1365..573)");
            }
            return (short)(int)r;
        }

        public static ImportResult Import(string objPath, string ilmPath, string outIlmPath)
        {
            var res = new ImportResult();
            try { ImportCore(objPath, ilmPath, outIlmPath, res); }
            catch (Exception ex) { res.Error = ex.Message; }
            return res;
        }

        public static bool Import(string objPath, string ilmPath, string outIlmPath, out string error)
        {
            ImportResult res = Import(objPath, ilmPath, outIlmPath);
            error = res.Error;
            return res.Error == null;
        }

        private static void ImportCore(string objPath, string ilmPath, string outIlmPath, ImportResult res)
        {
            string metaPath = StripExt(objPath) + ".ilmmeta.json";
            if (!File.Exists(metaPath))
                throw new IlmException("missing " + Path.GetFileName(metaPath) + " - it is written next to the OBJ by " +
                    "`export` and is required (it carries the bytes whose meaning is unknown)");
            ValidateJson(File.ReadAllText(metaPath), Path.GetFileName(metaPath));

            // Patch in place. A tight rewrite would drop the slack the original layout leaves
            // after each array, and func_8005A900/func_8005AA08 read vertices and normals THREE
            // at a time without a bounds check - a repacked file would hand them an out-of-bounds
            // read the original never had.
            byte[] data = File.ReadAllBytes(ilmPath);
            Ilm ilm = ParseIlm(data);
            ObjFile o = ParseObj(objPath);
            if (string.IsNullOrEmpty(outIlmPath)) outIlmPath = StripExt(objPath) + "_new.ILM";

            if (o.Objs.Count != ilm.Models.Length)
                throw new IlmException("OBJ has " + o.Objs.Count.ToString(Inv) + " objects but the ILM has " +
                    ilm.Models.Length.ToString(Inv) + " parts. Parts are bones - adding or removing them breaks the rig.");
            for (int i = 0; i < ilm.Models.Length; i++)
                if (!string.Equals(o.Objs[i].Name, ilm.Models[i].Name, StringComparison.Ordinal))
                    throw new IlmException("part name mismatch: OBJ has '" + o.Objs[i].Name + "' where the ILM has '" +
                        ilm.Models[i].Name + "'. The first two characters of the name ARE the bone index, so renaming " +
                        "silently re-binds the part to bone 0.");

            int vi = 0, ni = 0;
            int nvert = 0, nnorm = 0, nprim = 0;
            for (int mIdx = 0; mIdx < ilm.Models.Length; mIdx++)
            {
                Model model = ilm.Models[mIdx];
                ObjObject obj = o.Objs[mIdx];
                int want = 0;
                foreach (Mesh m in model.Meshes) want += m.PrimCount;
                if (obj.Faces.Count != want)
                    throw new IlmException("part '" + obj.Name + "' has " + obj.Faces.Count.ToString(Inv) + " faces but the ILM " +
                        "expects " + want.ToString(Inv) + " primitives. Adding or removing faces is not supported (counts are " +
                        "u8 and the bone/animation data lives in another file).");

                int fi = 0;
                foreach (Mesh mesh in model.Meshes)
                {
                    for (int j = 0; j < mesh.VertexCount; j++)
                    {
                        if (vi >= o.Verts.Count)
                            throw new IlmException("the OBJ ran out of 'v' lines at part '" + obj.Name + "': it has " +
                                o.Verts.Count.ToString(Inv) + " vertices but the ILM needs more.");
                        double[] p = o.Verts[vi++];
                        W16(data, mesh.XyP + j * 4, S16Checked(p[0], "vertex X"));
                        W16(data, mesh.XyP + j * 4 + 2, S16Checked(YIn(p[1]), "vertex Y"));
                        W16(data, mesh.ZP + j * 2, S16Checked(p[2], "vertex Z"));
                        nvert++;
                    }
                    for (int j = 0; j < mesh.NormalCount; j++)
                    {
                        if (ni >= o.Norms.Count)
                            throw new IlmException("the OBJ ran out of 'vn' lines at part '" + obj.Name + "': it has " +
                                o.Norms.Count.ToString(Inv) + " normals but the ILM needs more.");
                        double[] p = o.Norms[ni++];
                        // Byte +3 is s_Normal::count and is NEVER written: writing it previews fine
                        // and breaks lighting.
                        data[mesh.NormP + j * 4 + 0] = (byte)(sbyte)ClampS8(p[0]);
                        data[mesh.NormP + j * 4 + 1] = (byte)(sbyte)ClampS8(YIn(p[1]));
                        data[mesh.NormP + j * 4 + 2] = (byte)(sbyte)ClampS8(p[2]);
                        nnorm++;
                    }
                    foreach (Prim p in mesh.Prims)
                    {
                        int[][] face = obj.Faces[fi++];
                        int n = p.Tri ? 3 : 4;
                        if (face.Length != n)
                            throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " has " +
                                face.Length.ToString(Inv) + " corners but the primitive is a " + (p.Tri ? "triangle" : "quad") +
                                ". Triangulating or merging faces is not supported.");
                        for (int c = 0; c < n; c++)
                        {
                            // Only the UV word is written. clut, flags, and the vertex/normal index
                            // bytes are POOL SLOTS in a different numbering space from the OBJ's
                            // v/vn numbers, so they survive from the template. A triangle's UV3 word
                            // at prim+0xA is never reached and keeps its garbage value, which is what
                            // makes an unedited round-trip byte-exact.
                            int t = face[c][1];
                            if (t == 0) continue;
                            if (t < 0)
                                throw new IlmException("relative/negative OBJ indices are not supported - re-export with absolute indices.");
                            if (t > o.Uvs.Count)
                                throw new IlmException("part '" + obj.Name + "' face " + fi.ToString(Inv) + " references vt " +
                                    t.ToString(Inv) + " but the OBJ has only " + o.Uvs.Count.ToString(Inv) + " 'vt' lines.");
                            double[] uv = o.Uvs[t - 1];
                            W16(data, p.Off + UvOffsets[c], UvByte(uv[0]) | (UvByte(1.0 - uv[1]) << 8));
                        }
                        nprim++;
                    }
                }
            }

            data[2] = 0; // isLoaded: a 1 here makes the runtime skip pointer fix-up and crash
            File.WriteAllBytes(outIlmPath, data);

            res.IlmPath = outIlmPath;
            res.Parts = ilm.Models.Length;
            res.Vertices = nvert;
            res.Normals = nnorm;
            res.Prims = nprim;
        }

        private static int ClampS8(double v)
        {
            double r = RoundHalfEven(v);
            // Python's int(round(v)) raises on NaN/inf. NaN fails both clamps below and would
            // reach (int)r, whose result is undefined in C# — on x64 it is int.MinValue, which
            // truncates to a zero-length normal that previews fine and breaks lighting.
            if (double.IsNaN(r) || double.IsInfinity(r))
                throw new IlmException("normal component is not a finite number: " + v.ToString("R", Inv));
            if (r < -128.0) return -128;
            if (r > 127.0) return 127;
            return (int)r;
        }
    }
}
