using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Windows.Forms;
using SilentHillPC_Launcher;

// Simple mode: the launcher's plain view. A quality preset, a few boxes, the
// disc, the game language, and Play. A preset is only a set of values for the
// graphics settings in config.cfg, the same ones the advanced view shows; when
// they no longer match a preset (changed in the advanced view or in game) the
// list shows Custom. The game has the same table (pc_config.c): keep them alike.
public partial class Form1
{
    private bool          _simpleMode;
    private SimpleScene   _simple;
    private ComboBox      _sMinimap;
    private Button        _sPlay, _sControls, _sRA, _sMods;
    private Label         _aLblMode;
    private RadioButton   _aRadioSimple, _aRadioAdvanced;
    private bool          _simpleLoading;
    private int           _sPresetIdx;
    private bool          _sDither = true, _sLogging = true, _sWindowed, _sCrt, _sCustomRes;
    private Size          _advSize;
    private Point         _advLangAt;
    private ToolStripMenuItem _bannerMenuAdvanced;

    private struct SimpleHome
    {
        public Control Parent; public Point Location; public Size Size; public Color Back, Fore; public FlatStyle Flat;
    }
    private readonly Dictionary<Control, SimpleHome> _simpleHomes = new Dictionary<Control, SimpleHome>();

    private const string SimpleTitle = "SILENT HILL PC";
    private const int SimpleCustom = 5; // the list's last row; never clicked, only shown
    private static readonly string[] SimplePresetNames =
    {
        "Original", "Enhanced: Low Spec", "Enhanced: Normal", "Enhanced: High Quality", "Enhanced: Ultra Quality",
        "Custom",
    };
    private static readonly string[] SimplePresetAbout =
    {
        "The game as it was on the PlayStation: its own resolution, 30 fps and the original lighting.",
        "Desktop resolution and smooth textures with no extra effects. For older or integrated graphics.",
        "Desktop resolution, no FPS limit, 2x antialiasing, sharper textures and flashlight shadows.",
        "4x antialiasing and the modern flashlight with detailed shadows.",
        "8x antialiasing, the highest shadow detail and the whole town in view through thinner fog. For powerful graphics cards.",
        "Settings changed in Advanced mode or in the game.",
    };

    private static readonly Size  SimpleSize = new Size(760, 732);
    private static readonly Color SimpleEdit = Color.FromArgb(34, 36, 34);
    private static readonly Color SimpleText = Color.FromArgb(232, 230, 222);
    private static readonly Color SimpleDim  = Color.FromArgb(176, 176, 168);
    private const int SimpleRowTop = 104, SimpleRowH = 68, SimpleLeft = 40, SimpleRight = 480;

    private Rectangle SimplePresetRect(int i) { return new Rectangle(SimpleLeft - 8, SimpleRowTop + i * SimpleRowH, 420, SimpleRowH - 4); }
    private static readonly Rectangle SimpleDitherRect  = new Rectangle(SimpleRight, 106, 240, 26);
    private static readonly Rectangle SimpleLoggingRect = new Rectangle(SimpleRight, 138, 240, 26);
    private static readonly Rectangle SimpleWindowedRect = new Rectangle(SimpleRight, 170, 240, 26);
    private static readonly Rectangle SimpleCrtRect      = new Rectangle(SimpleRight, 202, 240, 26);
    private static readonly Rectangle SimpleModeSimple   = new Rectangle(118, 690, 110, 26);
    private static readonly Rectangle SimpleModeAdvanced = new Rectangle(236, 690, 150, 26);

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
        _simple.MouseMove += (s, e) =>
        {
            _simple.Cursor = SimpleHit(e.Location) != 0 ? Cursors.Hand : Cursors.Default;
            SimpleTipFor(e.Location);
        };
        _simple.MouseLeave += (s, e) => SimpleTipFor(new Point(-1, -1));

