using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Software-rendered model preview for the Mod Manager. Opens a .ILM (converted
    /// on the fly through IlmObjConverter) or an edited .OBJ directly, so an artist
    /// can check their edit BEFORE folding it back into an .ILM.
    ///
    /// The scene is deliberately built from the converter's own OBJ output rather
    /// than a private ILM parse: whatever this window shows is exactly what Blender
    /// will show, so the viewer doubles as a live check on the conversion itself.
    /// Textures come from ClutComposer.Compose — the one image that applies each
    /// body region's real palette row — so the preview matches the in-game look.
    ///
    /// Rendering is pure CPU + System.Drawing (the launcher has zero native or
    /// NuGet dependencies, and PSX models are a few hundred triangles, so a
    /// z-buffered software rasterizer is entirely sufficient).
    /// </summary>
    public class ModelViewerForm : Form
    {
        private readonly IlmViewScene _scene;
        private PictureBox _view;
        private CheckBox _chkTex, _chkWire;
        private float _yaw = 0.6f, _pitch = 0.25f;
        private float _dist, _panX, _panY;
        private readonly float _distHome;
        private Point _last;
        private MouseButtons _drag = MouseButtons.None;

        /// <summary>Open a viewer for an .ILM or .OBJ, reporting failure via MessageBox
        /// instead of letting an exception escape into the caller's Click handler.</summary>
        public static void Open(IWin32Window owner, string path)
        {
            IlmViewScene scene;
            try
            {
                string err;
                scene = IlmViewScene.Load(path, out err);
                if (scene == null)
                {
                    MessageBox.Show(owner, "Could not open the model:\n\n" + err,
                        "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(owner, "Could not open the model:\n\n" + ex.Message,
                    "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            var f = new ModelViewerForm(scene);
            f.Show(owner as Form);
            if (scene.Warnings.Count > 0)
                MessageBox.Show(f, string.Join("\n\n", scene.Warnings.ToArray()),
                    "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

        private ModelViewerForm(IlmViewScene scene)
        {
            _scene = scene;
            _dist = _distHome = scene.Radius * 2.6f + 1.0f;

            Text = "Model Viewer — " + scene.Title;
            ClientSize = new Size(800, 600);
            MinimumSize = new Size(480, 360);
            StartPosition = FormStartPosition.CenterParent;
            KeyPreview = true;

            var bar = new Panel { Dock = DockStyle.Top, Height = 30 };
            _chkTex = new CheckBox
            {
                Text = "Textured",
                Location = new Point(8, 6),
                AutoSize = true,
                Checked = scene.HasTexture,
                Enabled = scene.HasTexture
            };
            _chkWire = new CheckBox { Text = "Wireframe", Location = new Point(92, 6), AutoSize = true };
            var btnReset = new Button { Text = "Reset View", Location = new Point(188, 3), Size = new Size(80, 24) };
            var lbl = new Label
            {
                Text = scene.Parts + " parts   " + scene.VertexCount + " verts   " + scene.Tris.Count + " tris" +
                       "   |   drag: orbit    right-drag: pan    wheel: zoom",
                Location = new Point(282, 8),
                AutoSize = true,
                ForeColor = SystemColors.GrayText
            };
            _chkTex.CheckedChanged += (s, e) => Render();
            _chkWire.CheckedChanged += (s, e) => Render();
            btnReset.Click += (s, e) => ResetView();
            bar.Controls.Add(_chkTex);
            bar.Controls.Add(_chkWire);
            bar.Controls.Add(btnReset);
            bar.Controls.Add(lbl);

            _view = new PictureBox { Dock = DockStyle.Fill, BackColor = Color.FromArgb(48, 48, 52) };
            _view.MouseDown += (s, e) => { _drag = e.Button; _last = e.Location; };
            _view.MouseUp += (s, e) => { _drag = MouseButtons.None; };
            _view.MouseMove += OnViewMouseMove;
            _view.MouseDoubleClick += (s, e) => ResetView();
            MouseWheel += OnViewWheel;
            Resize += (s, e) => Render();

            Controls.Add(_view);
            Controls.Add(bar);
            Shown += (s, e) => Render();
        }

        private void ResetView()
        {
            _yaw = 0.6f; _pitch = 0.25f; _dist = _distHome; _panX = 0; _panY = 0;
            Render();
        }

        private void OnViewMouseMove(object s, MouseEventArgs e)
        {
            if (_drag == MouseButtons.None) return;
            int dx = e.X - _last.X, dy = e.Y - _last.Y;
            _last = e.Location;
            if (_drag == MouseButtons.Left)
            {
                _yaw += dx * 0.011f;
                _pitch += dy * 0.011f;
                if (_pitch > 1.55f) _pitch = 1.55f;
                if (_pitch < -1.55f) _pitch = -1.55f;
            }
            else
            {
                // Scale pan so the model tracks the cursor at any zoom.
                float k = _dist * 0.0016f;
                _panX += dx * k;
                _panY -= dy * k;
            }
            Render();
        }

        private void OnViewWheel(object s, MouseEventArgs e)
        {
            _dist *= (float)Math.Pow(0.88, e.Delta / 120.0);
            float lo = _scene.Radius * 0.3f + 0.1f, hi = _distHome * 8f;
            if (_dist < lo) _dist = lo;
            if (_dist > hi) _dist = hi;
            Render();
        }

        private void Render()
        {
            int w = _view.ClientSize.Width, h = _view.ClientSize.Height;
            if (w < 8 || h < 8) return;
            var bmp = IlmSoftRenderer.Render(_scene, w, h, _yaw, _pitch, _dist, _panX, _panY,
                _chkTex.Checked, _chkWire.Checked);
            var old = _view.Image;
            _view.Image = bmp;
            if (old != null) old.Dispose();
        }
    }

    /// <summary>Geometry + texture for the viewer, loaded from a .ILM (via export)
    /// or a .OBJ. Kept separate from the Form so a headless harness can render it.</summary>
    internal sealed class IlmViewScene
    {
        public struct Tri
        {
            public int V0, V1, V2;
            public float U0, Vv0, U1, Vv1, U2, Vv2;
            public bool Alpha, HasUv;
        }

        public string Title;
        public int Parts, VertexCount;
        public float[] Vx, Vy, Vz;
        public readonly List<Tri> Tris = new List<Tri>();
        public readonly List<int[]> Edges = new List<int[]>(); // unique [a,b] vertex pairs for wireframe
        public readonly List<string> Warnings = new List<string>();
        public int[] TexPix; // ARGB
        public int TexW, TexH;
        public bool HasTexture { get { return TexPix != null; } }
        public float Cx, Cy, Cz, Radius;

        public static IlmViewScene Load(string path, out string error)
        {
            error = null;
            string ext = Path.GetExtension(path);
            string obj = path, ilm = null;
            var warnings = new List<string>();

            if (string.Equals(ext, ".ilm", StringComparison.OrdinalIgnoreCase))
            {
                ilm = path;
                string dir = Path.Combine(Path.GetTempPath(), "SHPC_ModelViewer");
                Directory.CreateDirectory(dir);
                obj = Path.Combine(dir, Path.GetFileNameWithoutExtension(path) + ".obj");
                var res = IlmObjConverter.Export(ilm, obj);
                if (res == null || !string.IsNullOrEmpty(res.Error))
                {
                    error = res != null ? res.Error : "export failed";
                    return null;
                }
                foreach (string wmsg in res.Warnings) warnings.Add(wmsg);
            }
            else if (string.Equals(ext, ".obj", StringComparison.OrdinalIgnoreCase))
            {
                // An edited OBJ has no texture reference of its own; the original
                // .ILM (and its .TIM) normally sit beside it after an export.
                string cand = Path.ChangeExtension(path, ".ILM");
                if (File.Exists(cand)) ilm = cand;
                else { cand = Path.ChangeExtension(path, ".ilm"); if (File.Exists(cand)) ilm = cand; }
            }
            else
            {
                error = "not a .ILM or .OBJ: " + Path.GetFileName(path);
                return null;
            }

            var sc = ParseObj(obj, out error);
            if (sc == null) return null;
            sc.Title = Path.GetFileName(path);
            sc.Warnings.AddRange(warnings);

            if (ilm != null)
            {
                string dir = Path.Combine(Path.GetTempPath(), "SHPC_ModelViewer");
                Directory.CreateDirectory(dir);
                string png = Path.Combine(dir, Path.GetFileNameWithoutExtension(ilm) + "_sheet.png");
                string terr;
                if (ClutComposer.Compose(ilm, null, png, out terr))
                    sc.LoadTexture(png);
                // No TIM beside the ILM is normal for a handful of models — just view untextured.
            }
            return sc;
        }

        private static IlmViewScene ParseObj(string path, out string error)
        {
            error = null;
            var sc = new IlmViewScene();
            var vx = new List<float>(); var vy = new List<float>(); var vz = new List<float>();
            var tu = new List<float>(); var tv = new List<float>();
            bool alpha = false;
            int parts = 0;
            var edgeSeen = new HashSet<long>();
            var inv = CultureInfo.InvariantCulture;

            foreach (string raw in File.ReadLines(path))
            {
                string ln = raw.Trim();
                if (ln.Length == 0 || ln[0] == '#') continue;
                string[] p = ln.Split((char[])null, StringSplitOptions.RemoveEmptyEntries);
                if (p[0] == "o") { parts++; }
                else if (p[0] == "v" && p.Length >= 4)
                {
                    vx.Add(float.Parse(p[1], inv)); vy.Add(float.Parse(p[2], inv)); vz.Add(float.Parse(p[3], inv));
                }
                else if (p[0] == "vt" && p.Length >= 3)
                {
                    tu.Add(float.Parse(p[1], inv)); tv.Add(float.Parse(p[2], inv));
                }
                else if (p[0] == "usemtl")
                {
                    alpha = p[1].EndsWith("_alpha", StringComparison.Ordinal);
                }
                else if (p[0] == "f" && p.Length >= 4)
                {
                    int n = p.Length - 1;
                    var vi = new int[n]; var ti = new int[n];
                    for (int i = 0; i < n; i++)
                    {
                        string[] c = p[i + 1].Split('/');
                        vi[i] = int.Parse(c[0], inv) - 1;
                        ti[i] = (c.Length > 1 && c[1].Length > 0) ? int.Parse(c[1], inv) - 1 : -1;
                        if (vi[i] < 0 || vi[i] >= vx.Count) { error = "face references vertex " + (vi[i] + 1) + " which does not exist"; return null; }
                    }
                    // Fan-triangulate the polygon loop; OBJ corner order is already a loop.
                    for (int i = 1; i + 1 < n; i++)
                    {
                        var t = new Tri { V0 = vi[0], V1 = vi[i], V2 = vi[i + 1], Alpha = alpha };
                        if (ti[0] >= 0 && ti[i] >= 0 && ti[i + 1] >= 0 &&
                            ti[0] < tu.Count && ti[i] < tu.Count && ti[i + 1] < tu.Count)
                        {
                            t.HasUv = true;
                            t.U0 = tu[ti[0]]; t.Vv0 = tv[ti[0]];
                            t.U1 = tu[ti[i]]; t.Vv1 = tv[ti[i]];
                            t.U2 = tu[ti[i + 1]]; t.Vv2 = tv[ti[i + 1]];
                        }
                        sc.Tris.Add(t);
                    }
                    for (int i = 0; i < n; i++)
                    {
                        int a = vi[i], b = vi[(i + 1) % n];
                        long key = a < b ? ((long)a << 32) | (uint)b : ((long)b << 32) | (uint)a;
                        if (edgeSeen.Add(key)) sc.Edges.Add(new[] { a, b });
                    }
                }
            }
            if (vx.Count == 0 || sc.Tris.Count == 0) { error = "no geometry found in " + Path.GetFileName(path); return null; }

            sc.Vx = vx.ToArray(); sc.Vy = vy.ToArray(); sc.Vz = vz.ToArray();
            sc.VertexCount = vx.Count;
            sc.Parts = parts > 0 ? parts : 1;

            float minx = float.MaxValue, miny = float.MaxValue, minz = float.MaxValue;
            float maxx = float.MinValue, maxy = float.MinValue, maxz = float.MinValue;
            for (int i = 0; i < sc.VertexCount; i++)
            {
                if (sc.Vx[i] < minx) minx = sc.Vx[i]; if (sc.Vx[i] > maxx) maxx = sc.Vx[i];
                if (sc.Vy[i] < miny) miny = sc.Vy[i]; if (sc.Vy[i] > maxy) maxy = sc.Vy[i];
                if (sc.Vz[i] < minz) minz = sc.Vz[i]; if (sc.Vz[i] > maxz) maxz = sc.Vz[i];
            }
            sc.Cx = (minx + maxx) * 0.5f; sc.Cy = (miny + maxy) * 0.5f; sc.Cz = (minz + maxz) * 0.5f;
            float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
            sc.Radius = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
            if (sc.Radius < 1e-3f) sc.Radius = 1f;
            return sc;
        }

        private void LoadTexture(string png)
        {
            using (var src = new Bitmap(png))
            using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(bmp)) g.DrawImageUnscaled(src, 0, 0);
                TexW = bmp.Width; TexH = bmp.Height;
                var bd = bmp.LockBits(new Rectangle(0, 0, TexW, TexH), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                try
                {
                    TexPix = new int[TexW * TexH];
                    for (int y = 0; y < TexH; y++)
                        Marshal.Copy(bd.Scan0 + y * bd.Stride, TexPix, y * TexW, TexW);
                }
                finally { bmp.UnlockBits(bd); }
            }
        }
    }

    /// <summary>Z-buffered software rasterizer over an IlmViewScene. Static and
    /// windowless on purpose — the offscreen harness renders through the same code
    /// the form shows, so a saved PNG is proof of what the user will see.</summary>
    internal static class IlmSoftRenderer
    {
        public static Bitmap Render(IlmViewScene sc, int w, int h,
            float yaw, float pitch, float dist, float panX, float panY,
            bool textured, bool wireframe)
        {
            var color = new int[w * h];
            var depth = new float[w * h];
            int bg = unchecked((int)0xFF303034);
            for (int i = 0; i < color.Length; i++) { color[i] = bg; depth[i] = float.MaxValue; }

            int n = sc.VertexCount;
            var xs = new float[n]; var ys = new float[n]; var zs = new float[n]; // view space
            var px = new float[n]; var py = new float[n];                         // screen
            float cy = (float)Math.Cos(yaw), sy = (float)Math.Sin(yaw);
            float cp = (float)Math.Cos(pitch), sp = (float)Math.Sin(pitch);
            float f = (h * 0.5f) / (float)Math.Tan(25.0 * Math.PI / 180.0); // 50 deg vertical FOV
            float near = Math.Max(dist * 0.02f, 0.5f);

            for (int i = 0; i < n; i++)
            {
                float x = sc.Vx[i] - sc.Cx, y = sc.Vy[i] - sc.Cy, z = sc.Vz[i] - sc.Cz;
                float x1 = x * cy + z * sy;
                float z1 = -x * sy + z * cy;
                float y2 = y * cp - z1 * sp;
                float z2 = y * sp + z1 * cp;
                xs[i] = x1 + panX; ys[i] = y2 + panY; zs[i] = z2 + dist;
                if (zs[i] > near)
                {
                    px[i] = w * 0.5f + xs[i] * f / zs[i];
                    py[i] = h * 0.5f - ys[i] * f / zs[i];
                }
            }

            var deferred = new List<KeyValuePair<float, int>>(); // alpha tris, sorted far-to-near
            for (int t = 0; t < sc.Tris.Count; t++)
            {
                var tr = sc.Tris[t];
                if (zs[tr.V0] <= near || zs[tr.V1] <= near || zs[tr.V2] <= near) continue;
                if (tr.Alpha)
                {
                    deferred.Add(new KeyValuePair<float, int>(-(zs[tr.V0] + zs[tr.V1] + zs[tr.V2]), t));
                    continue;
                }
                Fill(sc, tr, color, depth, w, h, xs, ys, zs, px, py, textured, false);
            }
            deferred.Sort((a, b) => a.Key.CompareTo(b.Key));
            foreach (var kv in deferred)
                Fill(sc, sc.Tris[kv.Value], color, depth, w, h, xs, ys, zs, px, py, textured, true);

            var bmp = new Bitmap(w, h, PixelFormat.Format32bppRgb);
            var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
            try
            {
                for (int y = 0; y < h; y++)
                    Marshal.Copy(color, y * w, bd.Scan0 + y * bd.Stride, w);
            }
            finally { bmp.UnlockBits(bd); }

            if (wireframe)
            {
                using (var g = Graphics.FromImage(bmp))
                using (var pen = new Pen(Color.FromArgb(150, 255, 158, 44)))
                    foreach (var e in sc.Edges)
                    {
                        if (zs[e[0]] <= near || zs[e[1]] <= near) continue;
                        g.DrawLine(pen, px[e[0]], py[e[0]], px[e[1]], py[e[1]]);
                    }
            }
            return bmp;
        }

        private static void Fill(IlmViewScene sc, IlmViewScene.Tri tr, int[] color, float[] depth,
            int w, int h, float[] xs, float[] ys, float[] zs, float[] px, float[] py,
            bool textured, bool blend)
        {
            float x0 = px[tr.V0], y0 = py[tr.V0], z0 = zs[tr.V0];
            float x1 = px[tr.V1], y1 = py[tr.V1], z1 = zs[tr.V1];
            float x2 = px[tr.V2], y2 = py[tr.V2], z2 = zs[tr.V2];

            float denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
            if (denom > -1e-6f && denom < 1e-6f) return;
            float inv = 1f / denom;

            // Headlight shading off the view-space face normal; abs() lights both
            // sides so un-culled interior faces stay readable instead of going black.
            float ax = xs[tr.V1] - xs[tr.V0], ay = ys[tr.V1] - ys[tr.V0], az = zs[tr.V1] - zs[tr.V0];
            float bx = xs[tr.V2] - xs[tr.V0], by = ys[tr.V2] - ys[tr.V0], bz = zs[tr.V2] - zs[tr.V0];
            float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
            float nl = (float)Math.Sqrt(nx * nx + ny * ny + nz * nz);
            int shade = nl < 1e-9f ? 96 : (int)(255f * (0.35f + 0.65f * Math.Abs(nz / nl)));

            bool tex = textured && tr.HasUv && sc.TexPix != null;
            int flat = unchecked((int)0xFFB9B9BC);

            int minx = (int)Math.Max(0, Math.Floor(Math.Min(x0, Math.Min(x1, x2))));
            int maxx = (int)Math.Min(w - 1, Math.Ceiling(Math.Max(x0, Math.Max(x1, x2))));
            int miny = (int)Math.Max(0, Math.Floor(Math.Min(y0, Math.Min(y1, y2))));
            int maxy = (int)Math.Min(h - 1, Math.Ceiling(Math.Max(y0, Math.Max(y1, y2))));

            for (int yy = miny; yy <= maxy; yy++)
            {
                float fy = yy + 0.5f;
                for (int xx = minx; xx <= maxx; xx++)
                {
                    float fx = xx + 0.5f;
                    float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) * inv;
                    float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) * inv;
                    float w2 = 1f - w0 - w1;
                    if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;

                    int idx = yy * w + xx;
                    float z = w0 * z0 + w1 * z1 + w2 * z2;
                    if (z >= depth[idx]) continue;

                    int argb;
                    if (tex)
                    {
                        // vt = ((u+0.5)/256, 1-(v+0.5)/256): UV space is the 256-texel
                        // PSX page regardless of the sheet's pixel size.
                        float uu = w0 * tr.U0 + w1 * tr.U1 + w2 * tr.U2;
                        float vv = w0 * tr.Vv0 + w1 * tr.Vv1 + w2 * tr.Vv2;
                        int tx = (int)(uu * 256f);
                        int ty = (int)((1f - vv) * 256f);
                        if (tx < 0) tx = 0; if (tx >= sc.TexW) tx = sc.TexW - 1;
                        if (ty < 0) ty = 0; if (ty >= sc.TexH) ty = sc.TexH - 1;
                        argb = sc.TexPix[ty * sc.TexW + tx];
                        if (((argb >> 24) & 0xFF) < 8) continue; // transparent texel, like the PSX
                    }
                    else argb = flat;

                    int r = (((argb >> 16) & 0xFF) * shade) >> 8;
                    int gc = (((argb >> 8) & 0xFF) * shade) >> 8;
                    int b = ((argb & 0xFF) * shade) >> 8;
                    if (blend)
                    {
                        int dst = color[idx];
                        r = (r + ((dst >> 16) & 0xFF)) >> 1;
                        gc = (gc + ((dst >> 8) & 0xFF)) >> 1;
                        b = (b + (dst & 0xFF)) >> 1;
                        color[idx] = unchecked((int)0xFF000000) | (r << 16) | (gc << 8) | b;
                        // No depth write: semi-transparent prims never occlude, matching the game.
                    }
                    else
                    {
                        color[idx] = unchecked((int)0xFF000000) | (r << 16) | (gc << 8) | b;
                        depth[idx] = z;
                    }
                }
            }
        }
    }
}
