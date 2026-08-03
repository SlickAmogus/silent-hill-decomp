using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>High-poly texture prep. A rigged foreign model (CJ) carries several textures and
    /// per-texture UVs, but a Silent Hill character has ONE texture slot addressed by CLUT-row
    /// material names. This packs the model's textures into one sheet, relocates every UV into its
    /// atlas region (with the native-TIM V-fix — the loose-override shader samples u/W, v/H, so a
    /// non-square TIM stretches otherwise), and relabels each part's faces to its donor CLUT row so
    /// the v7 path accepts them. Output: a prepped OBJ + a loose atlas PNG. Verified end-to-end
    /// against the Python atlas pipeline that produced the working CJ.</summary>
    public static class AtlasPrep
    {
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        public class Result
        {
            public string ObjPath, AtlasPath, Error;
            public readonly List<string> Warnings = new List<string>();
            /// <summary>The v7 rebuild's own transcript — per-part counts and the weld
            /// total. The weld count is the signal that says the joints will hold, so
            /// the success dialog must show it, not drop it.</summary>
            public readonly List<string> Report = new List<string>();
            public int Textures, NativeW, NativeH, AtlasW, AtlasH, Welds;
        }

        /// <summary>material name -> resolved texture path, from the .mtl's map_Kd lines.</summary>
        public static Dictionary<string, string> ParseMtlTextures(string mtlPath)
        {
            var map = new Dictionary<string, string>(StringComparer.Ordinal);
            if (!File.Exists(mtlPath)) return map;
            string cur = null;
            string dir = Path.GetDirectoryName(mtlPath);
            foreach (string raw in File.ReadLines(mtlPath))
            {
                string ln = raw.Trim();
                if (ln.StartsWith("newmtl ", StringComparison.Ordinal)) cur = ln.Substring(7).Trim();
                else if (ln.StartsWith("map_Kd ", StringComparison.Ordinal) && cur != null)
                {
                    string tex = ln.Substring(7).Trim();
                    if (!Path.IsPathRooted(tex)) tex = Path.GetFullPath(Path.Combine(dir, tex));
                    map[cur] = tex;
                }
            }
            return map;
        }

        public static Result Build(string objPath, string mtlPath, string donorIlmPath,
                                   string outObjPath, string outAtlasPath)
        {
            var r = new Result();
            try
            {
                int nativeW, nativeH;
                if (!IlmObjConverter.TryReadNativeTimDims(donorIlmPath, out nativeW, out nativeH))
                {
                    nativeW = 256; nativeH = 256;
                    r.Warnings.Add("could not read the character's .TIM (expected NAME.TIM beside the .ILM) for its " +
                        "native size; assuming 256x256. If the real texture is not square the result may be stretched.");
                }
                r.NativeW = nativeW; r.NativeH = nativeH;

                Dictionary<string, string> partSh = IlmObjConverter.PartMaterialRows(donorIlmPath);
                Dictionary<string, string> matTex = ParseMtlTextures(mtlPath);

                // ---- pass 1: vt values, per-vt owner material, materials actually used ----
                var uvs = new List<double[]>();
                var owner = new Dictionary<int, string>();     // 1-based vt index -> source material
                var usedMats = new List<string>();
                var usedSet = new HashSet<string>(StringComparer.Ordinal);
                string curMat = null;
                foreach (string raw in File.ReadLines(objPath))
                {
                    string ln = raw.Trim();
                    if (ln.Length == 0) continue;
                    if (ln.StartsWith("vt ", StringComparison.Ordinal))
                    {
                        string[] t = ln.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                        uvs.Add(new double[] { ParseD(t[1]), ParseD(t[2]) });
                    }
                    else if (ln.StartsWith("usemtl ", StringComparison.Ordinal))
                    {
                        curMat = ln.Substring(7).Trim();
                        if (curMat.Length > 0 && usedSet.Add(curMat)) usedMats.Add(curMat);
                    }
                    else if (ln.StartsWith("f ", StringComparison.Ordinal))
                    {
                        string[] t = ln.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                        for (int i = 1; i < t.Length; i++)
                        {
                            string[] c = t[i].Split('/');
                            if (c.Length >= 2 && c[1].Length > 0)
                            {
                                int ti = int.Parse(c[1], Inv);
                                if (curMat != null && !owner.ContainsKey(ti)) owner[ti] = curMat;
                            }
                        }
                    }
                }

                // ---- pack the used textures into one atlas (shelf pack, tallest first) ----
                var toPack = new List<string>();
                foreach (string m in usedMats) if (matTex.ContainsKey(m)) toPack.Add(m);
                if (toPack.Count == 0)
                {
                    r.Error = "no material used by the OBJ has a texture (map_Kd) in the .mtl, so there is nothing to atlas.";
                    return r;
                }

                var texW = new Dictionary<string, int>();
                var texH = new Dictionary<string, int>();
                foreach (string m in toPack)
                {
                    if (!File.Exists(matTex[m])) { r.Error = "texture for material '" + m + "' not found:\n" + matTex[m]; return r; }
                    using (var img = Image.FromFile(matTex[m])) { texW[m] = img.Width; texH[m] = img.Height; }
                }

                var order = new List<string>(toPack);
                order.Sort((a, b) => texH[b].CompareTo(texH[a]));
                int atlasW = 512;
                foreach (string m in order) if (texW[m] > atlasW) atlasW = texW[m];
                // Gutters: the game samples on the donor TIM's native u8 grid, so one UV step
                // spans atlasW/nativeW atlas pixels — islands packed edge-to-edge let a boundary
                // texel floor into the neighbour. One native texel of padding per island makes
                // that impossible. The vertical gutter depends on atlasH, which the gutter
                // itself grows, so iterate to the (immediately reached in practice) fixed point.
                int gx = (atlasW + nativeW - 1) / nativeW;
                int gy = 1;
                var rect = new Dictionary<string, int[]>();    // m -> [x0,y0,w,h]
                int atlasH = 0;
                for (int pass = 0; pass < 16; pass++)
                {
                    rect.Clear();
                    int x = 0, y = 0, shelfH = 0;
                    foreach (string m in order)
                    {
                        int w = texW[m], h = texH[m];
                        if (x + w > atlasW) { x = 0; y += shelfH + gy; shelfH = 0; }
                        rect[m] = new int[] { x, y, w, h };
                        x += w + gx;
                        if (h > shelfH) shelfH = h;
                    }
                    atlasH = y + shelfH;
                    int need = (atlasH + nativeH - 1) / nativeH;
                    if (need <= gy) break;
                    gy = need;
                }

                using (var atlas = new Bitmap(atlasW, atlasH, PixelFormat.Format32bppArgb))
                {
                    using (var g = Graphics.FromImage(atlas))
                    {
                        g.Clear(Color.FromArgb(255, 0, 0, 0));
                        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
                        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
                        foreach (string m in order)
                        {
                            int[] rc = rect[m];
                            using (var img = Image.FromFile(matTex[m]))
                                g.DrawImage(img, new Rectangle(rc[0], rc[1], rc[2], rc[3]));
                        }
                    }
                    atlas.Save(outAtlasPath, ImageFormat.Png);
                }
                r.AtlasW = atlasW; r.AtlasH = atlasH; r.Textures = toPack.Count; r.AtlasPath = outAtlasPath;

                // ---- remap each vt into its owner material's atlas region + native V-fix ----
                // Game samples the atlas at (u8/nativeW, v8/nativeH); ilm_obj encodes u8=floor(u*256),
                // v8=floor((1-v)*256). Pre-scaling by native/256 makes the sample land on the atlas 1:1.
                double su = nativeW / 256.0, sv = nativeH / 256.0;
                // Half a native texel in atlas pixels: the clamp below keeps every remapped UV at
                // least this far inside its island so u8 quantisation cannot cross the gutter.
                double halfU = 0.5 * atlasW / nativeW, halfV = 0.5 * atlasH / nativeH;
                var remapped = new string[uvs.Count];
                var tiledMats = new HashSet<string>(StringComparer.Ordinal);
                for (int i = 0; i < uvs.Count; i++)
                {
                    string m; int[] rc;
                    if (owner.TryGetValue(i + 1, out m) && rect.TryGetValue(m, out rc))
                    {
                        double u = uvs[i][0], v = uvs[i][1];
                        // A tiling UV offset into the atlas would land inside the NEXT island.
                        // Wrapping is only correct when the tile spans its island exactly, hence
                        // the per-material warning below.
                        if (u < 0.0 || u > 1.0) { u -= Math.Floor(u); tiledMats.Add(m); }
                        if (v < 0.0 || v > 1.0) { v -= Math.Floor(v); tiledMats.Add(m); }
                        double px = rc[0] + u * rc[2];                            // atlas sample x, top-left origin
                        double py = rc[1] + (1.0 - v) * rc[3];                    // atlas sample y, top origin
                        px = Math.Max(rc[0] + halfU, Math.Min(rc[0] + rc[2] - halfU, px));
                        py = Math.Max(rc[1] + halfV, Math.Min(rc[1] + rc[3] - halfV, py));
                        double uo = su * (px / atlasW);
                        double vo = 1.0 - sv * (py / atlasH);
                        remapped[i] = "vt " + uo.ToString("F6", Inv) + " " + vo.ToString("F6", Inv);
                    }
                    else
                        remapped[i] = "vt " + uvs[i][0].ToString("F6", Inv) + " " + uvs[i][1].ToString("F6", Inv);
                }
                foreach (string m in usedMats)
                    if (tiledMats.Contains(m))
                        r.Warnings.Add("material '" + m + "' tiles its texture (u/v outside 0..1); tiled regions " +
                            "were wrapped and may look wrong - bake the texture flat for best results.");

                // ---- rewrite: vt remapped, usemtl -> the part's donor CLUT row, mtllib -> atlas ----
                string atlasMtl = Path.GetFileNameWithoutExtension(outObjPath) + ".mtl";
                var outLines = new List<string>();
                string curPart = null;
                int vtIdx = 0;
                foreach (string raw in File.ReadLines(objPath))
                {
                    string tr = raw.Trim();
                    if (tr.StartsWith("mtllib ", StringComparison.Ordinal)) outLines.Add("mtllib " + atlasMtl);
                    else if (tr.StartsWith("vt ", StringComparison.Ordinal)) { outLines.Add(remapped[vtIdx]); vtIdx++; }
                    else if (tr.StartsWith("o ", StringComparison.Ordinal)) { curPart = tr.Substring(2).Trim(); outLines.Add(raw); }
                    else if (tr.StartsWith("usemtl ", StringComparison.Ordinal))
                    {
                        string sm;
                        if (curPart == null || !partSh.TryGetValue(curPart, out sm)) sm = "mat00_row00";
                        outLines.Add("usemtl " + sm);
                    }
                    else outLines.Add(raw);
                }
                File.WriteAllText(outObjPath, string.Join("\n", outLines) + "\n", new UTF8Encoding(false));

                var seen = new HashSet<string>(StringComparer.Ordinal);
                var mtlSb = new StringBuilder();
                string atlasPngBase = Path.GetFileName(outAtlasPath);
                foreach (string sm in partSh.Values)
                    if (seen.Add(sm)) mtlSb.Append("newmtl " + sm + "\nKd 1 1 1\nmap_Kd " + atlasPngBase + "\n\n");
                File.WriteAllText(Path.Combine(Path.GetDirectoryName(outObjPath), atlasMtl), mtlSb.ToString(), new UTF8Encoding(false));

                r.ObjPath = outObjPath;
            }
            catch (Exception ex) { r.Error = ex.Message; }
            return r;
        }

        private static double ParseD(string s) { return double.Parse(s, NumberStyles.Float, Inv); }

        /// <summary>Locate the .mtl for an OBJ: the mtllib line resolved against the OBJ folder,
        /// else NAME.mtl beside it (Blender's default).</summary>
        public static string FindMtl(string objPath)
        {
            string dir = Path.GetDirectoryName(objPath);
            foreach (string raw in File.ReadLines(objPath))
            {
                string ln = raw.Trim();
                if (ln.StartsWith("mtllib ", StringComparison.Ordinal))
                {
                    string mt = ln.Substring(7).Trim();
                    if (!Path.IsPathRooted(mt)) mt = Path.Combine(dir, mt);
                    if (File.Exists(mt)) return mt;
                    break;
                }
            }
            string sib = Path.Combine(dir, Path.GetFileNameWithoutExtension(objPath) + ".mtl");
            return File.Exists(sib) ? sib : null;
        }

        /// <summary>The whole one-click high-poly-with-atlas chain: atlas-prep the rigged OBJ, then
        /// generate the rest-pose meta a brand-new model has no .ilmmeta.json for by exporting the
        /// donor ILM, then rebuild as v7. Writes outIlmPath + outAtlasPath (name it &lt;DONOR&gt;.TIM.png
        /// so the loose override picks it up). Warnings from both stages are merged. The WinForms
        /// button is a thin wrapper over this.</summary>
        public static Result BuildHighPoly(string objPath, string donorIlmPath, string outIlmPath, string outAtlasPath,
                                           GeometryPrep.Options geo = null, bool autoTexture = true)
        {
            var r = new Result();
            string prepObj = Path.Combine(Path.GetDirectoryName(outIlmPath),
                "_hp_" + Path.GetFileNameWithoutExtension(outIlmPath) + ".obj");
            string geoObj = Path.Combine(Path.GetDirectoryName(outIlmPath),
                "_geo_" + Path.GetFileNameWithoutExtension(outIlmPath) + ".obj");
            string prepMeta = Path.Combine(Path.GetDirectoryName(prepObj),
                Path.GetFileNameWithoutExtension(prepObj) + ".ilmmeta.json");
            string geoMeta = Path.Combine(Path.GetDirectoryName(geoObj),
                Path.GetFileNameWithoutExtension(geoObj) + ".ilmmeta.json");
            string tmpExportObj = Path.Combine(Path.GetTempPath(),
                "shp_meta_" + Path.GetFileNameWithoutExtension(donorIlmPath) + ".obj");
            try
            {
                if (geo != null && geo.MirrorLR && !geo.FixWinding)
                    r.Warnings.Add("Mirror flips face winding - Fix winding was enabled automatically");

                // Geometry pre-passes (winding fix, L/R mirror, seam collar) on a temp copy.
                GeometryPrep.Apply(objPath, geoObj, geo);

                string forV7, metaAt;
                if (autoTexture)
                {
                    string mtl = FindMtl(objPath);
                    if (mtl == null) { r.Error = "no .mtl found for the OBJ (needs a mtllib line or a NAME.mtl beside " +
                        "it) — the material textures come from it."; return r; }
                    // The atlas reads the ORIGINAL .mtl for textures (the passes keep usemtl/mtllib intact).
                    Result a = Build(geoObj, mtl, donorIlmPath, prepObj, outAtlasPath);
                    if (a.Error != null) return a;
                    r.Textures = a.Textures; r.NativeW = a.NativeW; r.NativeH = a.NativeH;
                    r.AtlasW = a.AtlasW; r.AtlasH = a.AtlasH; r.AtlasPath = a.AtlasPath;
                    r.Warnings.AddRange(a.Warnings);
                    forV7 = prepObj; metaAt = prepMeta;
                }
                else
                {
                    // Already prepped (the OBJ uses the game's materials + one sheet): v7 straight on
                    // the geometry-fixed OBJ, no atlas.
                    forV7 = geoObj; metaAt = geoMeta; r.AtlasPath = null;
                }

                // A .ilmmeta.json beside the user's OBJ is the rest pose they actually rigged
                // against (keyframe 0 is NOT reliably an idle pose) - silently minting a fresh
                // keyframe-0 meta instead would displace every part, so a sibling meta wins.
                string userMeta = Path.Combine(Path.GetDirectoryName(objPath),
                    Path.GetFileNameWithoutExtension(objPath) + ".ilmmeta.json");
                bool haveMeta = false;
                if (File.Exists(userMeta))
                {
                    try { File.Copy(userMeta, metaAt, true); haveMeta = true; }
                    catch (Exception mex)
                    {
                        r.Warnings.Add("found " + Path.GetFileName(userMeta) + " beside the OBJ but could not read " +
                            "it (" + mex.Message + "); falling back to the donor's keyframe-0 rest pose, which may " +
                            "displace parts if the model was rigged against a different export.");
                    }
                }
                if (!haveMeta)
                {
                    // A brand-new model has no .ilmmeta.json; the donor's rest poses ARE what the
                    // OBJ was rigged against, so mint the meta by exporting the donor and reuse it.
                    IlmObjConverter.ExportResult ex = IlmObjConverter.Export(donorIlmPath, tmpExportObj);
                    if (ex == null || !string.IsNullOrEmpty(ex.Error))
                    { r.Error = "could not read the donor's rest pose: " + (ex != null ? ex.Error : "unknown"); return r; }
                    File.Copy(ex.MetaPath, metaAt, true);
                }

                IlmObjConverter.ImportResult iv = IlmObjConverter.Import(forV7, donorIlmPath, outIlmPath,
                    new IlmObjConverter.ImportOptions { V7 = true });
                if (iv == null || !string.IsNullOrEmpty(iv.Error))
                { r.Error = iv != null ? iv.Error : "v7 rebuild failed"; return r; }
                r.Warnings.AddRange(iv.Warnings);
                r.Report.AddRange(iv.Report);
                r.Welds = iv.Welds;
                r.ObjPath = outIlmPath;
            }
            catch (Exception ex) { r.Error = ex.Message; }
            finally
            {
                TryDel(prepObj); TryDel(prepMeta); TryDel(tmpExportObj); TryDel(geoObj); TryDel(geoMeta);
                TryDel(Path.ChangeExtension(tmpExportObj, ".mtl"));
                TryDel(Path.ChangeExtension(tmpExportObj, ".ilmmeta.json"));
                TryDel(Path.Combine(Path.GetDirectoryName(prepObj), Path.GetFileNameWithoutExtension(prepObj) + ".mtl"));
            }
            return r;
        }

        private static void TryDel(string p) { try { if (p != null && File.Exists(p)) File.Delete(p); } catch { } }
    }
}
