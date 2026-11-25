using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Windows.Forms;
using SilentHillPC_Launcher;

// Simple mode: the view the launcher opens in. A quality preset, the disc, the
// game language, and Play. Its settings live in config_simple.cfg, which the
// game lays over its compiled defaults when config.cfg says simple_mode = 1, so
// nothing the player set up in the advanced view is read or changed. Controls,
// the disc and the language are one set for both views and stay in config.cfg.
public partial class Form1
{
    private bool          _simpleMode;
    private ConfigManager _simpleCfg;
    private SimpleScene   _simple;
    private ComboBox      _sMinimap;
    private Button        _sPlay, _sControls, _sRA;
    private Label         _aLblMode;
    private RadioButton   _aRadioSimple, _aRadioAdvanced;
    private bool          _simpleLoading;
    private int           _sPresetIdx;
    private bool          _sDither = true, _sLogging = true;
    private Size          _advSize;
    private Point         _advLangAt;
    private ToolStripMenuItem _bannerMenuAdvanced;

    private struct SimpleHome
    {
        public Control Parent; public Point Location; public Size Size; public Color Back, Fore; public FlatStyle Flat;
    }
    private readonly Dictionary<Control, SimpleHome> _simpleHomes = new Dictionary<Control, SimpleHome>();

    // List order, as asked for: Original, then the enhanced presets with Normal
    // first. The values are the game's PC_SIMPLE_* ids.
    private static readonly int[]    SimplePresetIds   = { 0, 2, 1, 3, 4 };
    private static readonly string[] SimplePresetNames =
    {
        "Original", "Enhanced: Normal", "Enhanced: Low Spec", "Enhanced: High Quality", "Enhanced: Ultra Quality",
    };
    private static readonly string[] SimplePresetAbout =
    {
        "The game as it was on the PlayStation: its own resolution, 30 fps and the original lighting.",
        "Desktop resolution, no FPS limit, 2x antialiasing, sharper textures and flashlight shadows.",
        "Desktop resolution and smooth textures with no extra effects. For older or integrated graphics.",
        "4x antialiasing and the modern flashlight with detailed shadows.",
        "8x antialiasing and the highest shadow detail. For powerful graphics cards.",
    };

    private static readonly Size  SimpleSize = new Size(760, 664);
    private static readonly Color SimpleEdit = Color.FromArgb(34, 36, 34);
    private static readonly Color SimpleText = Color.FromArgb(232, 230, 222);
    private static readonly Color SimpleDim  = Color.FromArgb(176, 176, 168);
    private const int SimpleRowTop = 104, SimpleRowH = 68, SimpleLeft = 40, SimpleRight = 480;

    private Rectangle SimplePresetRect(int i) { return new Rectangle(SimpleLeft - 8, SimpleRowTop + i * SimpleRowH, 420, SimpleRowH - 4); }
    private static readonly Rectangle SimpleDitherRect  = new Rectangle(SimpleRight, 106, 240, 26);
    private static readonly Rectangle SimpleLoggingRect = new Rectangle(SimpleRight, 138, 240, 26);
    private static readonly Rectangle SimpleModeSimple   = new Rectangle(118, 622, 110, 26);
    private static readonly Rectangle SimpleModeAdvanced = new Rectangle(236, 622, 150, 26);

    private static string SimpleConfigPath =>
        Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config_simple.cfg");

    /// <summary>Update settings for the view on screen: simple mode follows the
    /// realtime builds of the default repo without changing what Build Settings
    /// has saved for the advanced view.</summary>
    private LauncherSettings UpdateSettings()
    {
        var s = LauncherSettings.Load(config);
        if (_simpleMode && s.IsDefaultRepo)
        {
            s.Realtime = true;
            s.Build    = "latest";
        }
        return s;
    }

    private Button SimpleButton(string text, Rectangle at, EventHandler click)
    {
        var b = new Button
        {
            Text = text, Location = at.Location, Size = at.Size, Font = new Font("Segoe UI", 10f),
            BackColor = SimpleEdit, ForeColor = SimpleText, FlatStyle = FlatStyle.Flat,
        };
        b.FlatAppearance.BorderColor = Color.FromArgb(84, 86, 82);
        b.Click += click;
        return b;
    }

