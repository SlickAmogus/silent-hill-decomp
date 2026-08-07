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
    /// Software-rendered model viewer. ONE reusable window: opening another model
    /// (File > Open, drag &amp; drop, or a second "Model Viewer" click) loads into the
    /// same window instead of spawning a new one. Opens character models (.ILM),
    /// prop/item models (.PLM), PSX item TMDs (.TMD) and edited .OBJ files, and
    /// plays .ANM skeletal animation on character models with a scrub timeline.
    ///
    /// Character/prop scenes come from IlmObjConverter.BuildAnimScene — the same
    /// parser and scratch-pool weld replay the converter itself uses, so what this
    /// window shows in motion is the format's real behaviour. .OBJ files render
    /// as-is, so the viewer still doubles as a live check on a Blender round trip.
    ///
    /// Pose math is AnmFile — the engine-exact sampler (component-lerped q12
    /// matrices, shared translation slot 0, rootYOffset) — NOT a generic skeletal
    /// animator; in-between frames are deliberately non-orthonormal like the GTE's.
    ///
    /// Rendering is pure CPU + System.Drawing (the launcher has zero native or
    /// NuGet dependencies; PSX models are small, so a z-buffered software
    /// rasterizer is sufficient even at animation rates).
    /// </summary>
    public class ModelViewerForm : Form
    {
        private static ModelViewerForm s_open; // the one reusable window

        private IlmViewScene _scene;                // null until a model is opened
        private IlmObjConverter.AnimScene _anim;    // null for .OBJ / .TMD scenes
        private string _loadedPath;                 // what LoadPath opened; TMD export needs it
        private bool _loadedIsTmd;
        private string _gameRoot;                   // null when opened without one
        private int[] _vertPart, _vertLocal;        // scene vertex -> (part, local)

        private PictureBox _view;
        private CheckBox _chkTex, _chkWire;
        private Label _lblInfo;
        private ToolStripMenuItem _miExportThis;
        private float _yaw = 0.6f, _pitch = 0.25f;
        private float _dist, _panX, _panY;
        private float _distHome;
        private Point _last;
        private MouseButtons _drag = MouseButtons.None;

        // ---- animation state ----
        private AnmFile _anm2;         // loaded ANM, null when none
        private string _anmPath;
        private double _time;          // fractional keyframe cursor
        private bool _playing;
        private Timer _timer;
        private TrackBar _bar;
        private Button _btnPlay;
        private Label _lblKf;
        private ComboBox _cmbSpeed;
        private DateTime _lastTick;
        private bool _barFromCode;

        public static void Open(IWin32Window owner, string path)
        {
            Open(owner, path, null);
        }

        /// <summary>Show the viewer (reusing the open window when there is one) and
        /// load <paramref name="path"/> into it — or just show it empty when path is
        /// null, ready for File > Open / drag &amp; drop. Load failures report via
        /// MessageBox; the window stays up either way.</summary>
        public static void Open(IWin32Window owner, string path, string gameRoot)
        {
            ModelViewerForm f = s_open;
            if (f == null || f.IsDisposed)
            {
                f = new ModelViewerForm(gameRoot);
                s_open = f;
                f.FormClosed += (s, e) => { if (s_open == f) s_open = null; };
                f.Show(owner as Form);
            }
            else
            {
                if (gameRoot != null) f._gameRoot = gameRoot;
                if (f.WindowState == FormWindowState.Minimized) f.WindowState = FormWindowState.Normal;
                f.Activate();
            }
            if (path != null) f.LoadPath(path);
        }

        private ModelViewerForm(string gameRoot)
        {
            _gameRoot = gameRoot;

            Text = "Model Viewer";
            ClientSize = new Size(800, 640);
            MinimumSize = new Size(520, 400);
            StartPosition = FormStartPosition.CenterParent;
            KeyPreview = true;
            AllowDrop = true;
            DragEnter += OnDragEnter;
            DragDrop += OnDragDrop;

            var menu = BuildMenu();

            var bar = new Panel { Dock = DockStyle.Top, Height = 30 };
            _chkTex = new CheckBox { Text = "Textured", Location = new Point(8, 6), AutoSize = true, Enabled = false };
            _chkWire = new CheckBox { Text = "Wireframe", Location = new Point(92, 6), AutoSize = true };
            var btnReset = new Button { Text = "Reset View", Location = new Point(188, 3), Size = new Size(80, 24) };
            _lblInfo = new Label
            {
                Text = "File > Open a model, or drop one here",
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
            bar.Controls.Add(_lblInfo);

            _view = new PictureBox { Dock = DockStyle.Fill, BackColor = Color.FromArgb(48, 48, 52) };
            _view.MouseDown += (s, e) => { _drag = e.Button; _last = e.Location; };
            _view.MouseUp += (s, e) => { _drag = MouseButtons.None; };
            _view.MouseMove += OnViewMouseMove;
            _view.MouseDoubleClick += (s, e) => ResetView();
            MouseWheel += OnViewWheel;
            Resize += (s, e) => Render();

            var animPanel = BuildAnimPanel();

            Controls.Add(_view);
            Controls.Add(animPanel);
            Controls.Add(bar);
            Controls.Add(menu);
            MainMenuStrip = menu;

            Shown += (s, e) => Render();
            FormClosed += (s, e) => { if (_timer != null) _timer.Dispose(); };
        }

        // ---- open / load ----------------------------------------------------------

        private static readonly string[] OpenableExts = { ".ILM", ".PLM", ".TMD", ".OBJ" };

        private static bool IsOpenable(string path)
        {
            string ext = (Path.GetExtension(path) ?? "").ToUpperInvariant();
            return Array.IndexOf(OpenableExts, ext) >= 0;
        }

        /// <summary>Load a model into THIS window. On failure the previous scene (or
        /// the empty state) stays; the error goes to a MessageBox.</summary>
        public void LoadPath(string path)
        {
            IlmViewScene scene = null;
            IlmObjConverter.AnimScene anim = null;
            try
            {
                string ext = (Path.GetExtension(path) ?? "").ToUpperInvariant();
                string err = null;
                if (ext == ".OBJ")
                {
                    scene = IlmViewScene.Load(path, out err);
                }
                else if (ext == ".TMD")
                {
                    // FromTmd passes no page-14 preference, so the builder derives it
                    // from the filename (TmdViewSceneBuilder.StockTpage14ForFile).
                    scene = IlmViewScene.FromTmd(path, out err);
                }
                else // .ILM / .PLM (anything with the LM magic)
                {
                    anim = IlmObjConverter.BuildAnimScene(path, null);
                    scene = IlmViewScene.FromAnimScene(anim, out err);
                }
                if (scene == null)
                {
                    MessageBox.Show(this, "Could not open the model:\n\n" + (err ?? "unknown error"),
                        "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not open the model:\n\n" + ex.Message,
                    "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            _scene = scene;
            _anim = anim;
            _vertPart = null;
            _vertLocal = null;
            _anm2 = null;
            _anmPath = null;
            _time = 0;
            _playing = false;
            _btnPlay.Enabled = false;
            _btnPlay.Text = "Play";
            _bar.Enabled = false;
            _lblKf.Text = "no ANM";
            _distHome = scene.Radius * 2.6f + 1.0f;
            _chkTex.Checked = scene.HasTexture;
            _chkTex.Enabled = scene.HasTexture;
            _loadedPath = path;
            _loadedIsTmd = string.Equals(Path.GetExtension(path), ".TMD", StringComparison.OrdinalIgnoreCase);
            // A TMD scene carries no AnimScene (it has no bones), so the export item
            // used to stay greyed out on exactly the models TmdObjConverter handles.
            _miExportThis.Enabled = (_anim != null && _anim.IlmPath != null) || _loadedIsTmd;
            _lblInfo.Text = scene.Parts + " parts   " + scene.VertexCount + " verts   " + scene.Tris.Count + " tris" +
                            "   |   drag: orbit    right-drag: pan    wheel: zoom";
            Text = "Model Viewer — " + scene.Title;

            if (_anim != null)
            {
                BuildVertexMap();
                if (_anim.AnmPath != null) LoadAnm(_anim.AnmPath, false);
                ApplyPose();
            }
            ResetView();

            if (scene.Warnings.Count > 0)
                MessageBox.Show(this, string.Join("\n\n", scene.Warnings.ToArray()),
                    "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

        private void OnDragEnter(object s, DragEventArgs e)
        {
            if (e.Data.GetDataPresent(DataFormats.FileDrop))
            {
                var files = (string[])e.Data.GetData(DataFormats.FileDrop);
                if (files != null && files.Length > 0 && IsOpenable(files[0]))
                {
                    e.Effect = DragDropEffects.Copy;
                    return;
                }
            }
            e.Effect = DragDropEffects.None;
        }

        private void OnDragDrop(object s, DragEventArgs e)
        {
            var files = (string[])e.Data.GetData(DataFormats.FileDrop);
            if (files != null && files.Length > 0 && IsOpenable(files[0]))
                LoadPath(files[0]);
        }

        // ---- menu ----------------------------------------------------------------

        private MenuStrip BuildMenu()
        {
            var menu = new MenuStrip();

            var file = new ToolStripMenuItem("&File");
            file.DropDownItems.Add("&Open Model…", null, (s, e) => OnOpenAnother());
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("&Close", null, (s, e) => Close());

            var conv = new ToolStripMenuItem("&Convert");
            _miExportThis = new ToolStripMenuItem("Export &This Model → OBJ…");
            _miExportThis.Click += (s, e) =>
            {
                if (_loadedIsTmd) ConverterActions.ExportTmdFrom(this, _loadedPath);
                else ConverterActions.ExportModelFrom(this, _anim.IlmPath);
            };
            _miExportThis.Enabled = false;
            conv.DropDownItems.Add(_miExportThis);
            conv.DropDownItems.Add("&Model → OBJ…", null, (s, e) => ConverterActions.ExportModel(this, RootOrGuess()));
            conv.DropDownItems.Add("&OBJ → Model (high-poly)…", null, (s, e) => ConverterActions.HighPolyImport(this, RootOrGuess()));
            conv.DropDownItems.Add("&Simple OBJ → Model…", null, (s, e) => ConverterActions.SimpleImport(this, RootOrGuess()));
            conv.DropDownItems.Add(new ToolStripSeparator());
            conv.DropDownItems.Add("&TMD → OBJ… (item models)", null, (s, e) => ConverterActions.ExportTmd(this, RootOrGuess()));
            conv.DropDownItems.Add("OBJ → &TMD (reshape)…", null, (s, e) => ConverterActions.ImportTmd(this, RootOrGuess()));
            conv.DropDownItems.Add("OBJ → TMD (&replace)…", null, (s, e) => ConverterActions.RebuildTmd(this, RootOrGuess()));

            var animM = new ToolStripMenuItem("&Animation");
            animM.DropDownItems.Add("Choose &ANM…", null, (s, e) => OnChooseAnm());
            animM.DropDownItems.Add(new ToolStripSeparator());
            animM.DropDownItems.Add("Export ANM → editable &JSON…", null, (s, e) => OnExportAnmJson());
            animM.DropDownItems.Add("&Import JSON → ANM…", null, (s, e) => OnImportAnmJson());

            var help = new ToolStripMenuItem("&Help");
            help.DropDownItems.Add("&About the viewer…", null, (s, e) => ShowHelp());

            menu.Items.Add(file);
            menu.Items.Add(conv);
            menu.Items.Add(animM);
            menu.Items.Add(help);
            return menu;
        }

        /// <summary>The Convert flows want the game root for their initial dialogs; when the
        /// viewer was opened without one, fall back to the opened file's neighbourhood.</summary>
        private string RootOrGuess()
        {
            if (!string.IsNullOrEmpty(_gameRoot)) return _gameRoot;
            try
            {
                string d = _anim != null ? Path.GetDirectoryName(_anim.IlmPath) : null;
                return d ?? ".";
            }
            catch { return "."; }
        }

        private void OnOpenAnother()
        {
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a model to view";
                ofd.Filter = "Models (*.ilm;*.plm;*.tmd;*.obj)|*.ilm;*.plm;*.tmd;*.obj|All files (*.*)|*.*";
                try
                {
                    if (_anim != null) ofd.InitialDirectory = Path.GetDirectoryName(_anim.IlmPath);
                    else if (!string.IsNullOrEmpty(_gameRoot))
                    {
                        string gd = Path.Combine(_gameRoot, "gamedata");
                        if (Directory.Exists(gd)) ofd.InitialDirectory = gd;
                    }
                }
                catch { }
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                LoadPath(ofd.FileName);
            }
        }

        private void ShowHelp()
        {
            string[] lines =
            {
                "MODEL VIEWER",
                "",
                "Open models with File > Open, or drag & drop a file onto the window.",
                "The window is reused — opening another model replaces the current one.",
                "",
                "Opens:",
                "  .ILM   character models — animated when their .ANM is found (the",
                "         ANIM folder beside CHARA, like the extracted disc layout).",
                "  .PLM   prop models — weapons/items (ITEM), and each area's global",
                "         world props (BG\\*_GLB.PLM: doors, signs, fences, building",
                "         shells the map instances). Static, identity pose.",
                "  .TMD   PSX inventory item models (ITEM folder).",
                "  .OBJ   an edited export, exactly as Blender sees it.",
                "",
                "ANIMATION (character models)",
                "  The timeline scrubs the .ANM's whole keyframe pool. Play speed 1x is",
                "  the game's native 30 keyframes per second. The pose sampler is the",
                "  engine's own math — joints weld exactly where the game welds them.",
                "  \"Choose ANM…\" swaps in another animation file (an ANM fits when its",
                "  bone count covers the model's part digits).",
                "",
                "  Harry note: HB_BASE.ANM is his base movement set. The weapon and",
                "  per-map animations live in separate continuation files (HB_WEP*,",
                "  HB_M*) that only make sense appended after HB_BASE — those are not",
                "  loadable on their own here.",
                "",
                "EDITABLE ANIMATIONS",
                "  \"Export ANM → editable JSON\" writes every keyframe as raw channel",
                "  values (rotations are signed-byte 3x3 matrix coefficients, x32 = the",
                "  game's q12 values; translations are signed bytes scaled by the file's",
                "  scale). \"Import JSON → ANM\" re-emits a byte-identical .ANM when the",
                "  values are untouched, so round-trips are lossless.",
                "",
                "CONVERT",
                "  The Convert menu drives the SAME implementations as the Mod Manager's",
                "  buttons (Model → OBJ, high-poly OBJ → Model, simple import).",
            };
            ConverterActions.ShowTextDialog(this, "Model Viewer — Help", lines, false);
        }

        // ---- animation panel ------------------------------------------------------

        private Panel BuildAnimPanel()
        {
            var panel = new Panel { Dock = DockStyle.Bottom, Height = 34 };
            _btnPlay = new Button { Text = "Play", Location = new Point(8, 4), Size = new Size(56, 26), Enabled = false };
            _btnPlay.Click += (s, e) => TogglePlay();
            _bar = new TrackBar
            {
                Location = new Point(70, 4),
                Size = new Size(420, 26),
                Minimum = 0,
                Maximum = 1,
                TickStyle = TickStyle.None,
                Enabled = false,
                Anchor = AnchorStyles.Left | AnchorStyles.Top | AnchorStyles.Right
            };
            _bar.ValueChanged += (s, e) =>
            {
                if (_barFromCode) return;
                _playing = false;
                _btnPlay.Text = "Play";
                _time = _bar.Value;
                ApplyPose();
                Render();
            };
            _lblKf = new Label
            {
                Text = "no ANM",
                Location = new Point(498, 10),
                AutoSize = true,
                Anchor = AnchorStyles.Top | AnchorStyles.Right
            };
            _cmbSpeed = new ComboBox
            {
                Location = new Point(640, 6),
                Size = new Size(64, 24),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Anchor = AnchorStyles.Top | AnchorStyles.Right
            };
            _cmbSpeed.Items.AddRange(new object[] { "0.25x", "0.5x", "1x", "2x" });
            _cmbSpeed.SelectedIndex = 2;

            panel.Controls.Add(_btnPlay);
            panel.Controls.Add(_bar);
            panel.Controls.Add(_lblKf);
            panel.Controls.Add(_cmbSpeed);

            _timer = new Timer { Interval = 33 };
            _timer.Tick += (s, e) => OnAnimTick();
            return panel;
        }

        private void TogglePlay()
        {
            if (_anm2 == null) return;
            _playing = !_playing;
            _btnPlay.Text = _playing ? "Pause" : "Play";
            if (_playing)
            {
                _lastTick = DateTime.UtcNow;
                _timer.Start();
            }
        }

        private double Speed()
        {
            switch (_cmbSpeed.SelectedIndex)
            {
                case 0: return 0.25;
                case 1: return 0.5;
                case 3: return 2.0;
                default: return 1.0;
            }
        }

        private void OnAnimTick()
        {
            if (!_playing || _anm2 == null) { _timer.Stop(); return; }
            var now = DateTime.UtcNow;
            double dt = (now - _lastTick).TotalSeconds;
            _lastTick = now;
            if (dt > 0.25) dt = 0.25; // window was blocked — don't leap

            // 1x = the game's native 30 keyframes/second (the standard Q12(30) rate).
            _time += dt * 30.0 * Speed();
            double max = _anm2.KeyframeCount;
            if (_time >= max) _time -= max; // loop over the whole pool
            ApplyPose();
            Render();
        }

        private void LoadAnm(string path, bool report)
        {
            string err;
            AnmFile anm = AnmFile.Load(path, out err);
            if (anm == null)
            {
                if (report)
                    MessageBox.Show(this, "Could not load the ANM:\n\n" + err, "Model Viewer",
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (_anim.MaxBone >= anm.BoneCount)
            {
                if (report)
                    MessageBox.Show(this, Path.GetFileName(path) + " has " + anm.BoneCount +
                        " bones but this model binds bone " + _anim.MaxBone + " — not a match.",
                        "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            _anm2 = anm;
            _anmPath = path;
            _time = 0;
            _playing = false;
            _btnPlay.Enabled = true;
            _btnPlay.Text = "Play";
            _bar.Enabled = true;
            _bar.Maximum = Math.Max(1, anm.KeyframeCount - 1);
            _barFromCode = true; _bar.Value = 0; _barFromCode = false;
            Text = "Model Viewer — " + _scene.Title + "  [" + Path.GetFileName(path) + "]";
        }

        private void OnChooseAnm()
        {
            if (_anim == null)
            {
                MessageBox.Show(this, "Open a character model (.ILM) first — animations pose its skeleton.",
                    "Model Viewer", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select an animation (.ANM)";
                ofd.Filter = "Animations (*.anm)|*.anm|All files (*.*)|*.*";
                try
                {
                    string init = _anmPath != null ? Path.GetDirectoryName(_anmPath)
                        : Path.GetDirectoryName(_anim.IlmPath);
                    ofd.InitialDirectory = init;
                }
                catch { }
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                LoadAnm(ofd.FileName, true);
                ApplyPose();
                Render();
            }
        }

        private void OnExportAnmJson()
        {
            string src = _anmPath;
            if (src == null)
            {
                using (var ofd = new OpenFileDialog())
                {
                    ofd.Title = "Select an animation (.ANM) to export";
                    ofd.Filter = "Animations (*.anm)|*.anm|All files (*.*)|*.*";
                    if (ofd.ShowDialog(this) != DialogResult.OK) return;
                    src = ofd.FileName;
                }
            }
            string err;
            AnmFile anm = AnmFile.Load(src, out err);
            if (anm == null)
            {
                MessageBox.Show(this, "Could not load the ANM:\n\n" + err, "ANM → JSON",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save editable animation JSON";
                sfd.Filter = "JSON (*.json)|*.json|All files (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(src);
                sfd.FileName = Path.GetFileNameWithoutExtension(src) + ".anm.json";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    File.WriteAllText(sfd.FileName, anm.ToJson());
                    MessageBox.Show(this, "Wrote " + Path.GetFileName(sfd.FileName) + " (" +
                        anm.KeyframeCount + " keyframes, " + anm.BoneCount + " bones).\n\n" +
                        "Rotations are raw signed-byte q12 matrix coefficients and translations " +
                        "raw signed bytes — edit values in place, keep every value in -128..127, " +
                        "then \"Import JSON → ANM…\" writes it back (byte-identical when untouched).",
                        "ANM → JSON", MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Write failed:\n\n" + ex.Message, "ANM → JSON",
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        private void OnImportAnmJson()
        {
            string src;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select an animation JSON";
                ofd.Filter = "JSON (*.json)|*.json|All files (*.*)|*.*";
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                src = ofd.FileName;
            }
            string err;
            AnmFile anm = AnmFile.FromJson(File.ReadAllText(src), out err);
            if (anm == null)
            {
                MessageBox.Show(this, "The JSON did not validate:\n\n" + err, "JSON → ANM",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save the animation (.ANM)";
                sfd.Filter = "Animations (*.anm)|*.anm|All files (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(src);
                string stem = Path.GetFileNameWithoutExtension(src);
                if (stem.EndsWith(".anm", StringComparison.OrdinalIgnoreCase))
                    stem = stem.Substring(0, stem.Length - 4);
                sfd.FileName = stem + ".ANM";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    File.WriteAllBytes(sfd.FileName, anm.ToBytes());
                    MessageBox.Show(this, "Wrote " + Path.GetFileName(sfd.FileName) + ".\n\n" +
                        "To use it in game, drop it into gamedata\\load\\ANIM\\ under the ORIGINAL " +
                        "file name and set allow_loose_files = 1.",
                        "JSON → ANM", MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Write failed:\n\n" + ex.Message, "JSON → ANM",
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }

        // ---- pose -----------------------------------------------------------------

        /// <summary>Scene vertex -> (part, flattened local index), derived once from the
        /// AnimScene triangles. The unique set matters: welded corners share the OWNER
        /// part's vertex, so a seam vertex appears once and follows the owner's bone —
        /// exactly the game's weld semantics.</summary>
        private void BuildVertexMap()
        {
            _vertPart = new int[_scene.VertexCount];
            _vertLocal = new int[_scene.VertexCount];
            foreach (var t in _scene.SourceTris)
            {
                _vertPart[t.SceneV0] = t.P0; _vertLocal[t.SceneV0] = t.L0;
                _vertPart[t.SceneV1] = t.P1; _vertLocal[t.SceneV1] = t.L1;
                _vertPart[t.SceneV2] = t.P2; _vertLocal[t.SceneV2] = t.L2;
            }
        }

        /// <summary>Write the current pose into the scene's vertex arrays. No ANM (props,
        /// or none found): identity, i.e. every part in its own local space.</summary>
        private void ApplyPose()
        {
            if (_anim == null || _vertPart == null) return;

            int[][] R = null, T = null;
            if (_anm2 != null)
            {
                int kf0 = (int)Math.Floor(_time);
                if (kf0 < 0) kf0 = 0;
                if (kf0 > _anm2.KeyframeCount - 1) kf0 = _anm2.KeyframeCount - 1;
                int kf1 = kf0 + 1 < _anm2.KeyframeCount ? kf0 + 1 : kf0;
                int alpha = (int)((_time - kf0) * 4096.0);
                if (alpha < 0) alpha = 0;
                if (alpha > 4095) alpha = 4095;
                R = new int[_anm2.BoneCount][];
                T = new int[_anm2.BoneCount][];
                _anm2.WorldPose(kf0, kf1, alpha, R, T);

                _lblKf.Text = "KF " + _time.ToString("0.0", CultureInfo.InvariantCulture) +
                              " / " + (_anm2.KeyframeCount - 1);
                _barFromCode = true;
                int v = Math.Min(kf0, _bar.Maximum);
                if (_bar.Value != v) _bar.Value = v;
                _barFromCode = false;
            }

            var parts = _anim.Parts;
            for (int i = 0; i < _scene.VertexCount; i++)
            {
                var p = parts[_vertPart[i]];
                int li = _vertLocal[i];
                float lx = p.Lx[li], ly = p.Ly[li], lz = p.Lz[li];
                float wx, wy, wz;
                int b = p.Bone;
                if (R != null && b >= 0 && b < R.Length && R[b] != null)
                {
                    int[] r = R[b]; int[] t = T[b];
                    wx = (r[0] * lx + r[1] * ly + r[2] * lz) / 4096f + t[0];
                    wy = (r[3] * lx + r[4] * ly + r[5] * lz) / 4096f + t[1];
                    wz = (r[6] * lx + r[7] * ly + r[8] * lz) / 4096f + t[2];
                }
                else
                {
                    wx = lx; wy = ly; wz = lz;
                }
                // PSX y-down -> viewer y-up, same convention as the OBJ export.
                _scene.Vx[i] = wx;
                _scene.Vy[i] = -wy;
                _scene.Vz[i] = wz;
            }
        }

        // ---- view -----------------------------------------------------------------

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
            if (_scene == null) return;
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
            Bitmap bmp;
            if (_scene == null)
            {
                bmp = new Bitmap(w, h);
                using (var g = Graphics.FromImage(bmp))
                using (var f = new Font(FontFamily.GenericSansSerif, 10f))
                using (var br = new SolidBrush(Color.FromArgb(150, 150, 156)))
                {
                    g.Clear(Color.FromArgb(48, 48, 52));
                    const string hint = "File > Open a model (.ILM / .PLM / .TMD / .OBJ)\n\nor drag && drop one here";
                    var size = g.MeasureString(hint, f);
                    g.DrawString(hint, f, br, (w - size.Width) / 2f, (h - size.Height) / 2f);
                }
            }
            else
            {
                bmp = IlmSoftRenderer.Render(_scene, w, h, _yaw, _pitch, _dist, _panX, _panY,
                    _chkTex.Checked, _chkWire.Checked);
            }
            var old = _view.Image;
            _view.Image = bmp;
            if (old != null) old.Dispose();
        }
    }

    /// <summary>Geometry + texture for the viewer. Loaded from a .OBJ, materialized
    /// from an IlmObjConverter.AnimScene (character/prop models — re-posable), or
    /// built from a TMD. Kept separate from the Form so a headless harness can
    /// render it.</summary>
    internal sealed class IlmViewScene
    {
        public struct Tri
        {
            public int V0, V1, V2;
            public float U0, Vv0, U1, Vv1, U2, Vv2;
            public bool Alpha, HasUv;
            public int Mat; // material index for the per-material atlas; -1 = none
        }

        /// <summary>Provenance of an AnimScene-built triangle: scene vertex ids plus
        /// their (part, local) source, so the form can re-pose vertices in place.</summary>
        public struct SourceTri
        {
            public int SceneV0, SceneV1, SceneV2;
            public int P0, L0, P1, L1, P2, L2;
        }

        public string Title;
        public int Parts, VertexCount;
        public float[] Vx, Vy, Vz;
        public readonly List<Tri> Tris = new List<Tri>();
        public readonly List<SourceTri> SourceTris = new List<SourceTri>();
        public readonly List<int[]> Edges = new List<int[]>(); // unique [a,b] vertex pairs for wireframe
        public readonly List<string> Warnings = new List<string>();
        public int[] TexPix; // ARGB
        public int TexW, TexH;
        // UV -> texel scale. The ILM sheet convention is the 256-texel PSX page on
        // both axes regardless of the sheet's pixel size; the per-material atlas and
        // TMD atlases address their real pixel grid on V instead.
        public float UvScaleX = 256f, UvScaleY = 256f;
        public bool HasTexture { get { return TexPix != null; } }
        public float Cx, Cy, Cz, Radius;

        public static IlmViewScene Load(string path, out string error)
        {
            error = null;
            string ext = Path.GetExtension(path);
            string obj = path, ilm = null;
            var warnings = new List<string>();

            if (string.Equals(ext, ".obj", StringComparison.OrdinalIgnoreCase))
            {
                // An edited OBJ has no texture reference of its own; the original
                // .ILM (and its .TIM) normally sit beside it after an export.
                string cand = Path.ChangeExtension(path, ".ILM");
                if (File.Exists(cand)) ilm = cand;
                else { cand = Path.ChangeExtension(path, ".ilm"); if (File.Exists(cand)) ilm = cand; }
            }
            else
            {
                error = "not a .OBJ: " + Path.GetFileName(path);
                return null;
            }

            var sc = ParseObj(obj, out error);
            if (sc == null) return null;
            sc.Title = Path.GetFileName(path);
            sc.Warnings.AddRange(warnings);

            // The exported OBJ's UVs were authored against the single composed
            // sheet, so the legacy one-sheet texture path stays correct here.
            if (ilm != null)
            {
                try
                {
                    string dir = Path.Combine(Path.GetTempPath(), "SHPC_ModelViewer");
                    Directory.CreateDirectory(dir);
                    string png = Path.Combine(dir, Path.GetFileNameWithoutExtension(ilm) + "_sheet.png");
                    string terr;
                    if (ClutComposer.Compose(ilm, null, png, out terr)) sc.LoadTexture(png);
                }
                catch { }
            }
            return sc;
        }

        /// <summary>Materialize a converter AnimScene: unique (part, local) pairs become
        /// scene vertices (welded corners collapse onto the owner's vertex), positions
        /// are filled by the form's ApplyPose. Character models with no .ANM found get
        /// the same loud diagnosis the exporter gives; props (bone 0 only) are simply
        /// an identity-pose still life, which is correct.</summary>
        public static IlmViewScene FromAnimScene(IlmObjConverter.AnimScene anim, out string error)
        {
            error = null;
            var sc = new IlmViewScene();
            sc.Title = Path.GetFileName(anim.IlmPath);
            sc.Parts = anim.Parts.Length;

            var map = new Dictionary<long, int>();
            Func<int, int, int> vid = (part, local) =>
            {
                long key = ((long)part << 32) | (uint)local;
                int id;
                if (map.TryGetValue(key, out id)) return id;
                id = map.Count;
                map[key] = id;
                return id;
            };

            var edgeSeen = new HashSet<long>();
            foreach (var t in anim.Tris)
            {
                int a = vid(t.P0, t.L0), b = vid(t.P1, t.L1), c = vid(t.P2, t.L2);
                sc.Tris.Add(new Tri
                {
                    V0 = a, V1 = b, V2 = c,
                    U0 = t.U0, Vv0 = t.V0, U1 = t.U1, Vv1 = t.V1, U2 = t.U2, Vv2 = t.V2,
                    Alpha = t.Alpha, HasUv = t.HasUv, Mat = t.Mat
                });
                sc.SourceTris.Add(new SourceTri
                {
                    SceneV0 = a, SceneV1 = b, SceneV2 = c,
                    P0 = t.P0, L0 = t.L0, P1 = t.P1, L1 = t.L1, P2 = t.P2, L2 = t.L2
                });
                int[][] es = { new[] { a, b }, new[] { b, c }, new[] { c, a } };
                foreach (var e in es)
                {
                    long key = e[0] < e[1] ? ((long)e[0] << 32) | (uint)e[1] : ((long)e[1] << 32) | (uint)e[0];
                    if (edgeSeen.Add(key)) sc.Edges.Add(e);
                }
            }
            if (map.Count == 0) { error = "no geometry in " + sc.Title; return null; }

            sc.VertexCount = map.Count;
            sc.Vx = new float[sc.VertexCount];
            sc.Vy = new float[sc.VertexCount];
            sc.Vz = new float[sc.VertexCount];

            // Rest-fill from part-local space so bounds exist before the first pose.
            foreach (var t in sc.SourceTris)
            {
                sc.Vx[t.SceneV0] = anim.Parts[t.P0].Lx[t.L0];
                sc.Vy[t.SceneV0] = -anim.Parts[t.P0].Ly[t.L0];
                sc.Vz[t.SceneV0] = anim.Parts[t.P0].Lz[t.L0];
                sc.Vx[t.SceneV1] = anim.Parts[t.P1].Lx[t.L1];
                sc.Vy[t.SceneV1] = -anim.Parts[t.P1].Ly[t.L1];
                sc.Vz[t.SceneV1] = anim.Parts[t.P1].Lz[t.L1];
                sc.Vx[t.SceneV2] = anim.Parts[t.P2].Lx[t.L2];
                sc.Vy[t.SceneV2] = -anim.Parts[t.P2].Ly[t.L2];
                sc.Vz[t.SceneV2] = anim.Parts[t.P2].Lz[t.L2];
            }
            sc.ComputeBounds();
            // A posed character spans more than any single part's local box; widen the
            // framing to the whole-skeleton scale so the home view is not zoomed into
            // one thigh. (Bounds refresh is cosmetic — the renderer never culls by it.)
            if (anim.MaxBone > 0) sc.Radius = Math.Max(sc.Radius, 360f);

            sc.Warnings.AddRange(anim.Warnings);
            if (anim.MaxBone > 0 && anim.AnmPath == null)
                sc.Warnings.Add("NO ANIMATION FILE FOUND for " + sc.Title + " — every part poses at " +
                    "identity and piles on the origin. Keep the ANIM folder beside the model's folder " +
                    "(like the extracted disc layout) so the skeleton can be posed.");

            sc.BuildMaterialAtlas(anim);
            return sc;
        }

        /// <summary>One composed sheet per MATERIAL, stacked into a vertical atlas, and
        /// every triangle's V remapped into its own material's band. A model with one
        /// material renders exactly as the old single-sheet path did; a multi-material
        /// model (the BG *_GLB.PLM world-prop sets reference several map sheets) no
        /// longer samples everything from the first material's sheet.</summary>
        private void BuildMaterialAtlas(IlmObjConverter.AnimScene anim)
        {
            try
            {
                int matCount = anim.MaterialNames != null ? anim.MaterialNames.Length : 0;
                if (matCount == 0) return;

                string dir = Path.Combine(Path.GetTempPath(), "SHPC_ModelViewer");
                Directory.CreateDirectory(dir);

                // Resolve each material to a composed sheet. ResolveTargets does the
                // name -> .TIM work (sidecar dir first, then the whole-corpus clut
                // index, which is what finds CHARA/HERO.TIM for a weapon PLM or the
                // map sheets for a BG prop set).
                ClutComposer.ClutIndex idx = null;
                string root = FindExtractedRoot(anim.IlmPath);
                if (root != null)
                {
                    try { idx = ClutComposer.EnsureIndex(root, (i, n, m) => { }); } catch { }
                }
                string rerr;
                List<ClutComposer.Target> targets = ClutComposer.ResolveTargets(anim.IlmPath, idx, null, out rerr);
                if (targets == null || targets.Count == 0) return;

                var sheetPix = new Dictionary<string, int[]>();
                var sheetW = new Dictionary<string, int>();
                var sheetH = new Dictionary<string, int>();
                foreach (var t in targets)
                {
                    if (t.MaterialName == null || sheetPix.ContainsKey(t.MaterialName)) continue;
                    string png = Path.Combine(dir, Path.GetFileNameWithoutExtension(anim.IlmPath) +
                        "_" + t.MaterialName + "_sheet.png");
                    try
                    {
                        ClutComposer.ComposeTarget(t, png);
                        if (!File.Exists(png)) continue;
                        int w, h;
                        int[] pix = LoadPixels(png, out w, out h);
                        sheetPix[t.MaterialName] = pix;
                        sheetW[t.MaterialName] = w;
                        sheetH[t.MaterialName] = h;
                    }
                    catch { }
                }
                if (sheetPix.Count == 0) return;

                // Stack: 256-wide bands (page space), one per material WITH a sheet.
                var yOff = new int[matCount];
                var bandH = new int[matCount];
                var bandName = new string[matCount];
                int atlasH = 0;
                for (int mi = 0; mi < matCount; mi++)
                {
                    string name = anim.MaterialNames[mi];
                    yOff[mi] = -1;
                    if (name == null || !sheetPix.ContainsKey(name)) continue;
                    yOff[mi] = atlasH;
                    bandH[mi] = sheetH[name];
                    bandName[mi] = name;
                    atlasH += sheetH[name];
                }
                if (atlasH == 0) return;

                const int atlasW = 256;
                var atlas = new int[atlasW * atlasH];
                for (int mi = 0; mi < matCount; mi++)
                {
                    if (yOff[mi] < 0) continue;
                    string name = bandName[mi];
                    int w = Math.Min(sheetW[name], atlasW), h = sheetH[name];
                    int[] pix = sheetPix[name];
                    for (int y = 0; y < h; y++)
                        for (int x = 0; x < w; x++)
                            atlas[(yOff[mi] + y) * atlasW + x] = pix[y * sheetW[name] + x];
                }

                // Remap each textured tri's V into its material's band. The page-space
                // texel row is (1-v)*256; in the atlas it sits at yOff + that row, so
                // v' = 1 - (yOff + (1-v)*256) / atlasH with UvScaleY = atlasH. U keeps
                // page space (atlas is 256 wide). Single-sheet models reduce to the
                // old behaviour exactly.
                for (int i = 0; i < Tris.Count; i++)
                {
                    Tri t = Tris[i];
                    if (!t.HasUv) continue;
                    if (t.Mat < 0 || t.Mat >= matCount || yOff[t.Mat] < 0)
                    {
                        t.HasUv = false; // material has no sheet on disc — flat grey
                        Tris[i] = t;
                        continue;
                    }
                    float yo = yOff[t.Mat];
                    t.Vv0 = 1f - (yo + (1f - t.Vv0) * 256f) / atlasH;
                    t.Vv1 = 1f - (yo + (1f - t.Vv1) * 256f) / atlasH;
                    t.Vv2 = 1f - (yo + (1f - t.Vv2) * 256f) / atlasH;
                    Tris[i] = t;
                }

                TexPix = atlas;
                TexW = atlasW;
                TexH = atlasH;
                UvScaleX = 256f;
                UvScaleY = atlasH;
            }
            catch { } // texturing is best-effort; untextured is always a valid fallback
        }

        /// <summary>Walk up from the model looking for the extracted-tree root (the dir
        /// holding CHARA/ITEM — or an already-built clut_index.json).</summary>
        private static string FindExtractedRoot(string modelPath)
        {
            try
            {
                string d = Path.GetDirectoryName(Path.GetFullPath(modelPath));
                for (int i = 0; i < 4 && d != null; i++)
                {
                    if (File.Exists(Path.Combine(d, ClutComposer.IndexFileName))) return d;
                    if (Directory.Exists(Path.Combine(d, "CHARA")) &&
                        Directory.Exists(Path.Combine(d, "ITEM"))) return d;
                    d = Path.GetDirectoryName(d);
                }
            }
            catch { }
            return null;
        }

        /// <summary>Adapt a TMD (via TmdFile/TmdViewSceneBuilder) into the renderer's
        /// scene shape: PSX y-down flips to y-up, per-tri texture flags map onto
        /// Alpha/HasUv, and the atlas addresses its real pixel grid (UvScale).</summary>
        public static IlmViewScene FromTmd(string path, out string error)
        {
            var tmd = TmdFile.Load(path, out error);
            if (tmd == null) return null;
            var ts = TmdViewSceneBuilder.Build(tmd, path, out error);
            if (ts == null) return null;

            var sc = new IlmViewScene();
            sc.Title = Path.GetFileName(path) +
                       (tmd.ObjectCount > 1 ? "  (" + tmd.ObjectCount + " objects)" : "");
            sc.Parts = tmd.ObjectCount;
            sc.VertexCount = ts.Vx.Length;
            sc.Vx = ts.Vx;
            sc.Vy = new float[ts.Vy.Length];
            for (int i = 0; i < ts.Vy.Length; i++) sc.Vy[i] = -ts.Vy[i];
            sc.Vz = ts.Vz;

            var edgeSeen = new HashSet<long>();
            foreach (var t in ts.Tris)
            {
                sc.Tris.Add(new Tri
                {
                    V0 = t.V0, V1 = t.V1, V2 = t.V2,
                    U0 = t.U0, Vv0 = t.Vv0, U1 = t.U1, Vv1 = t.Vv1, U2 = t.U2, Vv2 = t.Vv2,
                    Alpha = t.SemiTransparent, HasUv = t.Textured, Mat = -1
                });
                int[][] es = { new[] { t.V0, t.V1 }, new[] { t.V1, t.V2 }, new[] { t.V2, t.V0 } };
                foreach (var e in es)
                {
                    long key = e[0] < e[1] ? ((long)e[0] << 32) | (uint)e[1] : ((long)e[1] << 32) | (uint)e[0];
                    if (edgeSeen.Add(key)) sc.Edges.Add(e);
                }
            }
            if (ts.TexPix != null)
            {
                sc.TexPix = ts.TexPix;
                sc.TexW = ts.TexW;
                sc.TexH = ts.TexH;
                sc.UvScaleX = ts.TexW;
                sc.UvScaleY = ts.TexH;
            }
            sc.Warnings.AddRange(ts.Warnings);
            sc.ComputeBounds();
            return sc;
        }

        public void ComputeBounds()
        {
            float minx = float.MaxValue, miny = float.MaxValue, minz = float.MaxValue;
            float maxx = float.MinValue, maxy = float.MinValue, maxz = float.MinValue;
            for (int i = 0; i < VertexCount; i++)
            {
                if (Vx[i] < minx) minx = Vx[i]; if (Vx[i] > maxx) maxx = Vx[i];
                if (Vy[i] < miny) miny = Vy[i]; if (Vy[i] > maxy) maxy = Vy[i];
                if (Vz[i] < minz) minz = Vz[i]; if (Vz[i] > maxz) maxz = Vz[i];
            }
            Cx = (minx + maxx) * 0.5f; Cy = (miny + maxy) * 0.5f; Cz = (minz + maxz) * 0.5f;
            float dx = maxx - minx, dy = maxy - miny, dz = maxz - minz;
            Radius = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
            if (Radius < 1e-3f) Radius = 1f;
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
                        var t = new Tri { V0 = vi[0], V1 = vi[i], V2 = vi[i + 1], Alpha = alpha, Mat = -1 };
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
            sc.ComputeBounds();
            return sc;
        }

        private static int[] LoadPixels(string png, out int w, out int h)
        {
            using (var src = new Bitmap(png))
            using (var bmp = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb))
            {
                using (var g = Graphics.FromImage(bmp)) g.DrawImageUnscaled(src, 0, 0);
                w = bmp.Width; h = bmp.Height;
                var bd = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                try
                {
                    var pix = new int[w * h];
                    for (int y = 0; y < h; y++)
                        Marshal.Copy(bd.Scan0 + y * bd.Stride, pix, y * w, w);
                    return pix;
                }
                finally { bmp.UnlockBits(bd); }
            }
        }

        public void LoadTexture(string png)
        {
            int w, h;
            TexPix = LoadPixels(png, out w, out h);
            TexW = w;
            TexH = h;
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
                        // ILM sheets: vt = ((u+0.5)/256, 1-(v+0.5)/256) — the 256-texel
                        // PSX page. Per-material and TMD atlases set UvScale to their
                        // real pixel grid instead.
                        float uu = w0 * tr.U0 + w1 * tr.U1 + w2 * tr.U2;
                        float vv = w0 * tr.Vv0 + w1 * tr.Vv1 + w2 * tr.Vv2;
                        int tx = (int)(uu * sc.UvScaleX);
                        int ty = (int)((1f - vv) * sc.UvScaleY);
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