        _sMinimap = new ComboBox
        {
            Location = new Point(SimpleRight, 270), Size = new Size(240, 25), DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = SimpleEdit, ForeColor = SimpleText, FlatStyle = FlatStyle.Flat, Font = new Font("Segoe UI", 10f),
        };
        _sPlay = new Button
        {
            Text = "Play!", Location = new Point(SimpleLeft, 530), Size = new Size(680, 56),
            Font = new Font("Segoe UI", 17f, FontStyle.Bold),
            BackColor = Color.FromArgb(104, 26, 22), ForeColor = Color.White, FlatStyle = FlatStyle.Flat,
        };
        _sPlay.FlatAppearance.BorderColor = Color.FromArgb(170, 70, 58);
        _sPlay.Click += (s, e) => { if (btnPlay.Enabled) btnPlay_Click(btnPlay, e); };
        _sControls = SimpleButton("Controls", new Rectangle(SimpleLeft, 602, 130, 32), btnControls_Click);
        _sRA       = SimpleButton("RetroAchievements", new Rectangle(178, 602, 176, 32), (s, e) => btnRA_Click(btnRA, e));
        _sMods     = SimpleButton("Mod Manager", new Rectangle(362, 602, 170, 32), (s, e) => btnManager_Click(btnManager, e));