    private void SimpleInit()
    {
        // The advanced view grows a row for its own Mode switch.
        _advSize   = new Size(ClientSize.Width, ClientSize.Height + 28);
        _advLangAt = btnLang != null ? btnLang.Location : Point.Empty;
        _aLblMode       = new Label { Text = "Mode:", Location = new Point(12, 6), AutoSize = true };
        _aRadioSimple   = new RadioButton { Text = "Simple",   Location = new Point(64, 4),  AutoSize = true };
        _aRadioAdvanced = new RadioButton { Text = "Advanced", Location = new Point(140, 4), AutoSize = true, Checked = true };
        _aRadioSimple.CheckedChanged += (s, e) =>
        {
            if (_aRadioSimple.Checked && !_simpleMode && !_simpleLoading) SimpleSet(true, true);
        };
        // A panel of their own keeps this pair apart from the form's other radios.
        var modeHost = new Panel { Location = new Point(0, ClientSize.Height), Size = new Size(300, 28) };
        modeHost.Controls.AddRange(new Control[] { _aLblMode, _aRadioSimple, _aRadioAdvanced });
        Controls.Add(modeHost);

        _simple = new SimpleScene { Location = new Point(0, 0), Size = SimpleSize, Visible = false };
        _simple.PaintContent = SimplePaint;
        _simple.MouseUp   += SimpleMouseUp;
        _simple.MouseMove += (s, e) => _simple.Cursor = SimpleHit(e.Location) != 0 ? Cursors.Hand : Cursors.Default;

        _sMinimap = new ComboBox
        {
            Location = new Point(SimpleRight, 206), Size = new Size(240, 25), DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = SimpleEdit, ForeColor = SimpleText, FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10f),
        };
        _sPlay = new Button
        {
            Text = "Play!", Location = new Point(SimpleLeft, 462), Size = new Size(680, 56),
            Font = new Font("Segoe UI", 17f, FontStyle.Bold),
            BackColor = Color.FromArgb(104, 26, 22), ForeColor = Color.White, FlatStyle = FlatStyle.Flat,
        };
        _sPlay.FlatAppearance.BorderColor = Color.FromArgb(170, 70, 58);
        _sPlay.Click += (s, e) => { if (btnPlay.Enabled) btnPlay_Click(btnPlay, e); };
        _sControls = SimpleButton("Controls", new Rectangle(SimpleLeft, 534, 150, 32), btnControls_Click);
        _sRA       = SimpleButton("RetroAchievements", new Rectangle(200, 534, 190, 32), (s, e) => btnRA_Click(btnRA, e));

        _simple.Controls.AddRange(new Control[] { _sMinimap, _sPlay, _sControls, _sRA });
        Controls.Add(_simple);

        _sMinimap.SelectedIndexChanged += (s, e) => SimpleSave();
        lblUpdateStatus.TextChanged      += (s, e) => { if (_simpleMode) _simple.ContentChanged(); };
        lblUpdateStatus.ForeColorChanged += (s, e) => { if (_simpleMode) _simple.ContentChanged(); };
        Loc.Changed += () =>
        {
            if (_simple == null) return;
            SimpleFillLists();
            _simple.ContentChanged();
        };

