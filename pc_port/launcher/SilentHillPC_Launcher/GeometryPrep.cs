using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace SilentHillPC_Launcher
{
    /// <summary>Optional pre-atlas geometry passes for a rigged foreign model, ported from the
    /// Python atlas pipeline. Each is opt-in in the high-poly dialog: they change the mesh, and a
    /// careful rig does them better by hand. Winding is safe to always run; L/R mirror and the seam
    /// collar are heuristics.</summary>
    public static class GeometryPrep
    {
        private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

        /// <summary>Which pre-atlas geometry passes to run. Winding is on by default (safe); the
        /// L/R mirror is off by default (only a mis-named rig needs it, and it flips the texture).</summary>
        public class Options
        {
            public bool FixWinding = true;
            public bool MirrorLR = false;
            public bool CloseSeams = false;
            public double CollarMargin = 16.0;
        }

        /// <summary>Run the selected passes objPath -> outObjPath, in order: mirror (so FixWinding
        /// re-derives the winding it reverses), winding, then the seam collar. No-op copy when
        /// nothing is selected.</summary>
        public static void Apply(string objPath, string outObjPath, Options opt)
        {
            if (opt == null) opt = new Options();
            bool any = false;
            if (opt.MirrorLR) { MirrorX(any ? outObjPath : objPath, outObjPath); any = true; }
            if (opt.FixWinding) { FixWinding(any ? outObjPath : objPath, outObjPath); any = true; }
            if (opt.CloseSeams) { Collar(any ? outObjPath : objPath, outObjPath, opt.CollarMargin); any = true; }
            if (!any) File.Copy(objPath, outObjPath, true);
        }

        private class Face { public int Line; public string[] Tokens; public int[] V; public bool Flip; }

        /// <summary>Recalculate-Outside in code: reorder each face's corners so winding is CONSISTENT
        /// and OUTWARD per part (flood-fill an orientation per connected component, then flip a
        /// component whose faces point inward). The game backface-culls, so mixed winding shows as
        /// holes. Verts/UVs/normals are untouched — only face corner order. objPath may equal
        /// outObjPath. Ported from consistent_winding.</summary>
        public static void FixWinding(string objPath, string outObjPath)
        {
            var lines = new List<string>(File.ReadLines(objPath));
            var verts = new List<double[]>();
            var faces = new List<Face>();
            var partFaces = new List<List<Face>>();
            List<Face> cur = null;

            for (int li = 0; li < lines.Count; li++)
            {
                string t = lines[li].TrimStart();
                if (t.StartsWith("v ", StringComparison.Ordinal))
                {
                    string[] p = t.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    verts.Add(new double[] { D(p[1]), D(p[2]), D(p[3]) });
                }
                else if (t.StartsWith("o ", StringComparison.Ordinal))
                {
                    cur = new List<Face>();
                    partFaces.Add(cur);
                }
                else if (t.StartsWith("f ", StringComparison.Ordinal))
                {
                    string[] p = t.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    var corners = new string[p.Length - 1];
                    var vidx = new int[p.Length - 1];
                    for (int i = 1; i < p.Length; i++)
                    {
                        corners[i - 1] = p[i];
                        int slash = p[i].IndexOf('/');
                        string vs = slash < 0 ? p[i] : p[i].Substring(0, slash);
                        vidx[i - 1] = int.Parse(vs, Inv) - 1;
                    }
                    var f = new Face { Line = li, Tokens = corners, V = vidx };
                    faces.Add(f);
                    if (cur == null) { cur = new List<Face>(); partFaces.Add(cur); }
                    cur.Add(f);
                }
            }

            foreach (var pf in partFaces) FixPart(verts, pf);

            foreach (var f in faces)
                lines[f.Line] = "f " + string.Join(" ", f.Flip ? Rev(f.Tokens) : f.Tokens);
            File.WriteAllText(outObjPath, string.Join("\n", lines) + "\n", new UTF8Encoding(false));
        }

        private static void FixPart(List<double[]> verts, List<Face> pf)
        {
            int n = pf.Count;
            if (n == 0) return;

            var ef = new Dictionary<long, List<int[]>>();      // edge -> [(faceIdx, cornerI)]
            for (int fi = 0; fi < n; fi++)
            {
                int[] vs = pf[fi].V; int k = vs.Length;
                for (int i = 0; i < k; i++)
                {
                    long key = EdgeKey(vs[i], vs[(i + 1) % k]);
                    List<int[]> lst;
                    if (!ef.TryGetValue(key, out lst)) { lst = new List<int[]>(); ef[key] = lst; }
                    lst.Add(new int[] { fi, i });
                }
            }

            var flip = new bool[n];
            var visited = new bool[n];
            var comps = new List<List<int>>();
            for (int start = 0; start < n; start++)
            {
                if (visited[start]) continue;
                visited[start] = true;
                var q = new Queue<int>(); q.Enqueue(start);
                var comp = new List<int> { start };
                while (q.Count > 0)
                {
                    int fi = q.Dequeue();
                    int[] vs = pf[fi].V; int k = vs.Length;
                    for (int i = 0; i < k; i++)
                    {
                        int a = vs[i], b = vs[(i + 1) % k];
                        int da = flip[fi] ? b : a, db = flip[fi] ? a : b;    // fi's directed shared edge
                        foreach (int[] e in ef[EdgeKey(a, b)])
                        {
                            int nf = e[0], j = e[1];
                            if (nf == fi || visited[nf]) continue;
                            int[] nvs = pf[nf].V; int nk = nvs.Length;
                            int na = nvs[j], nb = nvs[(j + 1) % nk];
                            flip[nf] = !(na == db && nb == da);              // consistent = opposite dir
                            visited[nf] = true; q.Enqueue(nf); comp.Add(nf);
                        }
                    }
                }
                comps.Add(comp);
            }

            foreach (var comp in comps)
            {
                var vids = new HashSet<int>();
                foreach (int fi in comp) foreach (int vi in pf[fi].V) vids.Add(vi);
                double cx = 0, cy = 0, cz = 0;
                foreach (int vi in vids) { cx += verts[vi][0]; cy += verts[vi][1]; cz += verts[vi][2]; }
                cx /= vids.Count; cy /= vids.Count; cz /= vids.Count;

                double s = 0;
                foreach (int fi in comp)
                {
                    int[] ov = pf[fi].V; int k = ov.Length;
                    int i0, i1, i2;
                    if (flip[fi]) { i0 = ov[k - 1]; i1 = ov[k - 2]; i2 = ov[k - 3]; }
                    else { i0 = ov[0]; i1 = ov[1]; i2 = ov[2]; }
                    double[] a = verts[i0], b = verts[i1], c = verts[i2];
                    double nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
                    double ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
                    double nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                    double fx = (a[0] + b[0] + c[0]) / 3, fy = (a[1] + b[1] + c[1]) / 3, fz = (a[2] + b[2] + c[2]) / 3;
                    s += nx * (fx - cx) + ny * (fy - cy) + nz * (fz - cz);
                }
                // Only trust the outward test on components big enough for the centroid to mean
                // something; a small scrap keeps the flood-fill's (source) orientation.
                if (comp.Count >= 8 && s < 0)
                    foreach (int fi in comp) flip[fi] = !flip[fi];
            }

            for (int fi = 0; fi < n; fi++) pf[fi].Flip = flip[fi];
        }

        /// <summary>Mirror across X (flip vertex + normal X sign). Fixes a rig whose left/right was
        /// named backwards — geometry on the wrong side of its bone, which crosses the body under
        /// animation. Run FixWinding AFTER (the mirror reverses winding, which FixWinding re-derives).
        /// Caveat: this also flips the texture left-right (the whole model mirrors), so it's clean on
        /// a symmetric character but puts an asymmetric detail on the wrong side. objPath may equal
        /// outObjPath. Ported from mirror_obj.py.</summary>
        public static void MirrorX(string objPath, string outObjPath)
        {
            var outLines = new List<string>();
            foreach (string raw in File.ReadLines(objPath))
            {
                string t = raw.TrimStart();
                if (t.StartsWith("v ", StringComparison.Ordinal) || t.StartsWith("vn ", StringComparison.Ordinal))
                {
                    string[] p = t.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                    string xs = p[1].StartsWith("-", StringComparison.Ordinal) ? p[1].Substring(1) : "-" + p[1];
                    outLines.Add(p[0] + " " + xs + " " + p[2] + " " + p[3]);
                }
                else outLines.Add(raw);
            }
            File.WriteAllText(outObjPath, string.Join("\n", outLines) + "\n", new UTF8Encoding(false));
        }

        private static string[] Rev(string[] a)
        {
            var r = new string[a.Length];
            for (int i = 0; i < a.Length; i++) r[i] = a[a.Length - 1 - i];
            return r;
        }

        // ---- seam collar (opt-in, "rough") -------------------------------------------------
        private static readonly string[] TUBE = { "NECK", "SHOUL", "JOU", "ZEN", "HAND", "MOMO", "SUNE", "FOOT" };
        private static readonly Dictionary<int, int> PARENT = new Dictionary<int, int>
        {
            {1,0},{2,1},{3,1},{4,3},{5,4},{6,5},{7,1},{8,7},{9,8},{10,9},
            {11,0},{12,11},{13,12},{14,13},{15,11},{16,15},{17,16},{0,0}
        };

        private class CPart { public string Name; public string Mtl = "mat00_row00"; public List<int[][]> Faces = new List<int[][]>(); }

        /// <summary>ROUGH auto seam-close: extrude each tube part's PARENT-facing open loop toward its
        /// parent so it telescopes into the neighbour. Weaker than dragging verts by hand (opt-in;
        /// manual overlap is better). Emits a self-contained per-part OBJ. Ported from collar.py.</summary>
        public static void Collar(string objPath, string outObjPath, double margin)
        {
            var V = new List<double[]>(); var VT = new List<double[]>(); var VN = new List<double[]>();
            var parts = new List<CPart>(); CPart cur = null; string mtllib = null;
            foreach (string raw in File.ReadLines(objPath))
            {
                string[] p = raw.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length == 0) continue;
                if (p[0] == "mtllib") { if (p.Length > 1) mtllib = p[1]; }
                else if (p[0] == "v") V.Add(new[] { D(p[1]), D(p[2]), D(p[3]) });
                else if (p[0] == "vt") VT.Add(new[] { D(p[1]), D(p[2]) });
                else if (p[0] == "vn") VN.Add(new[] { D(p[1]), D(p[2]), D(p[3]) });
                else if (p[0] == "o") { cur = new CPart { Name = p.Length > 1 ? p[1] : "" }; parts.Add(cur); }
                else if (p[0] == "usemtl") { if (cur != null && p.Length > 1) cur.Mtl = p[1]; }
                else if (p[0] == "f" && cur != null)
                {
                    var f = new int[p.Length - 1][];
                    for (int i = 1; i < p.Length; i++)
                    {
                        string[] a = p[i].Split('/');
                        int vi = int.Parse(a[0], Inv) - 1;
                        int ti = a.Length > 1 && a[1].Length > 0 ? int.Parse(a[1], Inv) - 1 : -1;
                        int ni = a.Length > 2 && a[2].Length > 0 ? int.Parse(a[2], Inv) - 1 : -1;
                        f[i - 1] = new[] { vi, ti, ni };
                    }
                    cur.Faces.Add(f);
                }
            }

            var bonePts = new Dictionary<int, List<double[]>>();
            foreach (var part in parts)
            {
                double[] c = PartCentroid(V, part);
                if (c == null) continue;
                int b = BoneOfName(part.Name);
                if (!bonePts.ContainsKey(b)) bonePts[b] = new List<double[]>();
                bonePts[b].Add(c);
            }
            var boneC = new Dictionary<int, double[]>();
            foreach (var kv in bonePts)
            {
                var acc = new double[3];
                foreach (var pt in kv.Value) for (int k = 0; k < 3; k++) acc[k] += pt[k];
                for (int k = 0; k < 3; k++) acc[k] /= kv.Value.Count;
                boneC[kv.Key] = acc;
            }

            var outLines = new List<string>();
            if (mtllib != null) outLines.Add("mtllib " + mtllib);
            int gv = 0, gt = 0, gn = 0;
            foreach (var part in parts)
            {
                var collarV = new List<double[]>();
                var collarFaces = new List<int[][]>();
                int b = BoneOfName(part.Name);
                double[] pj = null; int pb;
                if (PARENT.TryGetValue(b, out pb)) boneC.TryGetValue(pb, out pj);
                bool isTube = false;
                foreach (var tk in TUBE) if (part.Name.Contains(tk)) { isTube = true; break; }

                if (isTube && part.Faces.Count > 0 && pj != null)
                {
                    var ec = new Dictionary<long, int>();
                    foreach (var f in part.Faces)
                        foreach (var t in Tri(f))
                            for (int e = 0; e < 3; e++)
                            {
                                int x = t[e][0], y = t[(e + 1) % 3][0];
                                if (x == y) continue;
                                long key = EdgeKey(x, y);
                                int cc; ec[key] = ec.TryGetValue(key, out cc) ? cc + 1 : 1;
                            }
                    var bedges = new List<int[]>();
                    foreach (var kv in ec)
                        if (kv.Value == 1) bedges.Add(new[] { (int)(kv.Key >> 32), (int)(kv.Key & 0xFFFFFFFFL) });

                    if (bedges.Count > 0)
                    {
                        var adj = new Dictionary<int, List<int>>();
                        foreach (var e in bedges)
                        {
                            if (!adj.ContainsKey(e[0])) adj[e[0]] = new List<int>();
                            if (!adj.ContainsKey(e[1])) adj[e[1]] = new List<int>();
                            adj[e[0]].Add(e[1]); adj[e[1]].Add(e[0]);
                        }
                        var seen = new HashSet<int>(); var loops = new List<HashSet<int>>();
                        foreach (int s0 in new List<int>(adj.Keys))
                        {
                            if (seen.Contains(s0)) continue;
                            var comp = new List<int>(); var st = new Stack<int>(); st.Push(s0);
                            while (st.Count > 0)
                            {
                                int n = st.Pop();
                                if (seen.Contains(n)) continue;
                                seen.Add(n); comp.Add(n);
                                foreach (int m in adj[n]) if (!seen.Contains(m)) st.Push(m);
                            }
                            if (comp.Count >= 3) loops.Add(new HashSet<int>(comp));
                        }
                        if (loops.Count > 0)
                        {
                            HashSet<int> loop = null; double best = double.MaxValue;
                            foreach (var lp in loops)
                            {
                                double[] lcc = LoopCentroid(V, lp); double dd = 0;
                                for (int k = 0; k < 3; k++) { double q = lcc[k] - pj[k]; dd += q * q; }
                                if (dd < best) { best = dd; loop = lp; }
                            }
                            double[] lc = LoopCentroid(V, loop);
                            var d = new double[3]; double dl = 0;
                            for (int k = 0; k < 3; k++) { d[k] = pj[k] - lc[k]; dl += d[k] * d[k]; }
                            dl = Math.Sqrt(dl); if (dl == 0) dl = 1;
                            for (int k = 0; k < 3; k++) d[k] /= dl;

                            var vtOf = new Dictionary<int, int>(); var vnOf = new Dictionary<int, int>();
                            foreach (var f in part.Faces)
                                foreach (var c in f)
                                {
                                    if (c[1] >= 0 && !vtOf.ContainsKey(c[0])) vtOf[c[0]] = c[1];
                                    if (c[2] >= 0 && !vnOf.ContainsKey(c[0])) vnOf[c[0]] = c[2];
                                }
                            var newof = new Dictionary<int, int>();
                            foreach (int vi in loop)
                            {
                                collarV.Add(new[] { V[vi][0] + d[0] * margin, V[vi][1] + d[1] * margin, V[vi][2] + d[2] * margin });
                                newof[vi] = -(collarV.Count);
                            }
                            foreach (var e in bedges)
                            {
                                int a = e[0], bb = e[1];
                                if (!loop.Contains(a) || !loop.Contains(bb)) continue;
                                int ta = vtOf.ContainsKey(a) ? vtOf[a] : -1, tb = vtOf.ContainsKey(bb) ? vtOf[bb] : -1;
                                int na = vnOf.ContainsKey(a) ? vnOf[a] : -1, nb = vnOf.ContainsKey(bb) ? vnOf[bb] : -1;
                                collarFaces.Add(new[] { new[] { a, ta, na }, new[] { bb, tb, nb }, new[] { newof[bb], tb, nb }, new[] { newof[a], ta, na } });
                                collarFaces.Add(new[] { new[] { newof[a], ta, na }, new[] { newof[bb], tb, nb }, new[] { bb, tb, nb }, new[] { a, ta, na } });
                            }
                        }
                    }
                }

                var lv = new Dictionary<string, int>(); var lt = new Dictionary<string, int>(); var lnn = new Dictionary<string, int>();
                var vlines = new List<string>(); var tlines = new List<string>(); var nlines = new List<string>();
                var fl = new List<string>();
                foreach (var f in part.Faces) fl.Add(EmitFace(f, lv, lt, lnn, vlines, tlines, nlines, V, VT, VN, collarV, gv, gt, gn));
                foreach (var q in collarFaces) fl.Add(EmitFace(q, lv, lt, lnn, vlines, tlines, nlines, V, VT, VN, collarV, gv, gt, gn));
                outLines.Add("o " + part.Name);
                outLines.AddRange(vlines); outLines.AddRange(tlines); outLines.AddRange(nlines);
                outLines.Add("usemtl " + part.Mtl);
                outLines.AddRange(fl);
                gv += vlines.Count; gt += tlines.Count; gn += nlines.Count;
            }
            File.WriteAllText(outObjPath, string.Join("\n", outLines) + "\n", new UTF8Encoding(false));
        }

        private static string EmitFace(int[][] f, Dictionary<string, int> lv, Dictionary<string, int> lt, Dictionary<string, int> lnn,
            List<string> vlines, List<string> tlines, List<string> nlines,
            List<double[]> V, List<double[]> VT, List<double[]> VN, List<double[]> collarV, int gv, int gt, int gn)
        {
            var sb = new StringBuilder("f");
            foreach (var c in f)
                sb.Append(" ").Append(LocV(lv, vlines, V, collarV, c[0]) + 1 + gv).Append("/")
                  .Append(LocT(lt, tlines, VT, c[1]) + 1 + gt).Append("/")
                  .Append(LocN(lnn, nlines, VN, c[2]) + 1 + gn);
            return sb.ToString();
        }

        private static int LocV(Dictionary<string, int> lv, List<string> vlines, List<double[]> V, List<double[]> collarV, int gi)
        {
            string k = gi < 0 ? "c" + gi : "g" + gi; int idx;
            if (!lv.TryGetValue(k, out idx))
            {
                idx = lv.Count; lv[k] = idx;
                double[] p = gi < 0 ? collarV[-gi - 1] : V[gi];
                vlines.Add("v " + F6(p[0]) + " " + F6(p[1]) + " " + F6(p[2]));
            }
            return idx;
        }
        private static int LocT(Dictionary<string, int> lt, List<string> tlines, List<double[]> VT, int ti)
        {
            if (ti < 0 || ti >= VT.Count) { int z; if (!lt.TryGetValue("z", out z)) { z = lt.Count; lt["z"] = z; tlines.Add("vt 0 0"); } return z; }
            string k = "t" + ti; int idx;
            if (!lt.TryGetValue(k, out idx)) { idx = lt.Count; lt[k] = idx; tlines.Add("vt " + F6(VT[ti][0]) + " " + F6(VT[ti][1])); }
            return idx;
        }
        private static int LocN(Dictionary<string, int> lnn, List<string> nlines, List<double[]> VN, int ni)
        {
            if (ni < 0 || ni >= VN.Count) { int z; if (!lnn.TryGetValue("z", out z)) { z = lnn.Count; lnn["z"] = z; nlines.Add("vn 0 1 0"); } return z; }
            string k = "n" + ni; int idx;
            if (!lnn.TryGetValue(k, out idx)) { idx = lnn.Count; lnn[k] = idx; nlines.Add("vn " + F6(VN[ni][0]) + " " + F6(VN[ni][1]) + " " + F6(VN[ni][2])); }
            return idx;
        }

        private static int BoneOfName(string name)
        {
            int i = 0; while (i < name.Length && name[i] >= '0' && name[i] <= '9') i++;
            int b; return (i > 0 && int.TryParse(name.Substring(0, i), NumberStyles.Integer, Inv, out b)) ? b : 0;
        }
        private static int[][][] Tri(int[][] f)
        {
            var ts = new int[f.Length - 2][][];
            for (int i = 1; i < f.Length - 1; i++) ts[i - 1] = new[] { f[0], f[i], f[i + 1] };
            return ts;
        }
        private static double[] PartCentroid(List<double[]> V, CPart part)
        {
            var vs = new HashSet<int>();
            foreach (var f in part.Faces) foreach (var c in f) vs.Add(c[0]);
            if (vs.Count == 0) return null;
            var acc = new double[3];
            foreach (int vi in vs) for (int k = 0; k < 3; k++) acc[k] += V[vi][k];
            for (int k = 0; k < 3; k++) acc[k] /= vs.Count;
            return acc;
        }
        private static double[] LoopCentroid(List<double[]> V, HashSet<int> loop)
        {
            var acc = new double[3];
            foreach (int vi in loop) for (int k = 0; k < 3; k++) acc[k] += V[vi][k];
            for (int k = 0; k < 3; k++) acc[k] /= loop.Count;
            return acc;
        }
        private static string F6(double v) { return v.ToString("F6", Inv); }

        private static long EdgeKey(int a, int b)
        {
            int lo = a < b ? a : b, hi = a < b ? b : a;
            return ((long)lo << 32) | (uint)hi;
        }

        private static double D(string s) { return double.Parse(s, NumberStyles.Float, Inv); }
    }
}
