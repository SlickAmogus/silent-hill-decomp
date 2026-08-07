using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// TMD &lt;-&gt; OBJ converter for SH1's item models (ITEM/IT_00x.TMD banks,
    /// ITEM/UNQ*.TMD close-ups, ITEM/FOOK.TMD, TEST/*.TMD).
    ///
    /// The sibling of <see cref="IlmObjConverter"/>, and deliberately the same
    /// shape: export writes .obj + .mtl + .tmdmeta.json, import PATCHES THE
    /// ORIGINAL FILE IN PLACE rather than rewriting it. TMD is simpler than ILM —
    /// no bone pool, no weld aliasing, no modelOrder — so the whole thing is a
    /// geometry patch.
    ///
    /// EVERY CONVENTION BELOW WAS MEASURED against all 90 shipped TMDs (40 703
    /// primitives), not carried over from the ILM converter — two of them differ:
    ///
    ///   * NORMAL ORIENTATION. A TMD normal is ANTI-PARALLEL to
    ///     cross(v1-v0, v2-v0) in 40 665 of 40 679 non-degenerate prims (100.0%;
    ///     the 14 exceptions are near-degenerate slivers). Reflecting Y to reach
    ///     OBJ's Y-up space negates cross products, so exporting positions as
    ///     (x, -y, z) and normals as (nx, -ny, nz) — the SAME transform, with the
    ///     corner order untouched — leaves the OBJ face winding and the OBJ vertex
    ///     normals agreeing and pointing outward. This is NOT the ILM rule: ILM
    ///     normals are authored inward and take the negated transform (-x, y, -z).
    ///     Using ILM's rule here would light every item from the inside.
    ///
    ///   * QUAD CORNER ORDER is the one thing that does carry over. A TMD quad is
    ///     a triangle STRIP (the renderer emits tris 0,1,2 and 1,3,2), while an OBJ
    ///     face is a perimeter LOOP, so corners are emitted 0,1,3,2 — an involution,
    ///     so import reuses the same table.
    ///
    /// Layout facts the patcher relies on, all verified across the corpus:
    ///   * Sections are laid out [every object's prims][every object's verts]
    ///     [every object's normals]; offsets are relative to the object table at
    ///     0xC (never to the file start).
    ///   * Vertex and normal entries are 8 bytes with the 4th halfword always 0.
    ///   * flag is always 0 and object scale is always 0.
    ///   * On a textured prim, corner 0's word is the CLUT id, corner 1's the tpage
    ///     id, and corners 2..3's words are padding (always 0).
    ///   * 88 of 90 files carry trailing bytes after the last normal block — in 80
    ///     of those the tail is a literal copy of the file's own opening bytes (a CD
    ///     extraction artifact). It is not model data, and patching in place
    ///     preserves it byte-for-byte without having to know that.
    /// </summary>
    public static class TmdObjConverter
    {
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        private const int TableBase = 0xC;
        private const int ObjectEntrySize = 28;
        private const int PageSize = 256;

        /// <summary>OBJ face loop order for a TMD quad's strip corners. An
        /// involution: applying it twice is the identity, so export and import
        /// share the one table.</summary>
        private static readonly int[] QuadLoop = { 0, 1, 3, 2 };

        public sealed class ExportResult
        {
            public string ObjPath, MtlPath, MetaPath;
            public int Objects, Vertices, Prims, Materials, Pages;
            public readonly List<string> TexturePaths = new List<string>();
            public readonly List<string> Warnings = new List<string>();
            public string Error;
        }

        public sealed class ImportResult
        {
            public string OutPath;
            public int Objects, Vertices, Normals, Uvs;
            public readonly List<string> Warnings = new List<string>();
            public string Error;
        }

        // ---------------------------------------------------------------- export

        /// <summary>TMD -&gt; OBJ + MTL + .tmdmeta.json (+ one PNG per texture page
        /// when the ITEM TIMs can be found beside the model).</summary>
        public static ExportResult Export(string tmdPath, string objPath, string tpage14Tim)
        {
            var res = new ExportResult();
            string err;
            var tmd = TmdFile.Load(tmdPath, out err);
            if (tmd == null) { res.Error = err ?? "could not read the TMD"; return res; }

            string dir = Path.GetDirectoryName(Path.GetFullPath(objPath));
            string stem = Path.GetFileNameWithoutExtension(objPath);
            string mtlPath = Path.Combine(dir, stem + ".mtl");
            string metaPath = Path.Combine(dir, stem + ".tmdmeta.json");

            // Pass 1: collect the material set. Textured prims key on
            // (tpage, CLUT id) because those two words are what the GAME uses to
            // find the art; untextured prims key on their flat RGB.
            var matNames = new Dictionary<string, string>(StringComparer.Ordinal);
            var matOrder = new List<string>();
            var texturedKeys = new List<int[]>(); // {tpage, clut} per textured material, in matOrder order
            foreach (var obj in tmd.Objects)
            {
                foreach (var pr in obj.Prims)
                {
                    string key = MaterialKey(pr);
                    if (matNames.ContainsKey(key)) continue;
                    matNames[key] = key;
                    matOrder.Add(key);
                    texturedKeys.Add(pr.Textured ? new[] { pr.Tpage, pr.Clut } : null);
                }
            }

            // Pass 2: decode one PNG per (tpage, CLUT row) so the OBJ shows textured
            // in Blender. Per-page rather than one atlas keeps the OBJ's UVs in plain
            // page space (u = tu/256), which is exactly invertible on import.
            var pagePng = new Dictionary<int, string>();
            for (int m = 0; m < matOrder.Count; m++)
            {
                if (texturedKeys[m] == null) continue;
                int tpage = texturedKeys[m][0], clut = texturedKeys[m][1];
                int clutRow = clut >> 6;
                int pkey = (tpage << 16) | clutRow;
                if (pagePng.ContainsKey(pkey)) continue;
                int[] argb; string timName;
                if (!TmdViewSceneBuilder.TryDecodePageArgb(tpage, clutRow, tmdPath, tpage14Tim,
                                                           out argb, out timName, res.Warnings))
                {
                    pagePng[pkey] = null;
                    continue;
                }
                string png = stem + "_tp" + tpage.ToString(Inv) + "_c" + clutRow.ToString(Inv) + ".png";
                string full = Path.Combine(dir, png);
                if (WritePng(full, argb, PageSize, PageSize, res.Warnings))
                {
                    pagePng[pkey] = png;
                    res.TexturePaths.Add(full);
                }
                else pagePng[pkey] = null;
            }
            res.Pages = res.TexturePaths.Count;

            // Pass 3: the OBJ itself. v/vn/vt are global file-order lists (the OBJ
            // standard, and what IlmObjConverter's reader already expects); each TMD
            // object becomes one `o` group.
            var sb = new StringBuilder();
            sb.Append("# Silent Hill PC — TMD export\n");
            sb.Append("# source: ").Append(Path.GetFileName(tmdPath)).Append('\n');
            sb.Append("# ").Append(tmd.ObjectCount.ToString(Inv)).Append(" object(s)\n");
            sb.Append("# Positions are (x, -y, z) of the raw PSX units; normals are q4.12/4096\n");
            sb.Append("# under the SAME transform. Do not rename, add or remove objects, and do\n");
            sb.Append("# not change vertex/face counts — \"OBJ → TMD\" patches the original file.\n");
            sb.Append("mtllib ").Append(stem).Append(".mtl\n");

            // Global 1-based cursors.
            int vBase = 1, vnBase = 1, vtBase = 1;
            var objVertStart = new int[tmd.ObjectCount];
            var objNormStart = new int[tmd.ObjectCount];
            for (int i = 0; i < tmd.ObjectCount; i++)
            {
                var obj = tmd.Objects[i];
                objVertStart[i] = vBase;
                objNormStart[i] = vnBase;
                sb.Append("o ").Append(ObjectName(i)).Append('\n');
                for (int j = 0; j < obj.VertexCount; j++)
                {
                    // PSX Y is down; OBJ Y is up.
                    sb.Append("v ").Append(F(obj.X[j])).Append(' ')
                      .Append(F(-obj.Y[j])).Append(' ').Append(F(obj.Z[j])).Append('\n');
                }
                for (int j = 0; j < obj.NormalCount; j++)
                {
                    sb.Append("vn ").Append(FN(obj.Nx[j])).Append(' ')
                      .Append(FN(-obj.Ny[j])).Append(' ').Append(FN(obj.Nz[j])).Append('\n');
                }

                // UVs are per-corner, not per-vertex: two prims can hit the same
                // vertex with different texels, so each textured corner gets its own
                // vt. Untextured prims emit none and their faces omit the slot.
                var primVt = new int[obj.Prims.Length][];
                for (int k = 0; k < obj.Prims.Length; k++)
                {
                    var pr = obj.Prims[k];
                    if (!pr.Textured) { primVt[k] = null; continue; }
                    int n = pr.Quad ? 4 : 3;
                    var slots = new int[n];
                    for (int c = 0; c < n; c++)
                    {
                        sb.Append("vt ").Append(F6(pr.Tu[c] / 256.0)).Append(' ')
                          .Append(F6(1.0 - pr.Tv[c] / 256.0)).Append('\n');
                        slots[c] = vtBase++;
                    }
                    primVt[k] = slots;
                }

                string curMat = null;
                for (int k = 0; k < obj.Prims.Length; k++)
                {
                    var pr = obj.Prims[k];
                    string mat = MaterialKey(pr);
                    if (mat != curMat) { sb.Append("usemtl ").Append(mat).Append('\n'); curMat = mat; }

                    int n = pr.Quad ? 4 : 3;
                    sb.Append('f');
                    for (int e = 0; e < n; e++)
                    {
                        // Quad strip -> perimeter loop; tris pass straight through.
                        int c = pr.Quad ? QuadLoop[e] : e;
                        int v = objVertStart[i] + pr.Vert[c];
                        int vn = objNormStart[i] + pr.Norm[c];
                        sb.Append(' ').Append(v.ToString(Inv)).Append('/');
                        if (primVt[k] != null) sb.Append(primVt[k][c].ToString(Inv));
                        sb.Append('/').Append(vn.ToString(Inv));
                    }
                    sb.Append('\n');
                    res.Prims++;
                }
                vBase += obj.VertexCount;
                vnBase += obj.NormalCount;
                res.Vertices += obj.VertexCount;
            }

            try { File.WriteAllText(objPath, sb.ToString()); }
            catch (Exception ex) { res.Error = "could not write the OBJ: " + ex.Message; return res; }

            // MTL.
            var mb = new StringBuilder();
            mb.Append("# Silent Hill PC — TMD export\n");
            for (int m = 0; m < matOrder.Count; m++)
            {
                string name = matOrder[m];
                mb.Append("newmtl ").Append(name).Append('\n');
                if (texturedKeys[m] != null)
                {
                    int tpage = texturedKeys[m][0], clut = texturedKeys[m][1];
                    mb.Append("Kd 1.000000 1.000000 1.000000\n");
                    string png;
                    if (pagePng.TryGetValue((tpage << 16) | (clut >> 6), out png) && png != null)
                        mb.Append("map_Kd ").Append(png).Append('\n');
                }
                else
                {
                    int r, g, b;
                    ParseRgbKey(name, out r, out g, out b);
                    mb.Append("Kd ").Append(F6(r / 255.0)).Append(' ')
                      .Append(F6(g / 255.0)).Append(' ').Append(F6(b / 255.0)).Append('\n');
                }
                mb.Append("illum 1\n");
            }
            try { File.WriteAllText(mtlPath, mb.ToString()); }
            catch (Exception ex) { res.Warnings.Add("could not write the MTL: " + ex.Message); }

            // Meta. Import re-parses the template TMD for the real prim attributes,
            // so this exists to CATCH A MISMATCHED TEMPLATE (and to record which
            // ambiguous page-14 TIM the textures were decoded with), not to carry
            // data the patcher needs.
            var jb = new StringBuilder();
            jb.Append("{\n  \"format\": \"tmdmeta\",\n  \"version\": 1,\n");
            jb.Append("  \"source\": ").Append(JsonStr(Path.GetFileName(tmdPath))).Append(",\n");
            jb.Append("  \"id\": ").Append(tmd.Id.ToString(Inv)).Append(",\n");
            jb.Append("  \"tpage14Tim\": ").Append(JsonStr(tpage14Tim ?? "")).Append(",\n");
            jb.Append("  \"objects\": [\n");
            for (int i = 0; i < tmd.ObjectCount; i++)
            {
                var obj = tmd.Objects[i];
                jb.Append("    { \"index\": ").Append(i.ToString(Inv))
                  .Append(", \"name\": ").Append(JsonStr(ObjectName(i)))
                  .Append(", \"scale\": ").Append(obj.Scale.ToString(Inv))
                  .Append(", \"vertexCount\": ").Append(obj.VertexCount.ToString(Inv))
                  .Append(", \"normalCount\": ").Append(obj.NormalCount.ToString(Inv))
                  .Append(", \"primCount\": ").Append(obj.Prims.Length.ToString(Inv))
                  .Append(" }").Append(i + 1 < tmd.ObjectCount ? "," : "").Append('\n');
            }
            jb.Append("  ]\n}\n");
            try { File.WriteAllText(metaPath, jb.ToString()); }
            catch (Exception ex) { res.Warnings.Add("could not write the meta JSON: " + ex.Message); }

            res.ObjPath = objPath;
            res.MtlPath = mtlPath;
            res.MetaPath = metaPath;
            res.Objects = tmd.ObjectCount;
            res.Materials = matOrder.Count;
            return res;
        }

        // ---------------------------------------------------------------- import

        /// <summary>OBJ -&gt; TMD. Patches the template's geometry in place: vertex
        /// positions, normals and texture coordinates are rewritten, every other
        /// byte (headers, offsets, prim modes, CLUT/tpage words, the trailing
        /// extractor bytes) is carried over untouched. Topology is immutable —
        /// counts must match the template, which is also what makes the patch
        /// safe.</summary>
        public static ImportResult Import(string objPath, string templateTmdPath, string outTmdPath)
        {
            var res = new ImportResult();
            string err;
            var tmd = TmdFile.Load(templateTmdPath, out err);
            if (tmd == null) { res.Error = err ?? "could not read the template TMD"; return res; }

            byte[] data;
            try { data = File.ReadAllBytes(templateTmdPath); }
            catch (Exception ex) { res.Error = "could not read the template TMD: " + ex.Message; return res; }

            ObjFile obj;
            try { obj = ParseObj(objPath); }
            catch (Exception ex) { res.Error = "could not read the OBJ: " + ex.Message; return res; }

            if (obj.Objects.Count != tmd.ObjectCount)
            {
                res.Error = "the OBJ has " + obj.Objects.Count.ToString(Inv) + " object(s) but " +
                            Path.GetFileName(templateTmdPath) + " has " + tmd.ObjectCount.ToString(Inv) +
                            ". Objects must not be added, removed or merged — re-export and edit again " +
                            "without changing the object list.";
                return res;
            }

            for (int i = 0; i < tmd.ObjectCount; i++)
            {
                var src = obj.Objects[i];
                var dst = tmd.Objects[i];
                if (src.Faces.Count != dst.Prims.Length)
                {
                    res.Error = "object " + i.ToString(Inv) + " (\"" + src.Name + "\") has " +
                                src.Faces.Count.ToString(Inv) + " face(s) but the template has " +
                                dst.Prims.Length.ToString(Inv) +
                                ". Faces must not be added, removed, or triangulated — the TMD's " +
                                "primitive list is fixed.";
                    return res;
                }
            }

            // Resolve TMD vertex/normal slots through the FACE CORNERS rather than by
            // assuming the OBJ's v-block order still matches. Blender renumbers freely
            // (Sort Elements, mesh joins), and positional inference is exactly how the
            // ILM importer once let scrambled models through silently.
            int tableEnd = TableBase + tmd.ObjectCount * ObjectEntrySize;
            if (tableEnd > data.Length) { res.Error = "object table overruns the file"; return res; }

            for (int i = 0; i < tmd.ObjectCount; i++)
            {
                var src = obj.Objects[i];
                var dst = tmd.Objects[i];
                int t = TableBase + i * ObjectEntrySize;
                int vertTop = (int)U32(data, t + 0), vertCount = (int)U32(data, t + 4);
                int normTop = (int)U32(data, t + 8), normCount = (int)U32(data, t + 12);
                int primTop = (int)U32(data, t + 16);

                var vSlot = new int[vertCount];   // TMD slot -> OBJ v index (1-based), 0 = unseen
                var nSlot = new int[normCount];

                for (int k = 0; k < dst.Prims.Length; k++)
                {
                    var pr = dst.Prims[k];
                    var face = src.Faces[k];
                    int n = pr.Quad ? 4 : 3;
                    if (face.V.Count != n)
                    {
                        res.Error = "object " + i.ToString(Inv) + " face " + (k + 1).ToString(Inv) + " has " +
                                    face.V.Count.ToString(Inv) + " corner(s) but the template primitive is a " +
                                    (pr.Quad ? "quad" : "triangle") +
                                    ". Triangulating quads (or joining tris) changes the primitive list.";
                        return res;
                    }
                    for (int e = 0; e < n; e++)
                    {
                        int c = pr.Quad ? QuadLoop[e] : e;   // involution: same table as export
                        int vi = face.V[e], ni = face.N.Count == n ? face.N[e] : 0;
                        int slot = pr.Vert[c], nslot = pr.Norm[c];
                        if (vSlot[slot] != 0 && vSlot[slot] != vi)
                        {
                            res.Warnings.Add("object " + i.ToString(Inv) + ": vertex slot " + slot.ToString(Inv) +
                                             " is referenced by two different OBJ vertices — the later one wins. " +
                                             "This happens when a shared vertex was split in the editor.");
                        }
                        vSlot[slot] = vi;
                        if (ni > 0) nSlot[nslot] = ni;

                        // Texture coordinates patch straight back per corner.
                        if (pr.Textured && face.T.Count == n && face.T[e] > 0)
                        {
                            int ti = face.T[e];
                            if (ti <= obj.Uvs.Count)
                            {
                                double[] uv = obj.Uvs[ti - 1];
                                int tu = ClampByte((int)Math.Round(uv[0] * 256.0, MidpointRounding.AwayFromZero));
                                int tv = ClampByte((int)Math.Round((1.0 - uv[1]) * 256.0, MidpointRounding.AwayFromZero));
                                int payload = PrimPayloadOffset(data, TableBase + primTop, dst.Prims, k);
                                if (payload > 0 && payload + c * 4 + 1 < data.Length)
                                {
                                    data[payload + c * 4] = (byte)tu;
                                    data[payload + c * 4 + 1] = (byte)tv;
                                    res.Uvs++;
                                }
                            }
                        }
                    }
                }

                long vo = TableBase + (long)vertTop, no = TableBase + (long)normTop;
                for (int j = 0; j < vertCount; j++)
                {
                    if (vSlot[j] == 0) continue; // slot no face references: leave the original bytes
                    double[] p = obj.Verts[vSlot[j] - 1];
                    int o = (int)vo + j * 8;
                    if (o + 6 > data.Length) { res.Error = "vertex block overruns the file"; return res; }
                    // Inverse of export: (x, -y, z).
                    WriteS16(data, o + 0, p[0], res, "vertex X");
                    WriteS16(data, o + 2, -p[1], res, "vertex Y");
                    WriteS16(data, o + 4, p[2], res, "vertex Z");
                    res.Vertices++;
                }
                for (int j = 0; j < normCount; j++)
                {
                    if (nSlot[j] == 0) continue;
                    double[] nv = obj.Norms[nSlot[j] - 1];
                    int o = (int)no + j * 8;
                    if (o + 6 > data.Length) { res.Error = "normal block overruns the file"; return res; }
                    // q4.12: the OBJ carries n/4096 under the same (x, -y, z) transform.
                    WriteS16(data, o + 0, nv[0] * 4096.0, res, "normal X");
                    WriteS16(data, o + 2, -nv[1] * 4096.0, res, "normal Y");
                    WriteS16(data, o + 4, nv[2] * 4096.0, res, "normal Z");
                    res.Normals++;
                }
                res.Objects++;
            }

            try { File.WriteAllBytes(outTmdPath, data); }
            catch (Exception ex) { res.Error = "could not write the TMD: " + ex.Message; return res; }
            res.OutPath = outTmdPath;
            return res;
        }

        // --------------------------------------------------------------- rebuild

        public sealed class RebuildResult
        {
            public string OutPath;
            public int Objects, Vertices, Normals, Prims, Tris, Quads, Textured, Untextured;
            public long Bytes;
            public readonly List<string> Warnings = new List<string>();
            public string Error;
        }

        /// Engine acceptance limits, mirrored from pc_big_tmd.c so a model that would
        /// be refused at load fails HERE with a message the modder can act on, rather
        /// than silently reverting to the stock item in game.
        private const int EngineMaxVert = 0x2000;  // BIGTMD_MAX_VERT
        private const int EngineMaxPrim = 0x2000;  // BIGTMD_MAX_PRIM
        private const int EngineMaxObj  = 48;      // BIGTMD_MAX_OBJ / GS_TMD_MAX_OBJS

        /// <summary>
        /// OBJ -&gt; TMD, REBUILT: emits a whole new file, so vertex, normal and face
        /// counts are free. This is the "actually replace the model" path;
        /// <see cref="Import"/> is the conservative one that only moves existing
        /// vertices.
        ///
        /// The template still supplies what geometry cannot: which VRAM page and CLUT
        /// each material binds to. That is not a convenience — the game decides what
        /// is uploaded to a texture page, so a modded TMD MUST reuse stock tpage/CLUT
        /// words or it samples whatever happens to be resident. Materials named the
        /// way Export writes them (tpNN_clutNN / rgb_r_g_b) carry that binding
        /// directly; anything else falls back to the template's first material of the
        /// same kind, with a warning.
        ///
        /// Only the five packet shapes the engine accepts are emitted (0x30/0x34/
        /// 0x36/0x38/0x3C, flag 0 — "no light"). A standard PSX exporter's lit packets
        /// mis-stride in GsSortObject4J's handler batches, which is a crash, so
        /// nothing else is written even if the source model implies it.
        ///
        /// Object COUNT stays fixed: a bank's object index IS the item identity
        /// (g_MapOverlayHdr.loadableItems indexes it), so adding or removing objects
        /// would silently repoint items at each other's models.
        /// </summary>
        public static RebuildResult Rebuild(string objPath, string templateTmdPath, string outTmdPath)
        {
            var res = new RebuildResult();
            string err;
            var tmpl = TmdFile.Load(templateTmdPath, out err);
            if (tmpl == null) { res.Error = err ?? "could not read the template TMD"; return res; }

            ObjFile obj;
            try { obj = ParseObj(objPath); }
            catch (Exception ex) { res.Error = "could not read the OBJ: " + ex.Message; return res; }

            if (obj.Objects.Count != tmpl.ObjectCount)
            {
                res.Error = "the OBJ has " + obj.Objects.Count.ToString(Inv) + " object(s) but " +
                            Path.GetFileName(templateTmdPath) + " has " + tmpl.ObjectCount.ToString(Inv) +
                            ". The object list is the item identity — a bank indexes its items by " +
                            "object number — so objects may be reshaped but not added or removed.";
                return res;
            }
            if (tmpl.ObjectCount > EngineMaxObj)
            {
                res.Error = "the template has " + tmpl.ObjectCount.ToString(Inv) +
                            " objects; the engine accepts at most " + EngineMaxObj.ToString(Inv) + ".";
                return res;
            }

            // Per-object built blocks, serialised together at the end because the
            // file is laid out [all prims][all verts][all normals].
            int n = tmpl.ObjectCount;
            var primBytes = new List<byte>[n];
            var vertBytes = new List<byte>[n];
            var normBytes = new List<byte>[n];
            var vertCounts = new int[n];
            var normCounts = new int[n];
            var primCounts = new int[n];

            var fallbackWarned = new HashSet<string>();

            for (int i = 0; i < n; i++)
            {
                var src = obj.Objects[i];
                var tobj = tmpl.Objects[i];

                // Default VRAM binding: the template object's own first textured /
                // untextured prim, used whenever a material name does not carry one.
                int defTpage = 0, defClut = 0; bool haveTexDefault = false;
                int defR = 128, defG = 128, defB = 128;
                foreach (var pr in tobj.Prims)
                {
                    if (pr.Textured && !haveTexDefault) { defTpage = pr.Tpage; defClut = pr.Clut; haveTexDefault = true; }
                    if (!pr.Textured) { defR = pr.R; defG = pr.G; defB = pr.B; }
                }

                var vmap = new Dictionary<int, int>();   // OBJ v index -> local slot
                var nmap = new Dictionary<int, int>();   // OBJ vn index -> local slot
                var vlist = new List<int>();
                var nlist = new List<double[]>();        // OBJ-space normals, resolved on the spot
                var prims = new List<byte>();
                int primCount = 0;

                foreach (var face in src.Faces)
                {
                    bool textured, semi; int tpage, clut, cr, cg, cb;
                    ResolveMaterial(face.Mtl, haveTexDefault, defTpage, defClut, defR, defG, defB,
                                    res, fallbackWarned, out textured, out semi, out tpage, out clut,
                                    out cr, out cg, out cb);

                    // The engine hard-switches on the exact mode byte (pc_big_tmd.c):
                    // only 0x30/0x34/0x36/0x38/0x3C are accepted. 0x36 (textured tri)
                    // is the ONLY semi-transparent shape that exists, so a semi quad
                    // has to be split into two tris and semi on an untextured face
                    // cannot be represented at all.
                    if (semi && !textured)
                    {
                        semi = false;
                        if (fallbackWarned.Add("semiflat"))
                            res.Warnings.Add("Semi-transparency was dropped on untextured faces — the " +
                                             "engine has no semi-transparent flat-shaded packet.");
                    }

                    // Fan-triangulate anything past a quad: the engine has no n-gon
                    // packet, and silently dropping the extra corners would lose
                    // geometry the modder can see in Blender.
                    var runs = new List<int[]>();
                    if (face.V.Count == 3) runs.Add(new[] { 0, 1, 2 });
                    else if (face.V.Count == 4)
                    {
                        if (semi)
                        {
                            runs.Add(new[] { 0, 1, 2 });
                            runs.Add(new[] { 0, 2, 3 });
                            if (fallbackWarned.Add("semiquad"))
                                res.Warnings.Add("Semi-transparent quads were split into two triangles — " +
                                                 "0x36 is the only semi-transparent packet the engine accepts.");
                        }
                        else runs.Add(new[] { 0, 1, 2, 3 });
                    }
                    else if (face.V.Count > 4)
                    {
                        for (int k = 1; k + 1 < face.V.Count; k++) runs.Add(new[] { 0, k, k + 1 });
                        if (fallbackWarned.Add("ngon"))
                            res.Warnings.Add("Faces with more than 4 corners were split into triangles — " +
                                             "the PSX has no n-gon primitive.");
                    }
                    else
                    {
                        if (fallbackWarned.Add("degen"))
                            res.Warnings.Add("Faces with fewer than 3 corners were skipped.");
                        continue;
                    }

                    foreach (int[] run in runs)
                    {
                        int corners = run.Length;
                        var vIdx = new int[corners];
                        var nIdx = new int[corners];
                        var tu = new int[corners];
                        var tv = new int[corners];
                        double[] faceNrm = null; // computed once, only if a corner lacks vn

                        for (int c = 0; c < corners; c++)
                        {
                            // TMD corner c comes from OBJ loop position QuadLoop[c]
                            // on a quad (involution, same table as export).
                            int e = corners == 4 ? QuadLoop[c] : c;
                            int ov = face.V[run[e]];
                            int on = face.N.Count > run[e] ? face.N[run[e]] : 0;
                            int ot = face.T.Count > run[e] ? face.T[run[e]] : 0;

                            int slot;
                            if (!vmap.TryGetValue(ov, out slot))
                            {
                                slot = vlist.Count; vmap[ov] = slot; vlist.Add(ov);
                            }
                            vIdx[c] = slot;

                            int nslot;
                            if (on > 0 && on <= obj.Norms.Count)
                            {
                                if (!nmap.TryGetValue(on, out nslot))
                                {
                                    nslot = nlist.Count; nmap[on] = nslot; nlist.Add(obj.Norms[on - 1]);
                                }
                            }
                            else
                            {
                                // No vn on this corner: give it the face's own outward
                                // normal. Computed once per face, shared by its corners.
                                if (faceNrm == null) faceNrm = FaceNormalObj(obj, face, run);
                                nslot = nlist.Count; nlist.Add(faceNrm);
                            }
                            nIdx[c] = nslot;

                            if (textured && ot > 0 && ot <= obj.Uvs.Count)
                            {
                                double[] uv = obj.Uvs[ot - 1];
                                tu[c] = ClampByte((int)Math.Round(uv[0] * 256.0, MidpointRounding.AwayFromZero));
                                tv[c] = ClampByte((int)Math.Round((1.0 - uv[1]) * 256.0, MidpointRounding.AwayFromZero));
                            }
                        }

                        int mode = textured ? (corners == 4 ? 0x3C : 0x34) : (corners == 4 ? 0x38 : 0x30);
                        if (semi && textured && corners == 3) mode = 0x36; // the only legal semi shape
                        int ilen = textured ? (corners == 4 ? 8 : 6) : (corners == 4 ? 5 : 4);
                        int olen = textured ? (corners == 4 ? 12 : 9) : (corners == 4 ? 8 : 6);

                        prims.Add((byte)olen); prims.Add((byte)ilen); prims.Add(0); prims.Add((byte)mode);
                        if (textured)
                        {
                            for (int c = 0; c < corners; c++)
                            {
                                prims.Add((byte)tu[c]); prims.Add((byte)tv[c]);
                                int word = c == 0 ? clut : (c == 1 ? tpage : 0);
                                prims.Add((byte)(word & 0xFF)); prims.Add((byte)((word >> 8) & 0xFF));
                            }
                        }
                        else
                        {
                            prims.Add((byte)cr); prims.Add((byte)cg); prims.Add((byte)cb);
                            prims.Add((byte)mode); // the GPU code byte is the mode repeated
                        }
                        for (int c = 0; c < corners; c++)
                        {
                            prims.Add((byte)(nIdx[c] & 0xFF)); prims.Add((byte)((nIdx[c] >> 8) & 0xFF));
                            prims.Add((byte)(vIdx[c] & 0xFF)); prims.Add((byte)((vIdx[c] >> 8) & 0xFF));
                        }
                        primCount++;
                        if (corners == 4) res.Quads++; else res.Tris++;
                        if (textured) res.Textured++; else res.Untextured++;
                    }
                }

                if (vlist.Count == 0 || primCount == 0)
                {
                    res.Error = "object " + i.ToString(Inv) + " (\"" + src.Name + "\") has no geometry. " +
                                "Every object must keep at least one face.";
                    return res;
                }
                if (vlist.Count > EngineMaxVert)
                {
                    res.Error = "object " + i.ToString(Inv) + " has " + vlist.Count.ToString(Inv) +
                                " vertices; the engine accepts at most " + EngineMaxVert.ToString(Inv) +
                                " per object (pc_big_tmd.c). Decimate the mesh.";
                    return res;
                }
                if (primCount > EngineMaxPrim)
                {
                    res.Error = "object " + i.ToString(Inv) + " has " + primCount.ToString(Inv) +
                                " faces; the engine accepts at most " + EngineMaxPrim.ToString(Inv) +
                                " per object (pc_big_tmd.c). Decimate the mesh.";
                    return res;
                }

                // Serialise the vertex / normal blocks (8 bytes each, 4th halfword 0).
                var vb = new List<byte>();
                foreach (int ov in vlist)
                {
                    double[] p = obj.Verts[ov - 1];
                    AppendS16(vb, p[0], res, "vertex X");
                    AppendS16(vb, -p[1], res, "vertex Y");   // inverse of export's (x,-y,z)
                    AppendS16(vb, p[2], res, "vertex Z");
                    vb.Add(0); vb.Add(0);
                }
                var nb = new List<byte>();
                foreach (double[] v in nlist)
                {
                    double nx = v[0], ny = v[1], nz = v[2];
                    double len = Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    if (len < 1e-9) { nx = 0; ny = 0; nz = 1; len = 1; }
                    // Same transform as positions; see the class doc for why this is
                    // NOT the ILM rule.
                    AppendS16(nb, nx / len * 4096.0, res, "normal X");
                    AppendS16(nb, -ny / len * 4096.0, res, "normal Y");
                    AppendS16(nb, nz / len * 4096.0, res, "normal Z");
                    nb.Add(0); nb.Add(0);
                }

                primBytes[i] = prims; vertBytes[i] = vb; normBytes[i] = nb;
                vertCounts[i] = vlist.Count; normCounts[i] = nlist.Count; primCounts[i] = primCount;
                res.Vertices += vlist.Count;
                res.Normals += nlist.Count;
                res.Prims += primCount;
            }

            // Assemble: header, object table, then [all prims][all verts][all normals].
            var outBytes = new List<byte>();
            AppendU32(outBytes, (uint)(tmpl.Id == 0 ? 0x41 : tmpl.Id));
            AppendU32(outBytes, 0);                       // FIXP: offsets stay relative
            AppendU32(outBytes, (uint)n);

            int tableBytes = n * ObjectEntrySize;
            int cursor = tableBytes;                      // offsets are relative to 0xC
            var primTop = new int[n]; var vertTop = new int[n]; var normTop = new int[n];
            for (int i = 0; i < n; i++) { primTop[i] = cursor; cursor += primBytes[i].Count; }
            for (int i = 0; i < n; i++) { vertTop[i] = cursor; cursor += vertBytes[i].Count; }
            for (int i = 0; i < n; i++) { normTop[i] = cursor; cursor += normBytes[i].Count; }

            for (int i = 0; i < n; i++)
            {
                AppendU32(outBytes, (uint)vertTop[i]); AppendU32(outBytes, (uint)vertCounts[i]);
                AppendU32(outBytes, (uint)normTop[i]); AppendU32(outBytes, (uint)normCounts[i]);
                AppendU32(outBytes, (uint)primTop[i]); AppendU32(outBytes, (uint)primCounts[i]);
                AppendU32(outBytes, (uint)tmpl.Objects[i].Scale);
            }
            for (int i = 0; i < n; i++) outBytes.AddRange(primBytes[i]);
            for (int i = 0; i < n; i++) outBytes.AddRange(vertBytes[i]);
            for (int i = 0; i < n; i++) outBytes.AddRange(normBytes[i]);

            // Prove the result parses under the same strict reader the viewer and the
            // engine's validator use, before it can reach a save dialog.
            string verr;
            var check = TmdFile.Parse(outBytes.ToArray(), out verr);
            if (check == null)
            {
                res.Error = "the rebuilt model failed its own parse check (" + verr +
                            "). This is a converter bug — please report it with the OBJ.";
                return res;
            }

            try { File.WriteAllBytes(outTmdPath, outBytes.ToArray()); }
            catch (Exception ex) { res.Error = "could not write the TMD: " + ex.Message; return res; }

            res.OutPath = outTmdPath;
            res.Objects = n;
            res.Bytes = outBytes.Count;
            return res;
        }

        /// <summary>Material name -&gt; VRAM binding. Names Export writes
        /// (tpNN_clutNN / rgb_r_g_b) carry it exactly; anything else falls back to the
        /// template object's own binding, which keeps a Blender-authored model on
        /// stock pages instead of sampling an arbitrary one.</summary>
        private static void ResolveMaterial(string mtl, bool haveTexDefault, int defTpage, int defClut,
                                            int defR, int defG, int defB,
                                            RebuildResult res, HashSet<string> warned,
                                            out bool textured, out bool semi, out int tpage, out int clut,
                                            out int r, out int g, out int b)
        {
            textured = haveTexDefault; tpage = defTpage; clut = defClut;
            r = defR; g = defG; b = defB;
            // Export appends "_semi" for a semi-transparent prim; without reading it
            // back, every such face rebuilds opaque.
            semi = mtl != null && mtl.EndsWith("_semi", StringComparison.Ordinal);
            if (string.IsNullOrEmpty(mtl))
            {
                if (warned.Add("nomtl"))
                    res.Warnings.Add("Some faces carry no 'usemtl'; they use the original model's " +
                                     "own texture page and palette.");
                return;
            }
            if (mtl.StartsWith("tp", StringComparison.Ordinal))
            {
                int us = mtl.IndexOf("_clut", StringComparison.Ordinal);
                if (us > 2)
                {
                    int tp, cl;
                    string tail = mtl.Substring(us + 5);
                    int suffix = tail.IndexOf('_'); // trims the "_semi" marker
                    if (suffix >= 0) tail = tail.Substring(0, suffix);
                    if (int.TryParse(mtl.Substring(2, us - 2), NumberStyles.Integer, Inv, out tp) &&
                        int.TryParse(tail, NumberStyles.Integer, Inv, out cl))
                    {
                        textured = true; tpage = tp; clut = cl;
                        return;
                    }
                }
            }
            if (mtl.StartsWith("rgb_", StringComparison.Ordinal))
            {
                int rr, gg, bb;
                ParseRgbKey(mtl, out rr, out gg, out bb);
                textured = false; r = rr; g = gg; b = bb;
                return;
            }
            if (warned.Add("mtl:" + mtl))
                res.Warnings.Add("Material \"" + mtl + "\" is not one this converter writes " +
                                 "(tpNN_clutNN or rgb_r_g_b), so those faces keep the original " +
                                 "model's texture page and palette. A modded TMD has to reuse a " +
                                 "stock VRAM page — the game decides what is uploaded there.");
        }

        /// <summary>Outward face normal in OBJ space (CCW winding = outward).</summary>
        private static double[] FaceNormalObj(ObjFile f, ObjFace face, int[] run)
        {
            double[] a = f.Verts[face.V[run[0]] - 1];
            double[] b = f.Verts[face.V[run[1]] - 1];
            double[] c = f.Verts[face.V[run[2]] - 1];
            double e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
            double e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
            return new[]
            {
                e1y * e2z - e1z * e2y,
                e1z * e2x - e1x * e2z,
                e1x * e2y - e1y * e2x,
            };
        }

        private static void AppendS16(List<byte> d, double value, RebuildResult res, string what)
        {
            double r = Math.Round(value, MidpointRounding.AwayFromZero);
            if (r > 32767.0 || r < -32768.0)
            {
                if (res.Warnings.Count < 40)
                    res.Warnings.Add(what + " " + r.ToString("0", Inv) + " is outside the 16-bit range a " +
                                     "TMD stores and was clamped — scale the model down or move it to the origin.");
                r = r > 0 ? 32767.0 : -32768.0;
            }
            short s = (short)r;
            d.Add((byte)(s & 0xFF));
            d.Add((byte)((s >> 8) & 0xFF));
        }

        private static void AppendU32(List<byte> d, uint v)
        {
            d.Add((byte)(v & 0xFF)); d.Add((byte)((v >> 8) & 0xFF));
            d.Add((byte)((v >> 16) & 0xFF)); d.Add((byte)((v >> 24) & 0xFF));
        }

        /// <summary>Byte offset of primitive <paramref name="index"/>'s payload,
        /// walked from the block start because packets are variable length (ilen is
        /// per mode). Returns -1 if the walk leaves the file.</summary>
        private static int PrimPayloadOffset(byte[] d, int primBlockStart, TmdPrim[] prims, int index)
        {
            int p = primBlockStart;
            for (int k = 0; k < index; k++)
            {
                if (p + 4 > d.Length) return -1;
                p += 4 + d[p + 1] * 4; // ilen words
            }
            if (p + 4 > d.Length) return -1;
            return p + 4;
        }

        // --------------------------------------------------------------- helpers

        private static string ObjectName(int i)
        {
            return "tmdobj" + i.ToString("D3", Inv);
        }

        private static string MaterialKey(TmdPrim pr)
        {
            if (pr.Textured)
                return "tp" + pr.Tpage.ToString(Inv) + "_clut" + pr.Clut.ToString(Inv) +
                       (pr.SemiTransparent ? "_semi" : "");
            return "rgb_" + pr.R.ToString(Inv) + "_" + pr.G.ToString(Inv) + "_" + pr.B.ToString(Inv) +
                   (pr.SemiTransparent ? "_semi" : "");
        }

        private static void ParseRgbKey(string key, out int r, out int g, out int b)
        {
            r = g = b = 128;
            if (!key.StartsWith("rgb_", StringComparison.Ordinal)) return;
            string[] parts = key.Substring(4).Split('_');
            if (parts.Length >= 3)
            {
                int.TryParse(parts[0], NumberStyles.Integer, Inv, out r);
                int.TryParse(parts[1], NumberStyles.Integer, Inv, out g);
                int.TryParse(parts[2], NumberStyles.Integer, Inv, out b);
            }
        }

        /// <summary>Integer-valued coordinate. TMD positions are whole PSX units, so
        /// printing them as integers round-trips exactly and keeps the OBJ readable.</summary>
        private static string F(int v) { return v.ToString(Inv); }

        /// <summary>q4.12 normal component as its real value. Six decimals is ample:
        /// the quantum is 1/4096 ~ 2.4e-4, three orders above the print error, so
        /// Math.Round(parsed * 4096) recovers the original short exactly.</summary>
        private static string FN(int v) { return F6(v / 4096.0); }

        private static string F6(double v)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) v = 0.0;
            string s = v.ToString("0.000000", Inv);
            return s == "-0.000000" ? "0.000000" : s;
        }

        private static int ClampByte(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

        private static void WriteS16(byte[] d, int o, double value, ImportResult res, string what)
        {
            double r = Math.Round(value, MidpointRounding.AwayFromZero);
            if (r > 32767.0 || r < -32768.0)
            {
                res.Warnings.Add(what + " " + r.ToString("0", Inv) +
                                 " is outside the 16-bit range a TMD stores and was clamped — " +
                                 "the model is too large or too far from the origin.");
                r = r > 0 ? 32767.0 : -32768.0;
            }
            short s = (short)r;
            d[o] = (byte)(s & 0xFF);
            d[o + 1] = (byte)((s >> 8) & 0xFF);
        }

        private static uint U32(byte[] d, int o)
        {
            return (uint)d[o] | ((uint)d[o + 1] << 8) | ((uint)d[o + 2] << 16) | ((uint)d[o + 3] << 24);
        }

        private static string JsonStr(string s)
        {
            var sb = new StringBuilder("\"");
            foreach (char c in s ?? "")
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c == '\n') sb.Append("\\n");
                else if (c == '\r') sb.Append("\\r");
                else if (c == '\t') sb.Append("\\t");
                else if (c < ' ') sb.Append("\\u").Append(((int)c).ToString("x4", Inv));
                else sb.Append(c);
            }
            return sb.Append('"').ToString();
        }

        private static bool WritePng(string path, int[] argb, int w, int h, List<string> warnings)
        {
            try
            {
                using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
                {
                    var rect = new Rectangle(0, 0, w, h);
                    var bd = bmp.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                    try
                    {
                        System.Runtime.InteropServices.Marshal.Copy(argb, 0, bd.Scan0, w * h);
                    }
                    finally { bmp.UnlockBits(bd); }
                    bmp.Save(path, ImageFormat.Png);
                }
                return true;
            }
            catch (Exception ex)
            {
                warnings.Add("could not write " + Path.GetFileName(path) + ": " + ex.Message);
                return false;
            }
        }

        // ------------------------------------------------------------ OBJ reader

        private sealed class ObjFace
        {
            public readonly List<int> V = new List<int>();
            public readonly List<int> T = new List<int>();
            public readonly List<int> N = new List<int>();
            public string Mtl;   // usemtl in effect; carries the VRAM binding on rebuild
        }

        private sealed class ObjObj
        {
            public string Name;
            public readonly List<ObjFace> Faces = new List<ObjFace>();
        }

        private sealed class ObjFile
        {
            public readonly List<double[]> Verts = new List<double[]>();
            public readonly List<double[]> Norms = new List<double[]>();
            public readonly List<double[]> Uvs = new List<double[]>();
            public readonly List<ObjObj> Objects = new List<ObjObj>();
        }

        /// <summary>Minimal OBJ reader. v/vt/vn are global file-order lists shared by
        /// every object (the format's own rule); `o` opens a group, with `g` accepted
        /// as a fallback for exporters that emit only groups. Negative (relative)
        /// indices are resolved against the list length at the point of use, as the
        /// spec requires.</summary>
        private static ObjFile ParseObj(string path)
        {
            var f = new ObjFile();
            ObjObj cur = null;
            bool sawO = false;
            string mtl = null;   // persists across object boundaries, as the format requires

            foreach (string raw in File.ReadLines(path))
            {
                string ln = raw.Trim();
                if (ln.Length == 0 || ln[0] == '#') continue;
                int sp = ln.IndexOf(' ');
                string k = sp < 0 ? ln : ln.Substring(0, sp);
                string rest = sp < 0 ? "" : ln.Substring(sp + 1).Trim();

                if (k == "usemtl") { mtl = rest; continue; }

                if (k == "v") { f.Verts.Add(Nums(rest, 3)); continue; }
                if (k == "vn") { f.Norms.Add(Nums(rest, 3)); continue; }
                if (k == "vt") { f.Uvs.Add(Nums(rest, 2)); continue; }

                if (k == "o" || (k == "g" && !sawO))
                {
                    if (k == "o") sawO = true;
                    string name = rest;
                    // Blender writes `o NAME` followed by a redundant `g NAME`; treating
                    // those as two objects would double the count.
                    if (k == "g" && cur != null && string.Equals(cur.Name, name, StringComparison.Ordinal)) continue;
                    if (k == "g" && (name == "default" || name == "off" || name.Length == 0)) continue;
                    cur = new ObjObj { Name = name };
                    f.Objects.Add(cur);
                    continue;
                }

                if (k == "f")
                {
                    if (cur == null) { cur = new ObjObj { Name = "" }; f.Objects.Add(cur); }
                    var face = new ObjFace { Mtl = mtl };
                    foreach (string tok in rest.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries))
                    {
                        string[] bits = tok.Split('/');
                        face.V.Add(Ref(bits.Length > 0 ? bits[0] : "", f.Verts.Count));
                        face.T.Add(bits.Length > 1 ? Ref(bits[1], f.Uvs.Count) : 0);
                        face.N.Add(bits.Length > 2 ? Ref(bits[2], f.Norms.Count) : 0);
                    }
                    cur.Faces.Add(face);
                }
            }
            return f;
        }

        /// <summary>OBJ index: 1-based, or negative counting back from the current end.
        /// 0 means "absent" (an empty slot in v//vn).</summary>
        private static int Ref(string s, int count)
        {
            if (string.IsNullOrEmpty(s)) return 0;
            int v;
            if (!int.TryParse(s, NumberStyles.Integer, Inv, out v)) return 0;
            if (v < 0) return count + 1 + v;
            return v;
        }

        private static double[] Nums(string s, int n)
        {
            var outv = new double[n];
            string[] bits = s.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < n; i++)
            {
                double d = 0;
                if (i < bits.Length)
                    double.TryParse(bits[i], NumberStyles.Float, Inv, out d);
                outv[i] = d;
            }
            return outv;
        }
    }
}
