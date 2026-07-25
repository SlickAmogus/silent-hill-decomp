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
        }

        /// <summary>Run the selected passes objPath -> outObjPath (mirror first so FixWinding
        /// re-derives the winding it reverses). No-op copy when nothing is selected.</summary>
        public static void Apply(string objPath, string outObjPath, Options opt)
        {
            if (opt == null) opt = new Options();
            bool any = false;
            if (opt.MirrorLR) { MirrorX(any ? outObjPath : objPath, outObjPath); any = true; }
            if (opt.FixWinding) { FixWinding(any ? outObjPath : objPath, outObjPath); any = true; }
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

        private static long EdgeKey(int a, int b)
        {
            int lo = a < b ? a : b, hi = a < b ? b : a;
            return ((long)lo << 32) | (uint)hi;
        }

        private static double D(string s) { return double.Parse(s, NumberStyles.Float, Inv); }
    }
}
