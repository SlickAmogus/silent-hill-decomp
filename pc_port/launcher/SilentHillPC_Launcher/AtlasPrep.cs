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
            public int Textures, NativeW, NativeH, AtlasW, AtlasH;
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
                var rect = new Dictionary<string, int[]>();    // m -> [x0,y0,w,h]
                int x = 0, y = 0, shelfH = 0;
                foreach (string m in order)
                {
                    int w = texW[m], h = texH[m];
                    if (x + w > atlasW) { x = 0; y += shelfH; shelfH = 0; }
                    rect[m] = new int[] { x, y, w, h };
                    x += w;
                    if (h > shelfH) shelfH = h;
                }
                int atlasH = y + shelfH;

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
                var remapped = new string[uvs.Count];
                for (int i = 0; i < uvs.Count; i++)
                {
                    string m; int[] rc;
                    if (owner.TryGetValue(i + 1, out m) && rect.TryGetValue(m, out rc))
                    {
                        double u = uvs[i][0], v = uvs[i][1];
                        double ax = (rc[0] + u * rc[2]) / atlasW;                 // atlas sample x, top-left origin
                        double ay = (rc[1] + (1.0 - v) * rc[3]) / atlasH;         // atlas sample y, top origin
                        double uo = su * ax;
                        double vo = 1.0 - sv * ay;
                        remapped[i] = "vt " + uo.ToString("F6", Inv) + " " + vo.ToString("F6", Inv);
                    }
                    else
                        remapped[i] = "vt " + uvs[i][0].ToString("F6", Inv) + " " + uvs[i][1].ToString("F6", Inv);
                }

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
    }
}