        _simple.Controls.AddRange(new Control[] { _sMinimap, _sPlay, _sControls, _sRA, _sMods });
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
        // A config without the key is from before simple mode existed: its
        // owner stays in the view they know and is asked once (SimpleOfferOnce).
        SimpleSet(config.Get("simple_mode", "0").Trim() == "1", false);
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
            _sMods.Text     = Loc.T("Mod Manager");
        }
        finally { _simpleLoading = was; }
    }

    // A row's description is cut with "..." when it does not fit its two lines
    // (longer in some languages): hovering the row shows all of it.
    private readonly ToolTip _sTip = new ToolTip { AutoPopDelay = 20000, InitialDelay = 350, ReshowDelay = 150 };
    private int _sTipRow = -1;

    private void SimpleTipFor(Point p)
    {
        int row = -1;
        for (int i = 0; i <= SimpleCustom; i++)
            if (SimplePresetRect(i).Contains(p)) { row = i; break; }
        if (row == _sTipRow) return;
        _sTipRow = row;
        _sTip.SetToolTip(_simple, row < 0 ? "" : Loc.T(SimplePresetNames[row]) + "\n" + Loc.T(SimplePresetAbout[row]));
    }

    // 1..5 a preset row, 10 dithering, 11 logging, 12 windowed, 20 / 21 the mode pair.
    private int SimpleHit(Point p)
    {
        for (int i = 0; i < SimpleCustom; i++)
            if (SimplePresetRect(i).Contains(p)) return 1 + i;
        if (SimpleDitherRect.Contains(p))   return 10;
        if (SimpleLoggingRect.Contains(p))  return 11;
        if (SimpleWindowedRect.Contains(p)) return 12;
        if (SimpleCrtRect.Contains(p))      return 13;
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
        if (hit >= 1 && hit <= 5) { _sPresetIdx = hit - 1; SimpleApplyPreset(_sPresetIdx); SimpleApplyWindow(); _sCustomRes = false; }
        else if (hit == 10)
        {
            _sDither = !_sDither;
            config.Set("dithering", _sDither ? "1" : "0");
            if (_sDither) config.Set("dither_resolution", "0"); // the PSX pixel grid
        }
        else if (hit == 11) { _sLogging = !_sLogging; config.Set("enable_debug_log", _sLogging ? "1" : "0"); }
        else if (hit == 12)
        {
            _sWindowed = !_sWindowed;
            SimpleApplyWindow();
            // The window choice sets the size again; only Original's own
            // (native) resolution is not a size and stays as it is.
            _sCustomRes = _sPresetIdx == 0 && config.Get("native_resolution", "0").Trim() != "1";
        }
        else if (hit == 13) { _sCrt = !_sCrt; config.Set("post_process", _sCrt ? "1" : "0"); } // 1 = CRT
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
            SimpleOutlined(g, SimpleTitle, title, Color.FromArgb(214, 70, 56), new RectangleF(SimpleLeft - 2, 22, 680, 50));
            SimpleOutlined(g, Loc.T("Quality"), item, SimpleDim, new RectangleF(SimpleLeft, 76, 400, 24));
            SimpleOutlined(g, Loc.T("Options"), item, SimpleDim, new RectangleF(SimpleRight, 76, 240, 24));

            for (int i = 0; i <= SimpleCustom; i++)
            {
                var r = SimplePresetRect(i);
                bool on = i == _sPresetIdx;
                // Custom cannot be picked: it only says the settings are the player's own.
                bool grey = i == SimpleCustom && !on;
                if (on)
                    using (var hl = new SolidBrush(Color.FromArgb(70, 120, 40, 32)))
                        g.FillRectangle(hl, r);
                SimpleMark(g, new Rectangle(r.X + 8, r.Y + 4, 18, 18), true, on);
                SimpleOutlined(g, Loc.T(SimplePresetNames[i]), head,
                               on ? Color.White : (grey ? Color.FromArgb(112, 112, 106) : SimpleText),
                               new RectangleF(r.X + 36, r.Y + 2, r.Width - 40, 24));
                if (on && _sCustomRes && i != SimpleCustom)
                {
                    float nameW = g.MeasureString(Loc.T(SimplePresetNames[i]), head).Width;
                    SimpleOutlined(g, Loc.T("(Custom Resolution)"), body, SimpleDim,
                                   new RectangleF(r.X + 36 + nameW, r.Y + 6, r.Right - (r.X + 36 + nameW), 20));
                }
                SimpleOutlined(g, Loc.T(SimplePresetAbout[i]), body, grey ? Color.FromArgb(96, 96, 92) : SimpleDim,
                               new RectangleF(r.X + 36, r.Y + 25, r.Width - 44, 40));
            }

            SimpleMark(g, SimpleDitherRect, false, _sDither);
            SimpleOutlined(g, Loc.T("Dithering"), item, SimpleText,
                           new RectangleF(SimpleDitherRect.X + 28, SimpleDitherRect.Y + 1, 210, 24));
            SimpleMark(g, SimpleLoggingRect, false, _sLogging);
            SimpleOutlined(g, Loc.T("Logging"), item, SimpleText,
                           new RectangleF(SimpleLoggingRect.X + 28, SimpleLoggingRect.Y + 1, 210, 24));
            SimpleMark(g, SimpleWindowedRect, false, _sWindowed);
            SimpleOutlined(g, Loc.T("Windowed"), item, SimpleText,
                           new RectangleF(SimpleWindowedRect.X + 28, SimpleWindowedRect.Y + 1, 210, 24));
            SimpleMark(g, SimpleCrtRect, false, _sCrt);
            SimpleOutlined(g, Loc.T("CRT Filter"), item, SimpleText,
                           new RectangleF(SimpleCrtRect.X + 28, SimpleCrtRect.Y + 1, 210, 24));

            SimpleOutlined(g, Loc.T("Minimap:"), item, SimpleText, new RectangleF(SimpleRight, 246, 240, 22));
            SimpleOutlined(g, Loc.T("Disc:"), item, SimpleText, new RectangleF(SimpleRight, 308, 240, 22));
            SimpleOutlined(g, Loc.T("Game Language:"), item, SimpleText, new RectangleF(SimpleRight, 370, 240, 22));

            if (!string.IsNullOrEmpty(lblUpdateStatus.Text))
                SimpleOutlined(g, lblUpdateStatus.Text, body,
                               lblUpdateStatus.ForeColor.GetBrightness() < 0.45f ? SimpleDim : lblUpdateStatus.ForeColor,
                               new RectangleF(SimpleLeft, 646, 350, 20));

            SimpleOutlined(g, Loc.T("Mode:"), item, SimpleText, new RectangleF(SimpleLeft, 692, 80, 22));
            SimpleMark(g, SimpleModeSimple, true, true);
            SimpleOutlined(g, Loc.T("Simple"), item, SimpleText,
                           new RectangleF(SimpleModeSimple.X + 28, SimpleModeSimple.Y + 1, 90, 22));
            SimpleMark(g, SimpleModeAdvanced, true, false);
            SimpleOutlined(g, Loc.T("Advanced"), item, SimpleText,
                           new RectangleF(SimpleModeAdvanced.X + 28, SimpleModeAdvanced.Y + 1, 130, 22));
        }
    }

    // ---- presets: the same table as the game's PcConfig_SimplePresetValues ----

    private static Dictionary<string, string> SimplePresetValues(int id)
    {
        var v = new Dictionary<string, string>
        {
            { "disable_culling", "1" }, { "preload_chunks", "1" },
            { "fog_strength", "1.10" }, { "whole_map_exteriors", "2" },
        };
        if (id == 0)
        {
            v["native_resolution"] = "1"; v["vsync"] = "0"; v["fps_cap"] = "30"; v["texture_filter"] = "0";
            v["msaa"] = "0"; v["use_pgxp"] = "0"; v["flashlight_mode"] = "0"; v["shadow_resolution"] = "1024";
            v["bullet_decals"] = "0";
        }
        else
        {
            string[] filter = { "0", "1", "4", "5", "6" }, msaa = { "0", "0", "2", "4", "8" };
            string[] flash = { "0", "0", "1", "3", "3" }, shadow = { "1024", "512", "2048", "4096", "8192" };
            v["native_resolution"] = "0"; v["vsync"] = "1"; v["fps_cap"] = "0"; v["use_pgxp"] = "1";
            v["bullet_decals"] = "1"; v["widescreen_mode"] = "1"; v["menu_pillarbox"] = "1";
            v["texture_filter"] = filter[id]; v["msaa"] = msaa[id];
            if (id == 4)
            {
                // Ultra alone thins the fog and draws the whole town through it.
                v["fog_strength"] = "0.95"; v["whole_map_exteriors"] = "1"; v["world_visibility"] = "1";
            }
            v["flashlight_mode"] = flash[id]; v["shadow_resolution"] = shadow[id];
        }
        return v;
    }

    // What the game uses for a key the config does not have.
    private static readonly Dictionary<string, string> SimpleDefaults = new Dictionary<string, string>
    {
        { "disable_culling", "1" }, { "preload_chunks", "1" }, { "post_process", "0" }, { "tonemap", "0" },
        { "render_scale", "1" }, { "native_resolution", "0" }, { "vsync", "0" }, { "fps_cap", "30" },
        { "texture_filter", "0" }, { "msaa", "0" }, { "use_pgxp", "0" }, { "flashlight_mode", "0" },
        { "shadow_resolution", "1024" }, { "bullet_decals", "0" }, { "widescreen_mode", "1" }, { "menu_pillarbox", "1" },
        { "fog_strength", "1.10" }, { "whole_map_exteriors", "2" }, { "world_visibility", "1" },
    };

    private static double SimpleNumber(string s)
    {
        double d;
        return double.TryParse((s ?? "").Trim(), System.Globalization.NumberStyles.Float,
                               System.Globalization.CultureInfo.InvariantCulture, out d) ? d : double.NaN;
    }

    /// <summary>The list row the config's settings match: a preset, or Custom.</summary>
    private int SimpleDetectPreset()
    {
        for (int id = 0; id < SimpleCustom; id++)
        {
            bool same = true;
            foreach (var kv in SimplePresetValues(id))
            {
                // The render size is set when a preset is picked, and may be
                // changed afterwards without leaving the preset.
                if (kv.Key == "native_resolution") continue;
                // Culling is set by a preset but is the player's to change.
                if (kv.Key == "disable_culling") continue;
                // On or automatic both draw the town; only Ultra needs it, and
                // needs it not switched off.
                if (kv.Key == "whole_map_exteriors")
                {
                    if (id == 4 && config.Get(kv.Key, "2").Trim() == "0") { same = false; break; }
                    continue;
                }
                double have = SimpleNumber(config.Get(kv.Key, SimpleDefaults[kv.Key]));
                if (double.IsNaN(have) || Math.Abs(have - SimpleNumber(kv.Value)) > 0.001) { same = false; break; }
            }
            if (same) return id;
        }
        return SimpleCustom;
    }

    private void SimpleApplyPreset(int id)
    {
        foreach (var kv in SimplePresetValues(id)) config.Set(kv.Key, kv.Value);
        string fm = config.Get("flashlight_mode", "0").Trim();
        config.Set("per_pixel_flashlight", fm != "0" ? "1" : "0");
        config.Set("flashlight_shadows", (fm == "1" || fm == "3") ? "1" : "0");
    }

    /// <summary>Borderless at the desktop's size, or a window: 640x480 for
    /// Original, 1280x720 otherwise. Only on a click, so a size picked in the
    /// game or the advanced view stands until the player asks for another.</summary>
    private void SimpleApplyWindow()
    {
        var scr = Screen.PrimaryScreen.Bounds;
        bool original = _sPresetIdx == 0;
        config.Set("fullscreen", _sWindowed ? "0" : "2");
        config.Set("width",  (_sWindowed ? (original ? 640 : 1280) : scr.Width).ToString());
        config.Set("height", (_sWindowed ? (original ? 480 : 720)  : scr.Height).ToString());
    }

    /// <summary>True when the render size is not the one the preset sets: the
    /// PSX's own for Original; otherwise the desktop's, or 1280x720 in a window.</summary>
    private bool SimpleResolutionIsCustom(int preset)
    {
        if (preset < 0 || preset >= SimpleCustom) return false;
        bool native = config.Get("native_resolution", "0").Trim() == "1";
        if (preset == 0) return !native;
        if (native) return true;
        var scr = Screen.PrimaryScreen.Bounds;
        bool windowed = config.Get("fullscreen", "0").Trim() == "0";
        int w = ParseIntOr(config.Get("width", "640"), 640), h = ParseIntOr(config.Get("height", "480"), 480);
        return windowed ? (w != 1280 || h != 720) : (w != scr.Width || h != scr.Height);
    }

    /// <summary>Show config.cfg in the simple view.</summary>
    private void SimpleLoad()
    {
        if (_simple == null) return;
        _simpleLoading = true;
        try
        {
            config.RefreshFromDisk();
            _sPresetIdx = SimpleDetectPreset();
            _sCustomRes = SimpleResolutionIsCustom(_sPresetIdx);
            _sDither    = config.Get("dithering", "1").Trim() != "0";
            _sLogging   = config.Get("enable_debug_log", "0").Trim() != "0";
            _sWindowed  = config.Get("fullscreen", "0").Trim() == "0";
            _sCrt       = config.Get("post_process", "0").Trim() == "1";

            int mm = ParseIntOr(config.Get("minimap", "0"), 0);
            int mc = ParseIntOr(config.Get("minimap_corner", "0"), 0);
            if (mc < 0 || mc > 3) mc = 0;
            int mi = mm == 0 ? 0 : (mm == 2 ? 1 + mc : 5 + mc);
            _sMinimap.SelectedIndex = mi < _sMinimap.Items.Count ? mi : 0;
        }
        finally { _simpleLoading = false; }
        _simple.ContentChanged();
    }

    /// <summary>Write what the simple view holds besides the clicks already
    /// set: the minimap, the disc and the language. The advanced view's
    /// controls are not consulted, so nothing they show is written back.</summary>
    private void SimpleSave()
    {
        if (_simpleLoading || !_simpleMode) return;
        try
        {
            int mi = _sMinimap.SelectedIndex >= 0 ? _sMinimap.SelectedIndex : 0;
            config.Set("minimap", mi == 0 ? "0" : (mi <= 4 ? "2" : "1"));
            config.Set("minimap_corner", mi == 0 ? "0" : (mi <= 4 ? mi - 1 : mi - 5).ToString());
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

    /// <summary>Asked once of anyone whose config is from before simple mode
    /// existed. The flag is saved before asking, so no answer, and no later
    /// launcher update, brings the question back.</summary>
    private void SimpleOfferOnce()
    {
        try
        {
            if (_simpleMode) return;
            if (config.Get("launcher_simple_offered", "0").Trim() == "1") return;
            config.EnsureLauncherSection();
            config.Set("launcher_simple_offered", "1");
            config.Save();
            // Chosen already (in game, or by hand): nothing to ask.
            if (config.Get("simple_mode", "").Trim() != "") return;
            if (MessageBox.Show(this,
                    Loc.T("Simple Mode has been added to the launcher, would you like to switch to it now?"),
                    Loc.T("Simple Mode"), MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                SimpleSet(true, true);
        }
        catch { /* best effort: never block startup over this */ }
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

        // What the advanced view shows becomes the config before the simple
        // view reads it, so its list starts from the player's real settings.
        if (on && changed && persist)
        {
            try { SaveConfig(); } catch { }
        }
        _simpleMode = on;

        if (on)
        {
            ClientSize = SimpleSize;
            // The pieces both views share are moved rather than copied, so the
            // disc and language logic and the update flow keep working as is.
            SimpleBorrow(comboDisc,     new Point(SimpleRight, 332), new Size(240, 25));
            SimpleBorrow(comboGameLang, new Point(SimpleRight, 394), new Size(240, 25));
            SimpleBorrow(btnUpdate,     new Point(540, 602), new Size(180, 32));
            SimpleBorrow(progUpdate,    new Point(400, 648), new Size(320, 16));
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
/// snow, and the view's text drawn over it. Painted with GDI+; on a machine where
/// a frame costs too much it moves in rarer steps, and in a remote session it is
/// a still picture.
/// </summary>
internal sealed class SimpleScene : Panel
{
    private static readonly Color Ground = Color.FromArgb(10, 11, 10);
    private readonly Timer _timer = new Timer { Interval = 40 };
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly Random _rng = new Random(1999);
    private const int FogMargin = 4; // cells either side that are never on screen
    private Bitmap _fogA, _fogB;    // two fog banks, each two periods wide
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
        // Two periods and a margin, scaled up in ONE piece: scaling one period
        // and pasting it twice left a faint vertical line where the copies met,
        // because the smoothing stops at a bitmap's edge.
        int sw = Math.Max(8, Width / 4), sh = Math.Max(8, Height / 4);
        int cols = sw * 2 + FogMargin * 2;
        using (var small = new Bitmap(cols, sh, PixelFormat.Format32bppPArgb))
        {
            var data = small.LockBits(new Rectangle(0, 0, cols, sh), ImageLockMode.WriteOnly, PixelFormat.Format32bppPArgb);
            var px = new byte[data.Stride * sh];
            for (int y = 0; y < sh; y++)
            {
                for (int cx = 0; cx < cols; cx++)
                {
                    int   x = cx - FogMargin;
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
                    int i = y * data.Stride + cx * 4;
                    px[i]     = (byte)(150 * a);
                    px[i + 1] = (byte)(156 * a);
                    px[i + 2] = (byte)(158 * a);
                    px[i + 3] = (byte)(255 * a);
                }
            }
            System.Runtime.InteropServices.Marshal.Copy(px, 0, data.Scan0, px.Length);
            small.UnlockBits(data);

            var big = new Bitmap(cols * 4, Height, PixelFormat.Format32bppPArgb);
            using (var g = Graphics.FromImage(big))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                g.PixelOffsetMode   = PixelOffsetMode.Half;
                g.DrawImage(small, new Rectangle(0, 0, cols * 4, Height));
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
            int period = Math.Max(8, Width / 4) * 4;
            int xa = (int)(t * 9.0) % period, xb = period - 1 - (int)(t * 5.0) % period;
            g.CompositingQuality = CompositingQuality.HighSpeed;
            g.DrawImageUnscaled(_fogA, -xa - FogMargin * 4, 0);
            g.DrawImageUnscaled(_fogB, -xb - FogMargin * 4, 0);
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
            // When frames cost too much the fog moves in bigger, rarer steps,
            // and goes back to the full rate once they are cheap again. It never
            // stops: one busy spell (the game starting, a build running) used to
            // freeze it for as long as the launcher stayed open.
            _costMs += (Stopwatch.GetTimestamp() - t0) * 1000.0 / Stopwatch.Frequency;
            if (++_frames == 20)
            {
                double avg = _costMs / _frames;
                if (avg > 16.0 && _timer.Interval < 320) _timer.Interval *= 2;
                else if (avg < 6.0 && _timer.Interval > 40) _timer.Interval /= 2;
                _costMs = 0;
                _frames = 0;
            }
        }
    }
}
