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
    private ComboBox      _sPreset, _sMinimap;
    private CheckBox      _sDither;
    private Button        _sPlay, _sControls;
    private RadioButton   _sRadioSimple, _sRadioAdvanced;
    private Label         _sLblQuality, _sLblMinimap, _sLblDisc, _sLblLang, _sLblMode;
    private bool          _simpleLoading;
    private ToolStripMenuItem _bannerMenuAdvanced;

    private struct SimpleHome
    {
        public Control Parent; public Point Location; public Size Size; public Color Back, Fore; public bool Visible;
        public FlatStyle Flat;
    }
    private readonly Dictionary<Control, SimpleHome> _simpleHomes = new Dictionary<Control, SimpleHome>();

    // Combo order, as asked for: Original, then the enhanced presets with
    // Normal first. The values are the game's PC_SIMPLE_* ids.
    private static readonly int[]    SimplePresetIds   = { 0, 2, 1, 3, 4 };
    private static readonly string[] SimplePresetNames =
    {
        "Original", "Enhanced: Normal", "Enhanced: Low Spec", "Enhanced: High Quality", "Enhanced: Ultra Quality",
    };

    private static readonly Color SimpleCard = Color.FromArgb(14, 15, 14);
    private static readonly Color SimpleEdit = Color.FromArgb(38, 40, 38);
    private static readonly Color SimpleText = Color.FromArgb(224, 222, 214);
    private const int SimpleCardTop = 300;

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

    private void SimpleInit()
    {
        _simple = new SimpleScene(SimpleCardTop)
        {
            Location = new Point(0, 0),
            Size     = ClientSize,
            Visible  = false,
        };
        _simple.BannerImage = () => banner.Image;
        _simple.MouseUp += (s, e) =>
        {
            if (e.Button == MouseButtons.Right && e.Y < banner.Height) banner_MouseUp(_simple, e);
        };

        _sLblQuality = SimpleLabel("Quality:", 318);
        _sPreset     = SimpleCombo(314);
        _sDither     = new CheckBox
        {
            Text = "Dithering", Location = new Point(150, 344), AutoSize = true,
            BackColor = SimpleCard, ForeColor = SimpleText, FlatStyle = FlatStyle.Standard,
        };
        _sLblMinimap = SimpleLabel("Minimap:", 376);
        _sMinimap    = SimpleCombo(372);
        _sLblDisc    = SimpleLabel("Disc:", 406);
        _sLblLang    = SimpleLabel("Game Language:", 436);

        _sPlay = new Button
        {
            Text = "Play!", Location = new Point(24, 472), Size = new Size(406, 44),
            Font = new Font(Font.FontFamily, 13f, FontStyle.Bold),
            BackColor = Color.FromArgb(92, 24, 20), ForeColor = Color.White, FlatStyle = FlatStyle.Flat,
        };
        _sPlay.FlatAppearance.BorderColor = Color.FromArgb(150, 60, 50);
        _sPlay.Click += (s, e) => { if (btnPlay.Enabled) btnPlay_Click(btnPlay, e); };

        _sControls = new Button
        {
            Text = "Controls", Location = new Point(24, 560), Size = new Size(110, 24),
            BackColor = SimpleEdit, ForeColor = SimpleText, FlatStyle = FlatStyle.Flat,
        };
        _sControls.FlatAppearance.BorderColor = Color.FromArgb(70, 72, 70);
        _sControls.Click += btnControls_Click;

        _sLblMode = new Label
        {
            Text = "Mode:", Location = new Point(24, 618), AutoSize = true, BackColor = SimpleCard, ForeColor = SimpleText,
        };
        _sRadioSimple = new RadioButton
        {
            Text = "Simple", Location = new Point(84, 616), AutoSize = true, Checked = true,
            BackColor = SimpleCard, ForeColor = SimpleText,
        };
        _sRadioAdvanced = new RadioButton
        {
            Text = "Advanced", Location = new Point(164, 616), AutoSize = true,
            BackColor = SimpleCard, ForeColor = SimpleText,
        };
        _sRadioAdvanced.CheckedChanged += (s, e) =>
        {
            if (_sRadioAdvanced.Checked && _simpleMode) SimpleSet(false, true);
        };

        _simple.Controls.AddRange(new Control[]
        {
            _sLblQuality, _sPreset, _sDither, _sLblMinimap, _sMinimap, _sLblDisc, _sLblLang,
            _sPlay, _sControls, _sLblMode, _sRadioSimple, _sRadioAdvanced,
        });
        Controls.Add(_simple);

        _sPreset.SelectedIndexChanged  += (s, e) => SimpleSave();
        _sMinimap.SelectedIndexChanged += (s, e) => SimpleSave();
        _sDither.CheckedChanged        += (s, e) => SimpleSave();
        Loc.Changed += () => { if (_simple != null) { SimpleFillLists(); _simple.Invalidate(); } };

        SimpleFillLists();
        SimpleSet(config.Get("simple_mode", "1").Trim() != "0", false);
    }

    private Label SimpleLabel(string text, int y)
    {
        return new Label
        {
            Text = text, Location = new Point(24, y), AutoSize = true, BackColor = SimpleCard, ForeColor = SimpleText,
        };
    }

    private ComboBox SimpleCombo(int y)
    {
        return new ComboBox
        {
            Location = new Point(150, y), Size = new Size(280, 21), DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = SimpleEdit, ForeColor = SimpleText, FlatStyle = FlatStyle.Flat,
        };
    }

    private void SimpleFillLists()
    {
        bool was = _simpleLoading;
        _simpleLoading = true;
        try
        {
            int p = _sPreset.SelectedIndex, m = _sMinimap.SelectedIndex;
            _sPreset.Items.Clear();
            foreach (string n in SimplePresetNames) _sPreset.Items.Add(Loc.T(n));
            _sMinimap.Items.Clear();
            foreach (object o in comboMinimap.Items) _sMinimap.Items.Add(o);
            if (_sPreset.Items.Count > 0)  _sPreset.SelectedIndex  = p >= 0 && p < _sPreset.Items.Count ? p : 0;
            if (_sMinimap.Items.Count > 0) _sMinimap.SelectedIndex = m >= 0 && m < _sMinimap.Items.Count ? m : 0;
        }
        finally { _simpleLoading = was; }
    }

    /// <summary>Read config_simple.cfg into the simple view.</summary>
    private void SimpleLoad()
    {
        if (_simple == null) return;
        _simpleCfg = new ConfigManager(SimpleConfigPath);
        _simpleLoading = true;
        try
        {
            int id  = ParseIntOr(_simpleCfg.Get("simple_preset", "0"), 0);
            int idx = Array.IndexOf(SimplePresetIds, id);
            _sPreset.SelectedIndex = idx >= 0 ? idx : 0;
            _sDither.Checked = _simpleCfg.Get("simple_dithering", "1").Trim() != "0";

            int mm = ParseIntOr(_simpleCfg.Get("minimap", "0"), 0);
            int mc = ParseIntOr(_simpleCfg.Get("minimap_corner", "0"), 0);
            if (mc < 0 || mc > 3) mc = 0;
            int mi = mm == 0 ? 0 : (mm == 2 ? 1 + mc : 5 + mc);
            _sMinimap.SelectedIndex = mi < _sMinimap.Items.Count ? mi : 0;
        }
        finally { _simpleLoading = false; }
    }

    /// <summary>Write the simple view: its own settings to config_simple.cfg,
    /// and only the mode, the disc and the language to config.cfg.</summary>
    private void SimpleSave()
    {
        if (_simpleLoading || !_simpleMode || _simpleCfg == null) return;
        try
        {
            int pi = _sPreset.SelectedIndex >= 0 ? _sPreset.SelectedIndex : 0;
            int mi = _sMinimap.SelectedIndex >= 0 ? _sMinimap.SelectedIndex : 0;
            var scr = Screen.PrimaryScreen.Bounds;

            if (!File.Exists(SimpleConfigPath))
                File.WriteAllText(SimpleConfigPath,
                    "# Settings of the launcher's Simple mode. The graphics settings come from\r\n" +
                    "# simple_preset; config.cfg holds the Advanced mode's settings and the controls.\r\n");
            _simpleCfg.RefreshFromDisk();
            _simpleCfg.Set("simple_preset", SimplePresetIds[pi].ToString());
            _simpleCfg.Set("simple_dithering", _sDither.Checked ? "1" : "0");
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
                Visible = c.Visible,
                Flat = c is ComboBox ? ((ComboBox)c).FlatStyle : FlatStyle.Standard,
            };
        if (c is ComboBox) ((ComboBox)c).FlatStyle = FlatStyle.Flat;
        c.Parent   = _simple;
        c.Location = at;
        c.Size     = size;
        if (c is ComboBox || c is Button) c.BackColor = SimpleEdit; else c.BackColor = SimpleCard;
        if (c != lblUpdateStatus) c.ForeColor = SimpleText;
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
            if (kv.Key is ComboBox) ((ComboBox)kv.Key).FlatStyle = kv.Value.Flat;
            if (kv.Key != lblUpdateStatus) kv.Key.ForeColor = kv.Value.Fore;
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
            // The pieces both views share are moved rather than copied, so the
            // disc and language logic and the update flow keep working as is.
            SimpleBorrow(comboDisc,       new Point(150, 402), new Size(280, 21));
            SimpleBorrow(comboGameLang,   new Point(150, 432), new Size(280, 21));
            SimpleBorrow(lblUpdateStatus, new Point(24, 530),  new Size(280, 15));
            SimpleBorrow(btnUpdate,       new Point(310, 525), new Size(120, 24));
            SimpleBorrow(progUpdate,      new Point(150, 560), new Size(280, 23));
            _simpleLoading = true;
            _sRadioSimple.Checked = true;
            _simpleLoading = false;
            SimpleLoad();
            _simple.Visible = true;
            _simple.BringToFront();
            if (btnLang != null) btnLang.BringToFront();
            _simple.Start();
        }
        else
        {
            _simple.Stop();
            _simple.Visible = false;
            SimpleReturnAll();
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
/// The simple view's surface: the banner, the site's background art under a dark
/// veil, drifting fog and slow PS1-style snow, and a plain card below it for the
/// controls. The moving part is painted with GDI+; when a frame costs too much
/// (old machines, remote sessions) it stops moving and stays as a still picture.
/// </summary>
internal sealed class SimpleScene : Panel
{
    private readonly int _cardTop;
    private readonly Timer _timer = new Timer { Interval = 40 };
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly Random _rng = new Random(1999);
    private Bitmap _still;          // banner + art + veil, drawn once
    private Bitmap _fogA, _fogB;    // two tileable fog banks, twice the width
    private Image  _art;
    private Image  _stillBanner;
    private bool   _animated = true;
    private double _costMs;
    private int    _frames;

    private struct Flake { public float X, Y, Speed, Sway, Phase; public int Size, Alpha; }
    private Flake[] _flakes;

    public Func<Image> BannerImage;

    public SimpleScene(int cardTop)
    {
        _cardTop = cardTop;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer, true);
        BackColor = Color.FromArgb(14, 15, 14);
        if (SystemInformation.TerminalServerSession) _animated = false;
        try
        {
            var asm = System.Reflection.Assembly.GetExecutingAssembly();
            using (var s = asm.GetManifestResourceStream("SilentHillPC_Launcher.SimpleBg.jpg"))
                if (s != null) _art = Image.FromStream(s);
        }
        catch { _art = null; }
        _timer.Tick += (s, e) => Invalidate(new Rectangle(0, 0, Width, _cardTop));
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
            if (_still != null) _still.Dispose();
            if (_fogA != null) _fogA.Dispose();
            if (_fogB != null) _fogB.Dispose();
            if (_art != null) _art.Dispose();
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
        int sw = Math.Max(8, Width / 4), sh = Math.Max(8, _cardTop / 4);
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
                    // Thinner at the top and where the card begins.
                    float edge = Math.Min(1f, Math.Min(y / (sh * 0.25f), (sh - 1 - y) / (sh * 0.2f)));
                    a *= Math.Max(0f, edge);
                    int i = y * data.Stride + x * 4;
                    px[i]     = (byte)(148 * a);
                    px[i + 1] = (byte)(156 * a);
                    px[i + 2] = (byte)(158 * a);
                    px[i + 3] = (byte)(255 * a);
                }
            }
            System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
            small.UnlockBits(data);

            var big = new Bitmap(Width * 2, _cardTop, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(big))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                g.PixelOffsetMode   = PixelOffsetMode.Half;
                g.DrawImage(small, new Rectangle(0, 0, Width, _cardTop));
                g.DrawImage(small, new Rectangle(Width, 0, Width, _cardTop));
            }
            return big;
        }
    }

    private void BuildStill()
    {
        Image bannerImg = BannerImage != null ? BannerImage() : null;
        if (_still != null) _still.Dispose();
        _still = new Bitmap(Math.Max(1, Width), Math.Max(1, _cardTop), PixelFormat.Format32bppPArgb);
        _stillBanner = bannerImg;
        using (var g = Graphics.FromImage(_still))
        {
            int bh = 0;
            g.Clear(Color.FromArgb(8, 9, 8));
            if (bannerImg != null)
            {
                bh = Width * bannerImg.Height / Math.Max(1, bannerImg.Width);
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(bannerImg, new Rectangle(0, 0, Width, bh));
            }
            if (_art != null)
            {
                // Cover the strip under the banner, keeping the art's shape.
                var dst = new Rectangle(0, bh, Width, _cardTop - bh);
                float sa = (float)_art.Width / _art.Height, da = (float)dst.Width / Math.Max(1, dst.Height);
                RectangleF src = sa > da
                    ? new RectangleF((_art.Width - _art.Height * da) / 2f, 0, _art.Height * da, _art.Height)
                    : new RectangleF(0, (_art.Height - _art.Width / da) * 0.62f, _art.Width, _art.Width / da);
                g.DrawImage(_art, dst, src, GraphicsUnit.Pixel);
                using (var veil = new SolidBrush(Color.FromArgb(120, 5, 6, 5)))
                    g.FillRectangle(veil, dst);
            }
            using (var fade = new LinearGradientBrush(new Rectangle(0, _cardTop - 70, Width, 71),
                                                      Color.FromArgb(0, 14, 15, 14), Color.FromArgb(255, 14, 15, 14), 90f))
                g.FillRectangle(fade, 0, _cardTop - 70, Width, 70);
        }
    }

    private void BuildMoving()
    {
        _fogA = MakeFog(11, 0.55f);
        _fogB = MakeFog(47, 0.38f);
        _flakes = new Flake[70];
        for (int i = 0; i < _flakes.Length; i++)
        {
            int layer = i % 3;
            _flakes[i] = new Flake
            {
                X = (float)_rng.NextDouble() * Width,
                Y = (float)_rng.NextDouble() * _cardTop,
                Speed = 9f + layer * 7f + (float)_rng.NextDouble() * 4f,
                Sway  = 4f + (float)_rng.NextDouble() * 6f,
                Phase = (float)_rng.NextDouble() * 6.28f,
                Size  = 2 + (layer == 2 ? 1 : 0),
                Alpha = 120 + layer * 45,
            };
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        long t0 = Stopwatch.GetTimestamp();

        if (_still == null || _still.Width != Width ||
            (BannerImage != null && !ReferenceEquals(BannerImage(), _stillBanner)))
        {
            BuildStill();
        }
        g.DrawImageUnscaled(_still, 0, 0);
        using (var card = new SolidBrush(BackColor))
            g.FillRectangle(card, 0, _cardTop, Width, Height - _cardTop);

        if (_animated)
        {
            if (_fogA == null) BuildMoving();
            double t = _clock.Elapsed.TotalSeconds;
            int xa = (int)(t * 9.0) % Width, xb = Width - 1 - (int)(t * 5.0) % Width;
            g.SetClip(new Rectangle(0, 0, Width, _cardTop));
            g.CompositingQuality = CompositingQuality.HighSpeed;
            g.DrawImageUnscaled(_fogA, -xa, 0);
            g.DrawImageUnscaled(_fogB, -xb, 0);
            for (int i = 0; i < _flakes.Length; i++)
            {
                var f = _flakes[i];
                float y = (f.Y + (float)t * f.Speed) % _cardTop;
                float x = (f.X + (float)Math.Sin(t * 0.6 + f.Phase) * f.Sway + (float)t * f.Speed * 0.2f) % Width;
                // Fade out into the card instead of popping at its edge.
                int a = f.Alpha;
                if (y > _cardTop - 40) a = (int)(a * (_cardTop - y) / 40f);
                using (var b = new SolidBrush(Color.FromArgb(a < 0 ? 0 : a, 235, 230, 244)))
                    g.FillRectangle(b, (int)x, (int)y, f.Size, f.Size);
            }
            g.ResetClip();

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