        SimpleFillLists();
        SimpleSet(config.Get("simple_mode", "1").Trim() != "0", false);
    }

    private void SimpleFillLists()
    {
        bool was = _simpleLoading;
        _simpleLoading = true;
        try
        {
            int m = _sMinimap.SelectedIndex;
            _sMinimap.Items.Clear();
            foreach (object o in comboMinimap.Items) _sMinimap.Items.Add(Loc.T(o.ToString()));
            if (_sMinimap.Items.Count > 0) _sMinimap.SelectedIndex = m >= 0 && m < _sMinimap.Items.Count ? m : 0;
            _sPlay.Text     = Loc.T("Play!");
            _sControls.Text = Loc.T("Controls");
        }
        finally { _simpleLoading = was; }
    }

    // 1..5 a preset row, 10 dithering, 11 logging, 20 / 21 the mode pair.
    private int SimpleHit(Point p)
    {
        for (int i = 0; i < SimplePresetIds.Length; i++)
            if (SimplePresetRect(i).Contains(p)) return 1 + i;
        if (SimpleDitherRect.Contains(p))   return 10;
        if (SimpleLoggingRect.Contains(p))  return 11;
        if (SimpleModeSimple.Contains(p))   return 20;
        if (SimpleModeAdvanced.Contains(p)) return 21;
        return 0;
    }

    private void SimpleMouseUp(object sender, MouseEventArgs e)
    {
        if (e.Button == MouseButtons.Right)
        {
            banner_MouseUp(_simple, e);
            return;
        }
        if (e.Button != MouseButtons.Left) return;
        int hit = SimpleHit(e.Location);
        if (hit >= 1 && hit <= 5) _sPresetIdx = hit - 1;
        else if (hit == 10) _sDither = !_sDither;
        else if (hit == 11) _sLogging = !_sLogging;
        else if (hit == 21) { SimpleSet(false, true); return; }
        else return;
        _simple.ContentChanged();
        SimpleSave();
    }

    private static void SimpleOutlined(Graphics g, string s, Font f, Color fill, RectangleF box)
    {
        using (var path = new GraphicsPath())
        using (var fmt = new StringFormat(StringFormat.GenericTypographic))
        {
            fmt.FormatFlags &= ~StringFormatFlags.NoWrap;
            fmt.Trimming = StringTrimming.EllipsisWord;
            path.AddString(s, f.FontFamily, (int)f.Style, g.DpiY * f.SizeInPoints / 72f, box, fmt);
            using (var pen = new Pen(Color.FromArgb(235, 0, 0, 0), f.SizeInPoints >= 14f ? 4f : 3f) { LineJoin = LineJoin.Round })
                g.DrawPath(pen, path);
            using (var b = new SolidBrush(fill))
                g.FillPath(b, path);
        }
    }

    private static void SimpleMark(Graphics g, Rectangle at, bool round, bool on)
    {
        var box = new Rectangle(at.X, at.Y + 3, 18, 18);
        using (var edge = new Pen(Color.FromArgb(235, 0, 0, 0), 5f))
        using (var line = new Pen(Color.FromArgb(214, 212, 204), 2f))
        using (var dot = new SolidBrush(Color.FromArgb(214, 70, 56)))
        using (var back = new SolidBrush(Color.FromArgb(190, 12, 13, 12)))
        {
            if (round)
            {
                g.DrawEllipse(edge, box); g.FillEllipse(back, box); g.DrawEllipse(line, box);
                if (on) g.FillEllipse(dot, box.X + 4, box.Y + 4, 10, 10);
            }
            else
            {
                g.DrawRectangle(edge, box); g.FillRectangle(back, box); g.DrawRectangle(line, box);
                if (on) g.FillRectangle(dot, box.X + 4, box.Y + 4, 11, 11);
            }
        }
    }

    /// <summary>Everything the simple view shows as text, drawn over the fog.</summary>
    private void SimplePaint(Graphics g)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using (var title = new Font("Georgia", 25f, FontStyle.Bold))
        using (var head  = new Font("Segoe UI", 12.5f, FontStyle.Bold))
        using (var body  = new Font("Segoe UI", 9.75f))
        using (var item  = new Font("Segoe UI", 11f))
        {
            SimpleOutlined(g, "SILENT HILL", title, Color.FromArgb(214, 70, 56), new RectangleF(SimpleLeft - 2, 22, 500, 50));
            SimpleOutlined(g, Loc.T("Quality"), item, SimpleDim, new RectangleF(SimpleLeft, 76, 400, 24));
            SimpleOutlined(g, Loc.T("Options"), item, SimpleDim, new RectangleF(SimpleRight, 76, 240, 24));

            for (int i = 0; i < SimplePresetIds.Length; i++)
            {
                var r = SimplePresetRect(i);
                bool on = i == _sPresetIdx;
                if (on)
                    using (var hl = new SolidBrush(Color.FromArgb(70, 120, 40, 32)))
                        g.FillRectangle(hl, r);
                SimpleMark(g, new Rectangle(r.X + 8, r.Y + 4, 18, 18), true, on);
                SimpleOutlined(g, Loc.T(SimplePresetNames[i]), head, on ? Color.White : SimpleText,
                               new RectangleF(r.X + 36, r.Y + 2, r.Width - 40, 24));
                SimpleOutlined(g, Loc.T(SimplePresetAbout[i]), body, SimpleDim,
                               new RectangleF(r.X + 36, r.Y + 25, r.Width - 44, 40));
            }

            SimpleMark(g, SimpleDitherRect, false, _sDither);
            SimpleOutlined(g, Loc.T("Dithering"), item, SimpleText,
                           new RectangleF(SimpleDitherRect.X + 28, SimpleDitherRect.Y + 1, 210, 24));
            SimpleMark(g, SimpleLoggingRect, false, _sLogging);
            SimpleOutlined(g, Loc.T("Logging"), item, SimpleText,
                           new RectangleF(SimpleLoggingRect.X + 28, SimpleLoggingRect.Y + 1, 210, 24));

            SimpleOutlined(g, Loc.T("Minimap:"), item, SimpleText, new RectangleF(SimpleRight, 182, 240, 22));
            SimpleOutlined(g, Loc.T("Disc:"), item, SimpleText, new RectangleF(SimpleRight, 244, 240, 22));
            SimpleOutlined(g, Loc.T("Game Language:"), item, SimpleText, new RectangleF(SimpleRight, 306, 240, 22));

            if (!string.IsNullOrEmpty(lblUpdateStatus.Text))
                SimpleOutlined(g, lblUpdateStatus.Text, body,
                               lblUpdateStatus.ForeColor.GetBrightness() < 0.45f ? SimpleDim : lblUpdateStatus.ForeColor,
                               new RectangleF(SimpleLeft, 578, 350, 20));

            SimpleOutlined(g, Loc.T("Mode:"), item, SimpleText, new RectangleF(SimpleLeft, 624, 80, 22));
            SimpleMark(g, SimpleModeSimple, true, true);
            SimpleOutlined(g, Loc.T("Simple"), item, SimpleText,
                           new RectangleF(SimpleModeSimple.X + 28, SimpleModeSimple.Y + 1, 90, 22));
            SimpleMark(g, SimpleModeAdvanced, true, false);
            SimpleOutlined(g, Loc.T("Advanced"), item, SimpleText,
                           new RectangleF(SimpleModeAdvanced.X + 28, SimpleModeAdvanced.Y + 1, 130, 22));
        }
    }

    /// <summary>Read config_simple.cfg into the simple view.</summary>
    private void SimpleLoad()
    {
        if (_simple == null) return;
        _simpleCfg = new ConfigManager(SimpleConfigPath);
        _simpleLoading = true;
        try
        {
            int idx = Array.IndexOf(SimplePresetIds, ParseIntOr(_simpleCfg.Get("simple_preset", "0"), 0));
            _sPresetIdx = idx >= 0 ? idx : 0;
            _sDither    = _simpleCfg.Get("simple_dithering", "1").Trim() != "0";
            _sLogging   = _simpleCfg.Get("enable_debug_log", "1").Trim() != "0";

            int mm = ParseIntOr(_simpleCfg.Get("minimap", "0"), 0);
            int mc = ParseIntOr(_simpleCfg.Get("minimap_corner", "0"), 0);
            if (mc < 0 || mc > 3) mc = 0;
            int mi = mm == 0 ? 0 : (mm == 2 ? 1 + mc : 5 + mc);
            _sMinimap.SelectedIndex = mi < _sMinimap.Items.Count ? mi : 0;
        }
        finally { _simpleLoading = false; }
        _simple.ContentChanged();
    }

    /// <summary>Write the simple view: its own settings to config_simple.cfg,
    /// and only the mode, the disc and the language to config.cfg.</summary>
    private void SimpleSave()
    {
        if (_simpleLoading || !_simpleMode || _simpleCfg == null) return;
        try
        {
            int mi = _sMinimap.SelectedIndex >= 0 ? _sMinimap.SelectedIndex : 0;
            var scr = Screen.PrimaryScreen.Bounds;

            if (!File.Exists(SimpleConfigPath))
                File.WriteAllText(SimpleConfigPath,
                    "# Settings of the launcher's Simple mode. The graphics settings come from\r\n" +
                    "# simple_preset; config.cfg holds the Advanced mode's settings and the controls.\r\n");
            _simpleCfg.RefreshFromDisk();
            _simpleCfg.Set("simple_preset", SimplePresetIds[_sPresetIdx].ToString());
            _simpleCfg.Set("simple_dithering", _sDither ? "1" : "0");
            _simpleCfg.Set("enable_debug_log", _sLogging ? "1" : "0");
            _simpleCfg.Set("minimap", mi == 0 ? "0" : (mi <= 4 ? "2" : "1"));
            _simpleCfg.Set("minimap_corner", mi == 0 ? "0" : (mi <= 4 ? mi - 1 : mi - 5).ToString());
            // The enhanced presets render at the desktop's size.
            _simpleCfg.Set("width", scr.Width.ToString());
            _simpleCfg.Set("height", scr.Height.ToString());
            _simpleCfg.Save();

            config.Set("simple_mode", "1");
            if (comboDisc.SelectedIndex >= 0 && comboDisc.SelectedIndex < _discIds.Count)
                config.Set("disc_image", _discIds[comboDisc.SelectedIndex]);
            if (_gameLangLockedBy == null &&
                comboGameLang.SelectedIndex >= 0 && comboGameLang.SelectedIndex < _gameLangCodes.Count)
                config.Set("language", _gameLangCodes[comboGameLang.SelectedIndex]);
            config.Save();
        }
        catch { /* a read-only install must not stop the game from starting */ }
    }

    private void SimpleBorrow(Control c, Point at, Size size)
    {
        if (!_simpleHomes.ContainsKey(c))
            _simpleHomes[c] = new SimpleHome
            {
                Parent = c.Parent, Location = c.Location, Size = c.Size, Back = c.BackColor, Fore = c.ForeColor,
                Flat = c is ComboBox ? ((ComboBox)c).FlatStyle : (c is Button ? ((Button)c).FlatStyle : FlatStyle.Standard),
            };
        if (c is ComboBox) ((ComboBox)c).FlatStyle = FlatStyle.Flat;
        if (c is Button)   ((Button)c).FlatStyle   = FlatStyle.Flat;
        c.Parent    = _simple;
        c.Location  = at;
        c.Size      = size;
        c.BackColor = SimpleEdit;
        c.ForeColor = SimpleText;
        c.BringToFront();
    }

    private void SimpleReturnAll()
    {
        foreach (var kv in _simpleHomes)
        {
            kv.Key.Parent    = kv.Value.Parent;
            kv.Key.Location  = kv.Value.Location;
            kv.Key.Size      = kv.Value.Size;
            kv.Key.BackColor = kv.Value.Back;
            kv.Key.ForeColor = kv.Value.Fore;
            if (kv.Key is ComboBox) ((ComboBox)kv.Key).FlatStyle = kv.Value.Flat;
            if (kv.Key is Button)   ((Button)kv.Key).FlatStyle   = kv.Value.Flat;
        }
        // Dark mode may have been captured while these were away.
        if (_lightColors != null)
            foreach (var kv in _simpleHomes)
                if (!_lightColors.ContainsKey(kv.Key))
                    _lightColors[kv.Key] = new Color[] { kv.Value.Back, kv.Value.Fore };
        _simpleHomes.Clear();
        if (_darkMode) ApplyDarkMode();
    }

    /// <summary>Switch between the simple and the advanced view.</summary>
    private void SimpleSet(bool on, bool persist)
    {
        bool changed = on != _simpleMode;
        _simpleMode = on;

        if (on)
        {
            ClientSize = SimpleSize;
            // The pieces both views share are moved rather than copied, so the
            // disc and language logic and the update flow keep working as is.
            SimpleBorrow(comboDisc,     new Point(SimpleRight, 268), new Size(240, 25));
            SimpleBorrow(comboGameLang, new Point(SimpleRight, 330), new Size(240, 25));
            SimpleBorrow(btnUpdate,     new Point(560, 534), new Size(160, 32));
            SimpleBorrow(progUpdate,    new Point(400, 580), new Size(320, 16));
            SimpleLoad();
            _simple.Visible = true;
            _simple.BringToFront();
            if (btnLang != null)
            {
                btnLang.Location = new Point(SimpleSize.Width - 40, SimpleSize.Height - 34);
                btnLang.BringToFront();
            }
            _simple.Start();
        }
        else
        {
            _simple.Stop();
            _simple.Visible = false;
            SimpleReturnAll();
            ClientSize = _advSize;
            if (btnLang != null) btnLang.Location = _advLangAt;
            _simpleLoading = true;
            _aRadioAdvanced.Checked = true;
            _simpleLoading = false;
            if (changed) LoadConfig();
        }

        if (persist)
        {
            try
            {
                config.Set("simple_mode", on ? "1" : "0");
                config.Save();
            }
            catch { }
            if (on) SimpleSave();
            // The two views follow different update streams.
            if (changed) SilentAutoCheckForUpdates();
        }
    }
}

/// <summary>
/// The simple view's surface: a dark ground with drifting fog and slow PS1-style
/// snow, and the view's text drawn over it. Painted with GDI+; when a frame costs
/// too much (old machines, remote sessions) the fog and snow stop and the view
/// stays as a still picture.
/// </summary>
internal sealed class SimpleScene : Panel
{
    private static readonly Color Ground = Color.FromArgb(10, 11, 10);
    private readonly Timer _timer = new Timer { Interval = 40 };
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly Random _rng = new Random(1999);
    private Bitmap _fogA, _fogB;    // two tileable fog banks, twice the width
    private Bitmap _layer;          // the text, redrawn only when it changes
    private bool   _layerDirty = true;
    private bool   _animated = true;
    private double _costMs;
    private int    _frames;

    private struct Flake { public float X, Y, Speed, Sway, Phase; public int Size, Alpha; }
    private Flake[] _flakes;

    public Action<Graphics> PaintContent;

    public SimpleScene()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Ground;
        if (SystemInformation.TerminalServerSession) _animated = false;
        _timer.Tick += (s, e) => Invalidate();
    }

    public void ContentChanged()
    {
        _layerDirty = true;
        Invalidate();
    }

    public void Start()
    {
        if (_animated) _timer.Start();
        Invalidate();
    }

    public void Stop() { _timer.Stop(); }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Dispose();
            if (_fogA != null) _fogA.Dispose();
            if (_fogB != null) _fogB.Dispose();
            if (_layer != null) _layer.Dispose();
        }
        base.Dispose(disposing);
    }

    private static float Hash(int x, int y, int seed)
    {
        unchecked
        {
            uint h = (uint)(x * 374761393 + y * 668265263 + seed * 144665);
            h = (h ^ (h >> 13)) * 1274126177u;
            return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777215f;
        }
    }

    // Value noise that repeats every `period` cells across, so a bank can slide
    // sideways for ever.
    private static float Noise(float x, float y, int period, int seed)
    {
        int xi = (int)Math.Floor(x), yi = (int)Math.Floor(y);
        float fx = x - xi, fy = y - yi;
        fx = fx * fx * (3 - 2 * fx);
        fy = fy * fy * (3 - 2 * fy);
        int x0 = ((xi % period) + period) % period, x1 = (x0 + 1) % period;
        float a = Hash(x0, yi, seed), b = Hash(x1, yi, seed);
        float c = Hash(x0, yi + 1, seed), d = Hash(x1, yi + 1, seed);
        return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
    }

    private Bitmap MakeFog(int seed, float strength)
    {
        int sw = Math.Max(8, Width / 4), sh = Math.Max(8, Height / 4);
        using (var small = new Bitmap(sw, sh, PixelFormat.Format32bppPArgb))
        {
            var data = small.LockBits(new Rectangle(0, 0, sw, sh), ImageLockMode.WriteOnly, PixelFormat.Format32bppPArgb);
            var px = new byte[data.Stride * sh];
            for (int y = 0; y < sh; y++)
            {
                for (int x = 0; x < sw; x++)
                {
                    float v = 0, amp = 0.5f;
                    int   per = 4;
                    // Stretched sideways: long soft layers, not smoke.
                    for (int o = 0; o < 4; o++)
                    {
                        v += amp * Noise(x * per / (float)sw, y * per * 1.6f / sh + o * 7.3f, per, seed + o);
                        amp *= 0.5f;
                        per *= 2;
                    }
                    float a = (v - 0.36f) / 0.36f;
                    a = a < 0 ? 0 : (a > 1 ? 1 : a);
                    a = a * a * (3 - 2 * a) * strength;
                    int i = y * data.Stride + x * 4;
                    px[i]     = (byte)(150 * a);
                    px[i + 1] = (byte)(156 * a);
                    px[i + 2] = (byte)(158 * a);
                    px[i + 3] = (byte)(255 * a);
                }
            }
            System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
            small.UnlockBits(data);

            var big = new Bitmap(Width * 2, Height, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(big))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                g.PixelOffsetMode   = PixelOffsetMode.Half;
                g.DrawImage(small, new Rectangle(0, 0, Width, Height));
                g.DrawImage(small, new Rectangle(Width, 0, Width, Height));
            }
            return big;
        }
    }

    private void BuildMoving()
    {
        _fogA = MakeFog(11, 0.34f);
        _fogB = MakeFog(47, 0.24f);
        _flakes = new Flake[110];
        for (int i = 0; i < _flakes.Length; i++)
        {
            int layer = i % 3;
            _flakes[i] = new Flake
            {
                X = (float)_rng.NextDouble() * Width,
                Y = (float)_rng.NextDouble() * Height,
                Speed = 9f + layer * 7f + (float)_rng.NextDouble() * 4f,
                Sway  = 4f + (float)_rng.NextDouble() * 6f,
                Phase = (float)_rng.NextDouble() * 6.28f,
                Size  = 2 + (layer == 2 ? 1 : 0),
                Alpha = 90 + layer * 40,
            };
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        long t0 = Stopwatch.GetTimestamp();

        g.Clear(Ground);
        if (_fogA == null) BuildMoving();
        {
            // A still picture keeps the fog and snow where they stood.
            double t = _animated ? _clock.Elapsed.TotalSeconds : 12.0;
            int xa = (int)(t * 9.0) % Width, xb = Width - 1 - (int)(t * 5.0) % Width;
            g.CompositingQuality = CompositingQuality.HighSpeed;
            g.DrawImageUnscaled(_fogA, -xa, 0);
            g.DrawImageUnscaled(_fogB, -xb, 0);
            for (int i = 0; i < _flakes.Length; i++)
            {
                var f = _flakes[i];
                float y = (f.Y + (float)t * f.Speed) % Height;
                float x = (f.X + (float)Math.Sin(t * 0.6 + f.Phase) * f.Sway + (float)t * f.Speed * 0.2f) % Width;
                using (var b = new SolidBrush(Color.FromArgb(f.Alpha, 235, 230, 244)))
                    g.FillRectangle(b, (int)x, (int)y, f.Size, f.Size);
            }
        }

        if (_layer == null || _layer.Width != Width || _layer.Height != Height)
        {
            if (_layer != null) _layer.Dispose();
            _layer = new Bitmap(Math.Max(1, Width), Math.Max(1, Height), PixelFormat.Format32bppPArgb);
            _layerDirty = true;
        }
        if (_layerDirty && PaintContent != null)
        {
            using (var lg = Graphics.FromImage(_layer))
            {
                lg.Clear(Color.Transparent);
                PaintContent(lg);
            }
            _layerDirty = false;
        }
        g.DrawImageUnscaled(_layer, 0, 0);

        if (_animated)
        {
            // A machine that cannot keep this up gets the still picture.
            _costMs += (Stopwatch.GetTimestamp() - t0) * 1000.0 / Stopwatch.Frequency;
            if (++_frames == 40)
            {
                if (_costMs / _frames > 16.0)
                {
                    _animated = false;
                    _timer.Stop();
                    Invalidate();
                }
                _costMs = 0;
                _frames = 0;
            }
        }
    }
}
