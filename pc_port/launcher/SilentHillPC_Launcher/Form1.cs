using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using SilentHillPC_Launcher;

public partial class Form1 : Form
{
    private ConfigManager config;

    // Detected discs (filled by CheckDiscImage). The Disc dropdown is the
    // whole disc UI now — region comes from each image's boot serial, and
    // the `region` config key is only honored as a hand-set auto-pick
    // preference. PAL text language stays a config-only key
    // (`language = en/de/fr/es/it`).
    private List<DiscProbe.Disc> _discs = new List<DiscProbe.Disc>();
    // comboDisc entry -> config `disc_image` value ("" = auto pick).
    private readonly List<string> _discIds = new List<string>();
    private bool _regionUiUpdating;

    // Level select (comboMap): map overlay id -> friendly name. Canonical
    // names from the upstream README (the pre-Mod-Manager dropdown parsed
    // these from config.cfg comments, which no longer carry them). Only the
    // id before the separator is persisted to the `map` config key.
    private static readonly string[] s_mapEntries = {
        "map0_s00  -  Old Silent Hill - intro sequence",
        "map0_s01  -  Old Silent Hill - cafe",
        "map0_s02  -  Old Silent Hill - bonus unlockable areas",
        "map1_s00  -  School - 1F, courtyard, basement",
        "map1_s01  -  School - 2F",
        "map1_s02  -  School Otherworld - 1F and courtyard",
        "map1_s03  -  School Otherworld - 2F and roof",
        "map1_s04  -  Unused",
        "map1_s05  -  School - boss fight (Split Head)",
        "map1_s06  -  School - 1F and basement after the boss",
        "map2_s00  -  Old Silent Hill - streets",
        "map2_s01  -  Church",
        "map2_s02  -  Central Silent Hill - streets",
        "map2_s03  -  Unused",
        "map2_s04  -  Police station (Central Silent Hill)",
        "map3_s00  -  Hospital - until Kaufmann meeting",
        "map3_s01  -  Hospital - 1F and basement after Kaufmann",
        "map3_s02  -  Hospital - antique shop cutscene",
        "map3_s03  -  Hospital Otherworld - 3F and 2F",
        "map3_s04  -  Hospital Otherworld - 1F",
        "map3_s05  -  Hospital Otherworld - basement",
        "map3_s06  -  Hospital - 1F after Otherworld",
        "map4_s00  -  Unused",
        "map4_s01  -  Green Lion Antiques (normal + Otherworld)",
        "map4_s02  -  Central Silent Hill Otherworld - streets",
        "map4_s03  -  Mall and boss fight",
        "map4_s04  -  Hospital - 1F (Lisa cutscene)",
        "map4_s05  -  Central SH Otherworld - Floatstinger boss",
        "map4_s06  -  Unused",
        "map5_s00  -  Sewers - lower and upper levels",
        "map5_s01  -  Resort Area",
        "map5_s02  -  Annie's Bar and Indian Runner (Resort Area)",
        "map5_s03  -  Norman's Motel (Resort Area)",
        "map6_s00  -  Resort Area Otherworld",
        "map6_s01  -  Boat at Lakeside Pier",
        "map6_s02  -  Lakeside Pier and Lighthouse",
        "map6_s03  -  Sewer to Lakeside Amusement Park",
        "map6_s04  -  Amusement Park - Cybil boss, Alessa kidnapping",
        "map6_s05  -  Unused",
        "map7_s00  -  Nowhere - hospital 1F, Lisa cutscene",
        "map7_s01  -  Nowhere",
        "map7_s02  -  Nowhere - Alessa vs. Dahlia cutscene",
        "map7_s03  -  Nowhere - final boss",
    };

    private static string RegionDisplayName(string region)
    {
        switch (region)
        {
            case "USA": return "NTSC-U (USA)";
            case "PAL": return "PAL (Europe)";
            case "JAP": return "NTSC-J (Japan)";
            default:    return region;
        }
    }

    public Form1()
    {
        InitializeComponent();
        this.Icon = SilentHillPC_Launcher.Properties.Resources.launchericon;
        PopulateDisplayOptions();
        LoadConfig();
        SetupTooltips();
        SetupLanguageButton();
        this.Shown += (s, e) =>
        {
            CleanupOldFiles();
            UpdateChecker.CleanupStaleTemp();
            CheckDiscImage();
            FfmpegCheck.WarnIfNeeded(this, AppDomain.CurrentDomain.BaseDirectory);
            SilentAutoCheckForUpdates();
        };
    }


    // ---- launcher language -------------------------------------------------

    private Button btnLang;
    private readonly ToolTip _langTip = new ToolTip();

    /// <summary>
    /// The flag button in the bottom-right corner, sitting in the space the
    /// update progress bar gave up. Clicking it opens the language picker; the
    /// flag itself is the current language's, so the setting is readable at a
    /// glance without opening anything.
    /// </summary>
    private void SetupLanguageButton()
    {
        try { Loc.Current = LauncherSettings.Load(config).Language; }
        catch { Loc.Current = LauncherLang.English; }

        LocalizeValueLists();

        btnLang = new Button
        {
            /* The right column is full from y 469 down: comboMap 469..490,
             * progUpdate 503..526, lblUpdateStatus 533..548 -- so 416,472 landed
             * ON TOP of the Level dropdown and the randomizer checkbox once the
             * new options moved in. Back on the right edge, one row lower: the
             * only things it shares that band with are progUpdate and
             * chkRandomizer, both Visible=false in normal use (the progress bar
             * appears during a download). Created in code rather than the designer (the flag
             * is custom-painted), so it cannot be nudged from the design surface;
             * re-check this against Form1.Designer.cs after any layout change. */
            Location = new System.Drawing.Point(416, 504),
            Size = new System.Drawing.Size(30, 22),
            Text = "",
            FlatStyle = FlatStyle.Standard,
            TabStop = false
        };
        // The flag is PAINTED, not typed: emoji flags are regional-indicator
        // pairs that only DirectWrite ligates, so GDI would draw "US".
        btnLang.Paint += (s2, e2) =>
        {
            var box = new System.Drawing.Rectangle(4, 4, btnLang.Width - 9, btnLang.Height - 9);
            FlagIcon.Draw(e2.Graphics, box, Loc.Current);
        };
        btnLang.Click += btnLang_Click;
        Controls.Add(btnLang);
        btnLang.BringToFront();
        _langTip.SetToolTip(btnLang, Loc.T("Launcher language"));

        Loc.Changed += OnLanguageChanged;
        if (Loc.Current != LauncherLang.English) OnLanguageChanged();
    }

    private readonly Dictionary<Control, Func<string>> _dynText =
        new Dictionary<Control, Func<string>>();

    /// <summary>
    /// Set a control's text from its English, and remember how to redo it.
    ///
    /// Loc.Apply can only restore the text a control was BORN with, so anything
    /// the update flow rewrites at runtime ("Update available!", the status
    /// line) would snap back to its designer text on a language switch. These
    /// are re-applied after Apply instead.
    /// </summary>
    private void SetText(Control c, string english, params object[] args)
    {
        if (c == null) return;
        Func<string> f = () => (args == null || args.Length == 0)
            ? Loc.T(english)
            : string.Format(Loc.T(english), args);
        _dynText[c] = f;
        c.Text = f();
    }

    /// <summary>
    /// Combos whose items are words rather than data. Only these get owner-draw
    /// translation — resolution, refresh rate, FPS cap, disc image and level are
    /// left alone because their selected text is what gets written to config.cfg.
    /// </summary>
    private void LocalizeValueLists()
    {
        // "はい"/"いいえ" and "Nein" are wider than "Yes"/"No", and each of these
        // sits at the end of its row with free space to the right.
        foreach (var r in new[] { radioVsyncYes, radioVsyncNo, radioDecalsYes, radioDecalsNo, radioPreloadYes, radioPreloadNo,
                                  pgxpYes, pgxpNo, loggingYes, loggingNo, consoleYes, consoleNo })
            r.AutoSize = true;

        Loc.LocalizeItems(comboFullscreen);
        Loc.LocalizeItems(comboSkipIntros);
        Loc.LocalizeItems(comboPillarbox);
        Loc.LocalizeItems(comboFiltering);
        Loc.LocalizeItems(comboAA);
        Loc.LocalizeItems(comboPost);
        Loc.LocalizeItems(comboTone);
        Loc.LocalizeItems(comboFlash);
        Loc.LocalizeItems(comboAudioOut);
    }

    private readonly Dictionary<Label, int> _labelHome = new Dictionary<Label, int>();

    /// <summary>
    /// Nudge a label left when its translation would otherwise run under the
    /// control beside it — the labels are AutoSize, so a longer word grows to
    /// the right and paints over the combo it belongs to.
    ///
    /// A label is never moved past whatever is to its left, and never past its
    /// designed position, so this only ever consumes slack the row already had.
    /// </summary>
    private void FitLabels(Control root)
    {
        foreach (Control c in root.Controls)
        {
            var lbl = c as Label;
            if (lbl == null) { FitLabels(c); continue; }

            int home;
            if (!_labelHome.TryGetValue(lbl, out home)) { home = lbl.Left; _labelHome[lbl] = home; }
            lbl.Left = home;

            int rightX = int.MaxValue, leftEdge = 4;
            foreach (Control o in root.Controls)
            {
                if (ReferenceEquals(o, lbl) || Math.Abs(o.Top - lbl.Top) > 12) continue;
                if (o.Left > home && o.Left < rightX) rightX = o.Left;
                if (o.Right <= home && o.Right + 4 > leftEdge) leftEdge = o.Right + 4;
            }
            if (rightX == int.MaxValue) continue;

            int w = TextRenderer.MeasureText(lbl.Text, lbl.Font).Width;
            int want = rightX - 4 - w;
            if (want < home) lbl.Left = Math.Max(leftEdge, want);
        }
    }

    private void OnLanguageChanged()
    {
        Loc.Apply(this);
        SetupTooltips();
        foreach (var kv in _dynText) kv.Key.Text = kv.Value();
        FitLabels(this);
        if (btnLang != null)
        {
            btnLang.Invalidate();
            _langTip.SetToolTip(btnLang, Loc.T("Launcher language"));
        }
    }

    private void btnLang_Click(object sender, EventArgs e)
    {
        var before = Loc.Current;
        using (var dlg = new LanguageDialog(Loc.Current))
        {
            dlg.ShowDialog(this);
            Loc.Set(dlg.Selected);
        }
        if (Loc.Current == before) return;

        try
        {
            var st = LauncherSettings.Load(config);
            st.Language = Loc.Current;
            st.Save(config);
        }
        catch { /* config not writable - the choice still applies this session */ }
    }

    /// <summary>
    /// Delete *.old leftovers from a previous self-update (a running exe
    /// can't be overwritten, so ReplaceFile renames it aside; by the next
    /// launcher start the old process is gone and the file is unlocked).
    /// </summary>
    private void CleanupOldFiles()
    {
        try
        {
            string dir = AppDomain.CurrentDomain.BaseDirectory;
            foreach (var f in Directory.GetFiles(dir, "*.old"))
                try { File.Delete(f); } catch { /* still locked — next time */ }

            string maps = Path.Combine(dir, "maps");
            if (Directory.Exists(maps))
                foreach (var f in Directory.GetFiles(maps, "*.old"))
                    try { File.Delete(f); } catch { }
        }
        catch { /* best effort */ }
    }

    /// <summary>
    /// Detect the disc image(s) at startup (creating gamedata/ if needed) and
    /// show the identified serial/region in lblDisc; warn only when no
    /// recognizable .bin exists at all. Any filename is accepted — discs are
    /// identified by their ISO boot serial exactly like the game does. Gated
    /// on SilentHillPC.exe being present so dev runs from bin\Release don't
    /// nag or scaffold stray folders.
    /// </summary>
    private void CheckDiscImage()
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        if (!File.Exists(Path.Combine(dir, "SilentHillPC.exe"))) return;

        string gamedata = Path.Combine(dir, "gamedata");
        try { if (!Directory.Exists(gamedata)) Directory.CreateDirectory(gamedata); }
        catch { return; }

        _discs = DiscProbe.Scan(gamedata);

        _regionUiUpdating = true;
        comboDisc.Items.Clear();
        _discIds.Clear();
        comboDisc.Enabled = false;

        if (_discs.Count == 0)
        {
            _regionUiUpdating = false;
            SetText(lblDisc, "No disc image found in gamedata\\");
            MessageBox.Show(this,
                "No Silent Hill disc image found.\n\n" +
                "Please put a Silent Hill .bin (USA or PAL/European release)\n" +
                "in the gamedata folder!",
                "Silent Hill PC",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        // Disc dropdown: Auto (the game's region rules) plus every detected
        // image. Picking a specific file writes `disc_image`, which is how a
        // fan-translated / modified copy gets selected over the vanilla one.
        comboDisc.Items.Add("Disc: Auto");
        _discIds.Add("");
        foreach (var d in _discs)
        {
            comboDisc.Items.Add(d.FileName + (d.Modified ? "  [modified]" : ""));
            _discIds.Add(d.FileName);
        }
        comboDisc.Enabled = _discs.Count > 0;

        // Restore the saved filename case-insensitively (NTFS is; the game's
        // fopen is too). A saved value whose file is gone/renamed gets its own
        // "[missing]" entry so a hand-set config value is preserved on Play
        // instead of being silently reset to Auto.
        string savedDisc = config.Get("disc_image", "");
        int discIdx = _discIds.FindIndex(
            id => string.Equals(id, savedDisc, StringComparison.OrdinalIgnoreCase));
        if (discIdx < 0 && savedDisc.Length > 0)
        {
            comboDisc.Items.Add(savedDisc + "  [missing]");
            _discIds.Add(savedDisc);
            discIdx = _discIds.Count - 1;
        }
        comboDisc.SelectedIndex = discIdx >= 0 ? discIdx : 0;
        _regionUiUpdating = false;

        UpdateDiscUi();
    }

    /// <summary>
    /// Representative disc for a region ("USA"/"PAL"/"JAP"): mirror the
    /// game's pick order — known filenames first, then the first probed
    /// disc of that region.
    /// </summary>
    private DiscProbe.Disc DiscForRegion(string region)
    {
        string[] knownNames = {
            "Silent Hill (USA).bin",
            "Silent Hill (PAL).bin",
            "Silent Hill (Europe) (En,Fr,De,Es,It).bin",
            "Silent Hill (Japan).bin",
        };
        foreach (var kn in knownNames)
        {
            var d = _discs.FirstOrDefault(x => x.Region == region &&
                        x.FileName.Equals(kn, StringComparison.OrdinalIgnoreCase));
            if (d != null) return d;
        }
        return _discs.FirstOrDefault(x => x.Region == region);
    }

    /// <summary>
    /// The disc the game's Auto rules would boot: a hand-set `region`
    /// config preference first (the UI no longer writes the key), then
    /// USA > PAL > JAP priority.
    /// </summary>
    private DiscProbe.Disc AutoPickDisc()
    {
        string pref       = config.Get("region", "auto");
        string prefRegion = pref == "usa" ? "USA" : pref == "pal" ? "PAL" : pref == "jap" ? "JAP" : null;

        if (prefRegion != null)
        {
            var d = DiscForRegion(prefRegion);
            if (d != null) return d;
        }
        foreach (var region in new[] { "USA", "PAL", "JAP" })
        {
            var d = DiscForRegion(region);
            if (d != null) return d;
        }
        return null;
    }

    /// <summary>Explicitly selected disc, or null when the dropdown is on
    /// Auto (or on a saved-but-missing filename).</summary>
    private DiscProbe.Disc SelectedDisc()
    {
        if (comboDisc.SelectedIndex <= 0 || comboDisc.SelectedIndex >= _discIds.Count)
            return null;
        string id = _discIds[comboDisc.SelectedIndex];
        return _discs.FirstOrDefault(d =>
            string.Equals(d.FileName, id, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// Reflect the Disc selection: a specific disc means the game boots
    /// exactly that file (region from its serial); on Auto the status line
    /// shows what the game's own rules would pick. Unsupported serials
    /// (NTSC-J first prints etc.) disable Play with a highlight.
    /// </summary>
    private void UpdateDiscUi()
    {
        var disc = SelectedDisc();

        if (disc == null)
            disc = AutoPickDisc();
        if (disc == null)
            return;

        // Compact strings — the status label is a single 13px line.
        string text = $"{disc.Serial} {disc.Region}";

        if (!disc.Supported)
        {
            lblDisc.ForeColor = Color.Firebrick;
            lblDisc.Text      = text + " — " + (string.IsNullOrEmpty(disc.UnsupportedReason)
                                                ? "not supported yet"
                                                : disc.UnsupportedReason);
            btnPlay.Enabled   = false;
            return;
        }

        if (disc.Modified)
        {
            // Fan translation / patched image: the game reads its text and
            // voices straight off the disc.
            lblDisc.ForeColor = Color.RoyalBlue;
            lblDisc.Text      = text + " — fan patch";
        }
        else
        {
            lblDisc.ForeColor = SystemColors.ControlText;
            lblDisc.Text      = $"{disc.Serial} — {RegionDisplayName(disc.Region)}";
        }
        btnPlay.Enabled = true;
    }

    private void comboDisc_SelectedIndexChanged(object sender, EventArgs e)
    {
        if (_regionUiUpdating) return;
        UpdateDiscUi();
    }

    /// <summary>
    /// Fire-and-forget update probe on launcher startup. Doesn't pop a
    /// dialog or block anything — just updates lblUpdateStatus so the
    /// user sees "Update available: X" or "Up to date" without clicking.
    /// Click "Check for Updates" to actually run the install flow.
    ///
    /// Failures are silent (no network, no remote, etc.) — startup
    /// shouldn't yell at users about a missing connection.
    /// </summary>
    // Always checks the configured branch's LATEST build, regardless of which
    // build is pinned, and reports an update only when that latest is newer than
    // the highest version ever installed from this branch (so deliberately
    // running an old build isn't flagged). Build switching is the Download Build
    // button's job.
    private static LauncherSettings ForceLatest(LauncherSettings s)
        => new LauncherSettings { RepoUrl = s.RepoUrl, Branch = s.Branch, Build = "latest", OldBuildWarned = s.OldBuildWarned };

    private async void SilentAutoCheckForUpdates()
    {
        var settings = LauncherSettings.Load(config);
        string installDir = AppDomain.CurrentDomain.BaseDirectory;
        SetText(btnUpdate, "Check for Updates");
        SetText(downloadBuild, "Download Build");
        try
        {
            SetText(lblUpdateStatus, "Checking for updates...");
            var plan = await UpdateChecker.CheckAsync(installDir, ForceLatest(settings));

            // If the install already matches the latest, remember that so it
            // isn't mis-reported as an update later.
            if (!plan.HasUpdate) settings.RecordSeenAtLeast(config, plan.RemoteVersion);

            bool available = LauncherSettings.CompareVersions(plan.RemoteVersion, settings.GetSeen(config)) > 0;
            if (available)
            {
                SetText(btnUpdate, "Update available!");
                lblUpdateStatus.ForeColor = Color.LightGreen;
                SetText(lblUpdateStatus, "Update available: {0}", plan.RemoteVersion);
            }
            else
            {
                lblUpdateStatus.ForeColor = Color.LightGray;
                SetText(lblUpdateStatus, "Up to date ({0}).", plan.RemoteVersion);
            }

            // "Download Build" vs "Redownload Build": is the SELECTED build already
            // installed? For "latest" reuse the check above; for a pinned build do
            // a quick second check.
            bool selectedInstalled = settings.IsLatestBuild
                ? !plan.HasUpdate
                : !(await UpdateChecker.CheckAsync(installDir, settings)).HasUpdate;
            SetText(downloadBuild, selectedInstalled ? "Redownload Build" : "Download Build");
        }
        catch
        {
            // Silent — no internet, no nightly repo yet, etc.
            SetText(lblUpdateStatus, "");
        }
    }

    /// <summary>
    /// Hover tooltips for each option. Resolution/refresh-rate are
    /// self-explanatory and skipped. Tooltip text is intentionally short —
    /// long enough to explain non-obvious behavior, short enough to read at
    /// a glance.
    /// </summary>
    private void SetupTooltips()
    {
        var tip = new ToolTip
        {
            AutoPopDelay = 12000,  // keep visible up to 12s while hovered
            InitialDelay = 400,
            ReshowDelay  = 200,
            ShowAlways   = true,
        };

        /* Tooltips are localized here rather than at each call site: the English
         * text IS the dictionary key, so every Set() below stays readable and a
         * missing translation falls through to English on its own. SetupTooltips
         * is re-run by OnLanguageChanged, because a ToolTip's text is not a
         * control property and Loc.Apply cannot reach it. */
        void Set(Control c, string text)
        {
            if (c == null) return;
            text = Loc.T(text);
            tip.SetToolTip(c, text);
            // Radio buttons sit inside panels; attach to the panel too so
            // hovering between buttons doesn't drop the tooltip.
            if (c.Parent is Panel)
                tip.SetToolTip(c.Parent, text);
        }

        const string fullscreenTip =
            "Fullscreen = exclusive fullscreen at the chosen resolution.\n" +
            "Windowed = a normal window at the chosen resolution.\n" +
            "Borderless = covers the screen at desktop resolution\n" +
            "(no mode switch, fast alt-tab; refresh-rate setting not used).";
        Set(fullscreenLabel,   fullscreenTip);
        Set(comboFullscreen,   fullscreenTip);

        const string vsyncTip =
            "Synchronize frame presentation to your monitor's refresh rate.\n" +
            "Yes = no tearing, may add a frame of input latency.\n" +
            "No = lowest latency, possible tearing.";
        Set(vsyncLabel,     vsyncTip);
        Set(radioVsyncYes,  vsyncTip);
        Set(radioVsyncNo,   vsyncTip);

        const string fpsTip =
            "Maximum frames per second. 0 = unlimited.\n" +
            "Game logic ticks at 30 Hz internally; higher caps just refresh\n" +
            "the screen more often (smoother input + camera sampling).";
        Set(fpsLabel,  fpsTip);
        Set(comboFps,  fpsTip);

        const string filteringTip =
            "Off = crisp PSX pixels, no smoothing.\n" +
            "Dithering = recreates the PSX 24→15-bit dither pattern (recommended).\n" +
            "Bilinear = blurs textures; can hide pixel-art detail.";
        Set(filteringLabel, filteringTip);
        Set(comboFiltering, filteringTip);

        const string menuFilterTip =
            "Also apply bilinear filtering to menus and 2D screens (title, main menu,\n" +
            "save/load, options). Off by default, and independent of the in-game Filtering\n" +
            "setting above. Note: this smooths menu text as well as the artwork.";
        Set(lblMenu, menuFilterTip);
        Set(checkBox1, menuFilterTip);

        const string pgxpTip =
            "On: sub-pixel-precision vertices and perspective-correct textures\n" +
            "(reduced PSX vertex jitter and texture warping).\n" +
            "Off: authentic PSX look (affine textures, vertex snapping).\n" +
            "Press F1 in-game to toggle on the fly.";
        Set(pgxpLabel,  pgxpTip);
        Set(pgxpYes,    pgxpTip);
        Set(pgxpNo,     pgxpTip);

        const string skipIntrosTip =
            "How much of the boot sequence to skip.\n" +
            "Don't Skip = warning screen, logos and intro movie, as the\n" +
            "             original game does.\n" +
            "Skip to Menu = straight to the title screen.\n" +
            "Skip to Game = straight into gameplay: starts a New Game on\n" +
            "             NORMAL at the map set on the Advanced page, and\n" +
            "             skips the opening movie.";
        Set(skipIntrosLabel,  skipIntrosTip);
        Set(comboSkipIntros,  skipIntrosTip);

        const string decalsTip =
            "Leaves a bullet hole on walls and other world geometry where your\n" +
            "gunfire lands. Not in the original game, so it is off by default.\n" +
            "Also on the Controls page of the in-game PC Options menu.";
        Set(decalsLabel,      decalsTip);
        Set(radioDecalsYes,   decalsTip);
        Set(radioDecalsNo,    decalsTip);

        const string renderTip =
            "Which graphics API the game renders through.\n" +
            "OpenGL (native) = the default and the most tested path.\n" +
            "The rest go through ANGLE, which translates to another API:\n" +
            "Direct3D 11 and Vulkan are worth trying if your OpenGL driver\n" +
            "misbehaves (integrated graphics especially). WARP and Software\n" +
            "render on the CPU — very slow, but they run without a working\n" +
            "GPU driver at all.\n" +
            "ANGLE needs libEGL.dll and libGLESv2.dll next to the exe; if they\n" +
            "are missing the game falls back to native OpenGL and says so in\n" +
            "the log.";
        Set(lblRender,        renderTip);
        Set(comboRender,      renderTip);

        const string shadowTip =
            "Flashlight shadow map size. Higher is sharper and costs more\n" +
            "GPU time. Only used by the shadow flashlight modes.\n" +
            "Console: shadowres <size>";
        Set(lblShadow,        shadowTip);
        Set(comboShadow,      shadowTip);

        const string minimapTip =
            "On-screen minimap: shape and which corner it sits in.\n" +
            "Size and opacity stay in the in-game options.\n" +
            "Console: minimap";
        Set(lblMinimap,       minimapTip);
        Set(comboMinimap,     minimapTip);

        const string preloadTip =
            "Preload all map chunks at level start instead of streaming\n" +
            "them in as you walk. Eliminates pop-in but uses more memory\n" +
            "and lengthens the initial load. Yes is recommended; No =\n" +
            "original PSX streaming.";
        Set(chunksLabel,      preloadTip);
        Set(radioPreloadYes,  preloadTip);
        Set(radioPreloadNo,   preloadTip);

        const string pillarboxTip =
            "How 4:3 black bars (pillarboxes) are applied on a widescreen\n" +
            "(16:9 / wider) display. No effect on a 4:3 window.\n" +
            "  Menus Only (default): 2D menus / save-load / load screen are\n" +
            "  pillarboxed; 3D gameplay fills the screen (widescreen Hor+).\n" +
            "  Yes: pillarbox everything - 2D menus AND 3D gameplay get bars.\n" +
            "  No: no bars anywhere - 3D gameplay is Hor+ and menus stretch.";
        Set(refreshLabel,        pillarboxTip);
        Set(comboPillarbox,      pillarboxTip);

        const string discTip =
            "Which disc image file to boot (discs are identified by their\n" +
            "ISO boot serial, filenames don't matter). Auto = USA, then PAL,\n" +
            "then NTSC-J; picking a file boots exactly that image.\n" +
            "[modified] marks fan translations / patched discs — their voice\n" +
            "dub, story and item text are read straight off the disc. For a\n" +
            "Spanish (or other) fan translation, also pick the matching\n" +
            "Language in the title screen's options so the menus follow\n" +
            "(or set `language = es` in config.cfg). PAL carries\n" +
            "EN/DE/FR/ES/IT text; NTSC-J plays with English story text\n" +
            "replaced by Japanese (Rev 1/2 discs).";
        Set(regionLabel, discTip);
        Set(comboDisc,   discTip);
        Set(lblDisc,     discTip);

        const string levelTip =
            "Which map to load when you start a New Game. Default map0_s00\n" +
            "is the intro alley. Useful for jumping straight to a specific\n" +
            "scene during testing.";
        Set(label1,   levelTip);
        Set(comboMap, levelTip);

        const string audioTip =
            "Speaker layout. Auto detects your Windows sound setup\n" +
            "(a 5.1 system gets surround automatically). With rear\n" +
            "speakers active, monster cries / doors / world sounds pan\n" +
            "all around you and diffuse ambience layers play from the\n" +
            "surrounds. HRTF = binaural 3D for headphones.\n" +
            "Also live-switchable in-game: console AUDIOOUT.";
        Set(audioLabel,    audioTip);
        Set(comboAudioOut, audioTip);

        Set(chkRandomizer,
            "Randomizer gamemode.\n\n" +
            "New Game always opens in the police station (map2_s04). Every door\n" +
            "then leads somewhere random — another area, another room, a miniboss,\n" +
            "or nothing at all if it rolls locked. The door you came in through\n" +
            "stays shut for 10 seconds.\n\n" +
            "Monsters and item pickups are rerolled every time you enter an area.\n" +
            "After 10 areas (or a 1% chance on any door) you land at the final\n" +
            "boss, and your score picks which of the four endings you get.\n\n" +
            "Takes over the Level dropdown, and forces the global chara pool on.");

        const string uncensoredTip =
            "Censor the school child enemies on a PAL/European disc.\n\n" +
            "PAL is the only version that censored them into \"Mumblers\"; the\n" +
            "original Grey Child model ships on the disc unused.\n\n" +
            "Checked (default) = Mumblers, matching the retail PAL disc.\n" +
            "Unchecked = restore the Grey Children, matching the US/NTSC content\n" +
            "— so a PAL disc (which also carries every language) plays like the\n" +
            "uncensored US version. No effect on USA / NTSC-J discs.";
        Set(lblUncensored,  uncensoredTip);
        Set(chkUncensored,  uncensoredTip);

        const string loggingTip =
            "Write SH_DBG output to SilentHill.log next to the executable.\n" +
            "Required for diagnosing crashes/regressions; small disk-write\n" +
            "overhead. Leave set to Yes if you might report a bug.";
        Set(loggingLabel,  loggingTip);
        Set(loggingYes,    loggingTip);
        Set(loggingNo,     loggingTip);

        const string consoleTip =
            "Yes = open a separate external console window that mirrors\n" +
            "SH_DBG_ECHO output live. The in-game console can be toggled by\n" +
            "holding ~ at any time if debug controls are enabled, so this is\n" +
            "optional.";
        Set(consoleLabel,  consoleTip);
        Set(consoleYes,    consoleTip);
        Set(consoleNo,     consoleTip);

        const string aaTip =
            "Antialiasing (MSAA) smooths jagged polygon edges in the 3D world.\n" +
            "Higher = smoother but more GPU cost; textures, dither and 2D UI\n" +
            "stay sharp. Falls back to Off automatically if the GPU can't\n" +
            "provide it.";
        Set(aaLabel,   aaTip);
        Set(comboAA,   aaTip);

        const string postTip =
            "Full-screen post-process look applied to the final image\n" +
            "(CRT, scanlines, vignette, color grade, film grain, sharpen,\n" +
            "PSX downsample, cinematic). Press F2 in-game to cycle it live.";
        Set(postLabel, postTip);
        Set(comboPost, postTip);

        const string toneTip =
            "Tone mapping operator on the final image (Reinhard, ACES, Filmic).\n" +
            "Softens highlights for a more filmic look. Press F3 in-game to cycle.";
        Set(toneLabel, toneTip);
        Set(comboTone, toneTip);

        const string flashTip =
            "Flashlight rendering.\n" +
            "Classic: the original PSX flashlight (per-vertex lighting - each\n" +
            "  polygon corner is lit and the light is blended across the surface).\n" +
            "Classic + Shadows: a per-pixel (fragment-shader) cone calibrated to\n" +
            "  match the original's beam shape, brightness, room color and falloff\n" +
            "  exactly - the classic look plus real-time shadows (monsters/props\n" +
            "  cast dynamic shadows in the beam).\n" +
            "Modern: a stylized per-pixel spotlight - hard dark surround, per-\n" +
            "  surface shading and its own warm beam, like a modern horror game.\n" +
            "Modern + Shadows: the Modern spotlight with real-time shadows.\n" +
            "Per-pixel modes compute the light at every pixel instead of per\n" +
            "  polygon corner. Cycle in-game with F4; beam brightness/size are\n" +
            "  tunable with [ and ] or the flint/flsize console commands.";
        Set(flashLabel, flashTip);
        Set(comboFlash, flashTip);

        Set(btnManager, "Manage texture packs, load-folder mods, and FMV mods: enable/disable and set load order.");

        Set(btnPlay, "Save current settings to config.cfg and launch SilentHillPC.exe.");

        // RA's tip reflects live sign-in state, so it gets its own refreshable pass.
        RefreshRaTooltip();
        Set(btnChangelog, "Shows the LOCAL copy of CHANGELOG.md that's currently installed. (Build Settings and the update prompt preview other builds' changelogs.)");
        Set(btnControls, "Customize keyboard and controller bindings, and toggle debug/cheat keys.");
        Set(btnUpdate, "Check the selected branch for a build newer than any you've installed, and offer to update + switch to the latest.");
        Set(btnBuildSettings, "Choose the repo, branch, and specific build the launcher tracks for updates.");
        Set(downloadBuild, "Download (or re-download) the exact build selected in Build Settings, replacing your game files with it.");
    }

    /// <summary>
    /// Canonical map descriptions (per the upstream decomp README). These
    /// are the primary source for the dropdown, so the launcher shows
    /// correct names even against an old/minimal config.cfg. Keep in sync
    /// with the `# Available maps` comment block in pc_port/config.cfg.
    /// </summary>
    private static readonly Dictionary<string, string> BuiltinMapDescriptions = new Dictionary<string, string>
    {
        { "map0_s00", "Old Silent Hill - intro sequence" },
        { "map0_s01", "Old Silent Hill - cafe" },
        { "map0_s02", "Old Silent Hill - bonus unlockable areas" },
        { "map1_s00", "School - 1F, courtyard, basement" },
        { "map1_s01", "School - 2F" },
        { "map1_s02", "School Otherworld - 1F and courtyard" },
        { "map1_s03", "School Otherworld - 2F and roof" },
        { "map1_s04", "Unused" },
        { "map1_s05", "School - boss fight (Split Head)" },
        { "map1_s06", "School - 1F and basement after the boss" },
        { "map2_s00", "Old Silent Hill - streets" },
        { "map2_s01", "Church" },
        { "map2_s02", "Central Silent Hill - streets" },
        { "map2_s03", "Unused" },
        { "map2_s04", "Police station (Central Silent Hill)" },
        { "map3_s00", "Hospital - until Kaufmann meeting" },
        { "map3_s01", "Hospital - 1F and basement after Kaufmann" },
        { "map3_s02", "Hospital - antique shop cutscene" },
        { "map3_s03", "Hospital Otherworld - 3F and 2F" },
        { "map3_s04", "Hospital Otherworld - 1F" },
        { "map3_s05", "Hospital Otherworld - basement" },
        { "map3_s06", "Hospital - 1F after Otherworld" },
        { "map4_s00", "Unused" },
        { "map4_s01", "Green Lion Antiques (normal + Otherworld)" },
        { "map4_s02", "Central Silent Hill Otherworld - streets" },
        { "map4_s03", "Mall and boss fight" },
        { "map4_s04", "Hospital - 1F (Lisa cutscene)" },
        { "map4_s05", "Central SH Otherworld - Floatstinger boss" },
        { "map4_s06", "Unused" },
        { "map5_s00", "Sewers - lower and upper levels" },
        { "map5_s01", "Resort Area" },
        { "map5_s02", "Annie's Bar and Indian Runner (Resort Area)" },
        { "map5_s03", "Norman's Motel (Resort Area)" },
        { "map6_s00", "Resort Area Otherworld" },
        { "map6_s01", "Boat at Lakeside Pier" },
        { "map6_s02", "Lakeside Pier and Lighthouse" },
        { "map6_s03", "Sewer to Lakeside Amusement Park" },
        { "map6_s04", "Amusement Park - Cybil boss, Alessa kidnapping" },
        { "map6_s05", "Unused" },
        { "map7_s00", "Nowhere - hospital 1F, Lisa cutscene" },
        { "map7_s01", "Nowhere" },
        { "map7_s02", "Nowhere - Alessa vs. Dahlia cutscene" },
        { "map7_s03", "Nowhere - final boss" },
    };

    /// <summary>
    /// Map id → description for the dropdown. Built-in canonical names win;
    /// `# mapX_sY  Description` comment lines from config.cfg only fill ids
    /// the built-in table doesn't know (future/renamed maps).
    /// </summary>
    private Dictionary<string, string> LoadMapDescriptions(string cfgPath)
    {
        var result = new Dictionary<string, string>(BuiltinMapDescriptions);
        if (!File.Exists(cfgPath)) return result;

        var rx = new Regex(@"^#\s+(map\d+_s\d+)\s+(.+?)\s*$");
        try
        {
            foreach (var line in File.ReadAllLines(cfgPath))
            {
                var m = rx.Match(line);
                if (m.Success && !result.ContainsKey(m.Groups[1].Value))
                    result[m.Groups[1].Value] = m.Groups[2].Value.Trim();
            }
        }
        catch { /* best-effort; unknown ids just fall back to plain id */ }
        return result;
    }

    /// <summary>
    /// If config.cfg is missing, regenerate it from the embedded copy of
    /// pc_port/config.cfg (linked into the project as an EmbeddedResource,
    /// so it is always the current detailed template with full comments).
    /// </summary>
    private void EnsureConfigExists(string cfgPath)
    {
        if (File.Exists(cfgPath)) return;

        try
        {
            var asm = System.Reflection.Assembly.GetExecutingAssembly();
            using (var s = asm.GetManifestResourceStream("SilentHillPC_Launcher.DefaultConfig.cfg"))
            {
                if (s == null) return; // resource missing: ConfigManager defaults still apply
                using (var f = File.Create(cfgPath))
                {
                    s.CopyTo(f);
                }
            }
        }
        catch { /* read-only dir etc.; launcher still works off in-memory defaults */ }
    }

    private void PopulateDisplayOptions()
    {
        /* Renderer backends. Filled HERE because the constructor runs
         * PopulateDisplayOptions() before LoadConfig(), and LoadConfig sets
         * comboRender.SelectedIndex — an empty combo makes that throw
         * ArgumentOutOfRangeException and the launcher never opens. It first
         * lived in LocalizeValueLists(), which runs after LoadConfig, and did
         * exactly that.
         *
         * Display text is paired with the config value by INDEX through
         * RendererValues, so what lands in config.cfg never depends on the
         * label: the labels are PsyX_Backend's descriptions, the values are
         * what PsyX_Backend_FromName parses. */
        comboRender.Items.Clear();
        comboRender.Items.AddRange(new object[] {
            "OpenGL (native)", "OpenGL ES 3.0", "Direct3D 11 (ANGLE)",
            "Vulkan (ANGLE)", "Direct3D 11 WARP (CPU)", "Software (SwiftShader)" });

        /* Same index-pairing rule as the renderer: the visible text is localized,
         * the config value comes from the parallel array, so a translation can
         * never change what is written to config.cfg. */
        comboShadow.Items.Clear();
        comboShadow.Items.AddRange(new object[] {
            "256 x 256", "512 x 512", "1024 x 1024", "2048 x 2048", "4096 x 4096" });

        comboMinimap.Items.Clear();
        comboMinimap.Items.AddRange(new object[] {
            Loc.T("Off"),
            Loc.T("Circle + Top Left"),     Loc.T("Circle + Top Right"),
            Loc.T("Circle + Bottom Left"),  Loc.T("Circle + Bottom Right"),
            Loc.T("Square + Top Left"),     Loc.T("Square + Top Right"),
            Loc.T("Square + Bottom Left"),  Loc.T("Square + Bottom Right") });

        var modes = DisplayModes.GetModes();

        // Unique resolutions, LARGEST FIRST. A HashSet enumerates in whatever
        // order it likes, so this list used to arrive scrambled -- with ~40
        // entries that is enough on its own for someone to conclude their
        // resolution isn't there.
        var seen = new HashSet<string>();
        var resolutions = new List<(int w, int h)>();
        foreach (var m in modes)
        {
            if (seen.Add($"{m.width}x{m.height}"))
                resolutions.Add((m.width, m.height));
        }
        resolutions.Sort((a, b) => a.w != b.w ? b.w.CompareTo(a.w) : b.h.CompareTo(a.h));

        foreach (var r in resolutions)
            comboResolution.Items.Add($"{r.w}x{r.h}");

        // Unique refresh rates, highest first.
        var refreshRates = new List<int>();
        foreach (var m in modes)
        {
            if (m.hz > 0 && !refreshRates.Contains(m.hz))
                refreshRates.Add(m.hz);
        }
        refreshRates.Sort((a, b) => b.CompareTo(a));

        foreach (var hz in refreshRates)
            comboRefresh.Items.Add(hz.ToString());
    }


    private void LoadConfig()
    {
        string cfgPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.cfg");
        EnsureConfigExists(cfgPath);
        config = new ConfigManager(cfgPath);

        // Level select. Cleared first: LoadConfig runs from both the
        // constructor and Form1_Load — without it every entry duplicated.
        // An unknown hand-set map id (test maps like mapt_s00) gets its own
        // raw entry so Play doesn't silently rewrite it to map0_s00.
        comboMap.Items.Clear();
        foreach (var m in s_mapEntries)
            comboMap.Items.Add(m);
        string savedMap = config.Get("map", "map0_s00");
        int mapIdx = -1;
        for (int i = 0; i < comboMap.Items.Count; i++)
        {
            string id = comboMap.Items[i].ToString().Split(new[] { "  -  " }, StringSplitOptions.None)[0];
            if (string.Equals(id, savedMap, StringComparison.OrdinalIgnoreCase)) { mapIdx = i; break; }
        }
        if (mapIdx < 0 && savedMap.Length > 0)
        {
            comboMap.Items.Add(savedMap);
            mapIdx = comboMap.Items.Count - 1;
        }
        comboMap.SelectedIndex = mapIdx >= 0 ? mapIdx : 0;

        // Randomizer gamemode. It always starts in map2_s04, so it owns the Level
        // row: the dropdown greys out and reads "Randomizer Enabled" while it's on.
        // Must run after the Items.Clear() above, which drops the synthetic entry.
        chkRandomizer.Checked = config.Get("randomizer", "0") == "1";
        ApplyRandomizerUi();

        // The box reads as "Censored" (🚫): CHECKED = censored (retail PAL
        // Mumblers), UNCHECKED = uncensored (Grey Children). The config key is
        // still `uncensored`, so the mapping is inverted here. Default 0
        // (censored) → checked, so the box defaults on.
        chkUncensored.Checked = config.Get("uncensored", "0") == "0";

        // fullscreen
        // fullscreen: 0 = windowed, 1 = exclusive fullscreen, 2 = borderless.
        // Dropdown order: Fullscreen(0), Windowed(1), Borderless(2).
        switch (config.Get("fullscreen", "0"))
        {
            case "1":  comboFullscreen.SelectedIndex = 0; break;
            case "2":  comboFullscreen.SelectedIndex = 2; break;
            default:   comboFullscreen.SelectedIndex = 1; break;
        }

        // vsync
        radioVsyncYes.Checked = config.Get("vsync", "0") == "1";
        radioVsyncNo.Checked = config.Get("vsync", "0") == "0";

        // enable debug logging (writes SH_DBG output to SilentHill.log)
        loggingYes.Checked = config.Get("enable_debug_log", "0") == "1";
        loggingNo.Checked = !loggingYes.Checked;

        // External console: Yes = external window (show_console=1). The in-game
        // overlay is toggleable with ~, so the legacy ingame/both modes (2/3)
        // also count as "external on" here; SaveConfig collapses to 1/0.
        int consoleMode;
        if (!int.TryParse(config.Get("show_console", "0"), out consoleMode) || consoleMode < 0 || consoleMode > 3)
            consoleMode = 0;
        consoleYes.Checked = (consoleMode == 1 || consoleMode == 3);
        consoleNo.Checked = !consoleYes.Checked;

        // PGXP — sub-pixel-precision GTE coords + perspective-correct textures.
        // WORK IN PROGRESS — defaults off until prim emit sites are migrated.
        pgxpYes.Checked = config.Get("use_pgxp", "0") == "1";
        pgxpNo.Checked = !pgxpYes.Checked;

        comboFps.SelectedItem = config.Get("fps_cap", "30");
        string fps = config.Get("fps_cap", "30");
        if (comboFps.Items.Contains(fps))
            comboFps.SelectedItem = fps;
        else
            comboFps.SelectedItem = "30";

        // Filtering: int in config (0/1/2) <-> dropdown index
        // 0 = Off, 1 = Dithering, 2 = Bilinear
        int filterIdx;
        if (!int.TryParse(config.Get("psx_dither", "1"), out filterIdx))
            filterIdx = 1; // default to dithering
        if (filterIdx < 0 || filterIdx > 2) filterIdx = 1;
        comboFiltering.SelectedIndex = filterIdx;

        // "Menus:" checkbox — also bilinear-filter menu/2D screens (config key menu_filter)
        checkBox1.Checked = config.Get("menu_filter", "0") == "1";

        // Antialiasing (MSAA): config msaa value 0/2/4/8 <-> dropdown index 0..3
        switch (config.Get("msaa", "0"))
        {
            case "2": comboAA.SelectedIndex = 1; break;
            case "4": comboAA.SelectedIndex = 2; break;
            case "8": comboAA.SelectedIndex = 3; break;
            default:  comboAA.SelectedIndex = 0; break;
        }

        // Post-process look: config post_process int 0..N <-> dropdown index
        int postIdx;
        if (!int.TryParse(config.Get("post_process", "0"), out postIdx) ||
            postIdx < 0 || postIdx >= comboPost.Items.Count)
            postIdx = 0;
        comboPost.SelectedIndex = postIdx;

        // Tone mapping: config tonemap int 0..3 <-> dropdown index
        int toneIdx;
        if (!int.TryParse(config.Get("tonemap", "0"), out toneIdx) ||
            toneIdx < 0 || toneIdx >= comboTone.Items.Count)
            toneIdx = 0;
        comboTone.SelectedIndex = toneIdx;

        // Flashlight: 0 = Classic, 1 = Classic + Shadows, 2 = Modern,
        // 3 = Modern + Shadows. flashlight_mode is authoritative; configs from
        // older builds only carry the legacy pp/shadows keys (pp+shadows was
        // the pre-calibration per-pixel look, so it maps to Modern + Shadows).
        {
            string fm = config.Get("flashlight_mode", "");
            int fmIdx;
            if (!int.TryParse(fm, out fmIdx) || fmIdx < 0 || fmIdx > 3)
            {
                if (config.Get("per_pixel_flashlight", "0") != "1")
                    fmIdx = 0;
                else
                    fmIdx = config.Get("flashlight_shadows", "1") == "1" ? 3 : 2;
            }
            comboFlash.SelectedIndex = fmIdx;
        }

        // Audio output (speaker layout): auto = OpenAL detects the system
        // layout. With rears active the game pans SFX on the full circle and
        // sends wide-stereo BGM layers to the surrounds.
        {
            string ao = config.Get("audio_output", "auto");
            int aoIdx = Array.IndexOf(AudioOutputIds, ao);
            comboAudioOut.SelectedIndex = aoIdx >= 0 ? aoIdx : 0;
        }

        // resolution
        string w = config.Get("width", "640");
        string h = config.Get("height", "480");
        comboResolution.SelectedItem = $"{w}x{h}";

        // refresh rate is config-only now (auto-detected by default); keep the
        // hidden combo populated harmlessly so its references stay valid.
        comboRefresh.SelectedItem = config.Get("refresh_rate", "0");

        // pillarboxing: combine menu_pillarbox + widescreen_mode into one 3-way choice.
        bool _menuPb = config.Get("menu_pillarbox", "1") == "1";
        bool _wide3d = config.Get("widescreen_mode", "1") != "0"; // 0 = pillarbox 3D
        if (!_menuPb)      comboPillarbox.SelectedIndex = 1; // No
        else if (!_wide3d) comboPillarbox.SelectedIndex = 0; // Yes (3D pillarboxed)
        else               comboPillarbox.SelectedIndex = 2; // Menus Only (default)

        // Region dropdown is populated from the detected discs in
        // CheckDiscImage (runs on Shown), not from config.

        // skip_intros is a level: 0 = don't skip, 1 = to the menu, 2 = into gameplay
        int skipIntrosLevel;
        if (!int.TryParse(config.Get("skip_intros", "0"), out skipIntrosLevel)) skipIntrosLevel = 0;
        if (skipIntrosLevel < 0) skipIntrosLevel = 0;
        if (skipIntrosLevel > 2) skipIntrosLevel = 2;
        comboSkipIntros.SelectedIndex = skipIntrosLevel;

        // preload_chunks (recommended: Yes — matches engine default)
        // bullet_decals — off by default, matching the engine default.
        radioDecalsYes.Checked = config.Get("bullet_decals", "0") == "1";
        radioDecalsNo.Checked = !radioDecalsYes.Checked;
        {
            /* Never assign SelectedIndex without checking the item count. A
             * combo that is empty (or shorter than expected) throws, and a
             * throw in LoadConfig happens in the constructor, so the launcher
             * dies before it draws a window — no UI, no log, nothing the user
             * can act on. Clamping costs nothing and the worst case is a
             * dropdown showing the wrong entry. */
            int ri = Array.IndexOf(RendererValues, config.Get("renderer", "gl").Trim().ToLowerInvariant());
            /* Unknown, or the retired "auto" — both mean OpenGL. GR_ResolveBackend
             * maps PSYX_BACKEND_AUTO straight to PSYX_BACKEND_GL with no probing
             * of any kind, so "Automatic" was a second name for the entry below
             * it and is no longer offered. A config that still says auto lands
             * here and shows what it actually gets. */
            if (ri < 0) ri = 0;
            if (ri >= comboRender.Items.Count) ri = 0;
            comboRender.SelectedIndex = comboRender.Items.Count > 0 ? ri : -1;
        }
        {
            int si = Array.IndexOf(ShadowResValues, config.Get("shadow_resolution", "1024").Trim());
            if (si < 0 || si >= comboShadow.Items.Count) si = 2; /* 1024 */
            comboShadow.SelectedIndex = comboShadow.Items.Count > 0 ? si : -1;
        }
        {
            /* One dropdown over two keys: `minimap` is 0 off / 1 square / 2 circle,
             * `minimap_corner` is 0..3. Off collapses both. */
            int shape  = ParseIntOr(config.Get("minimap", "0"), 0);
            int corner = ParseIntOr(config.Get("minimap_corner", "0"), 0);
            if (corner < 0 || corner > 3) corner = 0;

            int mi;
            if (shape == 2)      mi = 1 + corner;   /* circle */
            else if (shape == 1) mi = 5 + corner;   /* square */
            else                 mi = 0;            /* off */

            if (mi >= comboMinimap.Items.Count) mi = 0;
            comboMinimap.SelectedIndex = comboMinimap.Items.Count > 0 ? mi : -1;
        }
        radioPreloadYes.Checked = config.Get("preload_chunks", "1") == "1";
        radioPreloadNo.Checked = !radioPreloadYes.Checked;

    }

    private void SaveConfig()
    {
        config.Set("fullscreen",
            comboFullscreen.SelectedIndex == 0 ? "1" :   // Fullscreen
            comboFullscreen.SelectedIndex == 2 ? "2" :   // Borderless
            "0");                                        // Windowed
        config.Set("vsync", radioVsyncYes.Checked ? "1" : "0");
        // resolution
        if (comboResolution.SelectedItem != null)
        {
            var parts = comboResolution.SelectedItem.ToString().Split('x');
            config.Set("width", parts[0]);
            config.Set("height", parts[1]);
        }

        // refresh rate is config-only now (auto-detected by default) — do NOT
        // write it from the launcher so a hand-set value is preserved.

        // pillarboxing: 3-way dropdown -> menu_pillarbox + widescreen_mode
        switch (comboPillarbox.SelectedIndex)
        {
            case 0: config.Set("menu_pillarbox", "1"); config.Set("widescreen_mode", "0"); break; // Yes
            case 1: config.Set("menu_pillarbox", "0"); config.Set("widescreen_mode", "1"); break; // No
            default: config.Set("menu_pillarbox", "1"); config.Set("widescreen_mode", "1"); break; // Menus Only
        }

        /* skip_intros. disable_culling is deliberately no longer surfaced here — it stays
         * game-side (pc_config.c) and in-game-toggleable, and ConfigManager.Save() rewrites
         * only keys it holds, so a user's existing value is preserved untouched. */
        config.Set("skip_intros", comboSkipIntros.SelectedIndex.ToString());

        // preload_chunks
        config.Set("bullet_decals", radioDecalsYes.Checked ? "1" : "0");
        config.Set("renderer", RendererValues[comboRender.SelectedIndex >= 0 ? comboRender.SelectedIndex : 1]);
        config.Set("shadow_resolution", ShadowResValues[comboShadow.SelectedIndex >= 0 ? comboShadow.SelectedIndex : 2]);
        {
            int mi = comboMinimap.SelectedIndex;
            if (mi < 0) mi = 0;
            config.Set("minimap",        mi == 0 ? "0" : (mi <= 4 ? "2" : "1"));
            config.Set("minimap_corner", mi == 0 ? "0" : ((mi <= 4 ? mi - 1 : mi - 5)).ToString());
        }
        config.Set("preload_chunks", radioPreloadYes.Checked ? "1" : "0");

        // enable debug logging
        config.Set("enable_debug_log", loggingYes.Checked ? "1" : "0");

        // External console: Yes = external window (1), No = off (0). The in-game
        // console stays togglable with ~ regardless of this setting.
        config.Set("show_console", consoleYes.Checked ? "1" : "0");

        // PGXP toggle (work-in-progress)
        config.Set("use_pgxp", pgxpYes.Checked ? "1" : "0");

        if (comboFps.SelectedItem != null)
            config.Set("fps_cap", comboFps.SelectedItem.ToString());

        // Filtering: dropdown index (0=Off, 1=Dithering, 2=Bilinear) -> int
        if (comboFiltering.SelectedIndex >= 0)
            config.Set("psx_dither", comboFiltering.SelectedIndex.ToString());
        config.Set("menu_filter", checkBox1.Checked ? "1" : "0");

        // Antialiasing (MSAA): dropdown index 0..3 -> msaa 0/2/4/8
        switch (comboAA.SelectedIndex)
        {
            case 1:  config.Set("msaa", "2"); break;
            case 2:  config.Set("msaa", "4"); break;
            case 3:  config.Set("msaa", "8"); break;
            default: config.Set("msaa", "0"); break;
        }

        // Post-process look: dropdown index -> post_process int
        if (comboPost.SelectedIndex >= 0)
            config.Set("post_process", comboPost.SelectedIndex.ToString());

        // Tone mapping: dropdown index -> tonemap int
        if (comboTone.SelectedIndex >= 0)
            config.Set("tonemap", comboTone.SelectedIndex.ToString());

        // Flashlight dropdown -> flashlight_mode (authoritative) + the legacy
        // keys for older builds. 0 = Classic, 1 = Classic + Shadows, 2 = Modern,
        // 3 = Modern + Shadows.
        {
            int fmIdx = comboFlash.SelectedIndex >= 0 ? comboFlash.SelectedIndex : 0;
            config.Set("flashlight_mode", fmIdx.ToString());
            config.Set("per_pixel_flashlight", fmIdx >= 1 ? "1" : "0");
            config.Set("flashlight_shadows", (fmIdx == 1 || fmIdx == 3) ? "1" : "0");
        }

        // Audio output dropdown -> audio_output string id.
        if (comboAudioOut.SelectedIndex >= 0)
            config.Set("audio_output", AudioOutputIds[comboAudioOut.SelectedIndex]);

        // Disc: exact filename (fan-translated / modified images), "" = auto.
        // Untouched when no disc was detected so a hand-set value survives.
        // (The `region` key is no longer UI-managed; a hand-set value keeps
        // steering the Auto pick.)
        if (comboDisc.SelectedIndex >= 0 && comboDisc.SelectedIndex < _discIds.Count)
            config.Set("disc_image", _discIds[comboDisc.SelectedIndex]);

        config.Set("randomizer", chkRandomizer.Checked ? "1" : "0");
        config.Set("uncensored", chkUncensored.Checked ? "0" : "1"); // checked = censored

        // Level: persist only the map id, not the " - description" suffix. Skipped
        // while the randomizer owns the row — the selection is the synthetic
        // "Randomizer Enabled" entry, and the real map id must survive so that
        // unchecking the box restores it.
        if (!chkRandomizer.Checked && comboMap.SelectedItem != null)
        {
            string sel = comboMap.SelectedItem.ToString();
            config.Set("map", sel.Split(new[] { "  -  " }, StringSplitOptions.None)[0]);
        }

        config.Save();
    }

    // Dropdown index -> audio_output config id (matches the game's parser).
    private static readonly string[] AudioOutputIds = { "auto", "stereo", "quad", "51", "71", "hrtf" };

    private const string RandomizerMapEntry = "Randomizer Enabled";

    // The map id the Level dropdown had before the randomizer took the row over,
    // so unchecking puts it back.
    private string _mapBeforeRandomizer;

    private void ApplyRandomizerUi()
    {
        if (chkRandomizer.Checked)
        {
            if (comboMap.Enabled)
                _mapBeforeRandomizer = comboMap.SelectedItem?.ToString();

            int idx = comboMap.Items.IndexOf(RandomizerMapEntry);
            if (idx < 0)
                idx = comboMap.Items.Add(RandomizerMapEntry);
            comboMap.SelectedIndex = idx;
            comboMap.Enabled = false;
        }
        else
        {
            comboMap.Enabled = true;

            int idx = comboMap.Items.IndexOf(RandomizerMapEntry);
            if (idx >= 0)
            {
                comboMap.Items.RemoveAt(idx);

                int restore = _mapBeforeRandomizer != null
                            ? comboMap.Items.IndexOf(_mapBeforeRandomizer)
                            : -1;
                comboMap.SelectedIndex = restore >= 0 ? restore
                                       : (comboMap.Items.Count > 0 ? 0 : -1);
            }
        }
    }

    private void chkRandomizer_CheckedChanged(object sender, EventArgs e)
    {
        ApplyRandomizerUi();
    }

    // Win32 focus helpers. Windows' focus-stealing-prevention blocks a newly-
    // spawned process from taking the foreground unless the parent grants
    // permission via AllowSetForegroundWindow OR the parent explicitly calls
    // SetForegroundWindow on the child window once it exists. Use both: the
    // permission grant covers the case where the game brings itself to front
    // during SDL init, and the explicit call handles SDL builds that don't.
    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool AllowSetForegroundWindow(uint dwProcessId);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    /// <summary>
    /// Warn once per launch when the config carries `resident_textures = 0` and
    /// offer to turn it on. Runs AFTER SaveConfig so the value read here is the
    /// one the game is about to load, including a hand edit made while the
    /// launcher was open (ConfigManager.Save re-reads, and this key is never
    /// Set() by the launcher, so Get reports what is really on disk).
    ///
    /// Only ever prompts; the fallback is legitimate on some hardware, so the
    /// launcher must not overrule a deliberate choice. `launcher_warn_resident_textures = 0`
    /// suppresses it permanently.
    /// </summary>
    private void WarnIfResidentTexturesOff()
    {
        if (config == null) return;
        if (config.Get("resident_textures", "1").Trim() != "0") return;
        if (config.Get("launcher_warn_resident_textures", "1").Trim() == "0") return;

        using (var dlg = new SilentHillPC_Launcher.ResidentTexturesDialog())
        {
            dlg.ShowDialog(this);
            switch (dlg.Result)
            {
                case SilentHillPC_Launcher.ResidentTexturesDialog.Choice.TurnOn:
                    config.Set("resident_textures", "1");
                    config.Save();
                    break;
                case SilentHillPC_Launcher.ResidentTexturesDialog.Choice.NeverAsk:
                    config.EnsureLauncherSection();
                    config.Set("launcher_warn_resident_textures", "0");
                    config.Save();
                    break;
                default:
                    break; /* leave it off, ask again next launch */
            }
        }
    }

    private void btnPlay_Click(object sender, EventArgs e)
    {
        SaveConfig();
        WarnIfResidentTexturesOff();

        string exePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SilentHillPC.exe");

        if (!File.Exists(exePath))
        {
            MessageBox.Show("SilentHillPC.exe not found.");
            return;
        }

        try
        {
            var p = Process.Start(exePath);
            if (p != null)
            {
                // Grant the child process permission to come to the foreground.
                AllowSetForegroundWindow((uint)p.Id);

                // Wait briefly for the SDL window to be created and message-
                // loop ready, then explicitly raise it. WaitForInputIdle's
                // timeout is forgiving — if the game takes longer to init, we
                // just skip the explicit call; the AllowSetForegroundWindow
                // permission still lets SDL bring it forward later.
                try { p.WaitForInputIdle(2000); } catch { }
                p.Refresh();
                if (p.MainWindowHandle != IntPtr.Zero)
                {
                    SetForegroundWindow(p.MainWindowHandle);
                }
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show("Failed to launch SilentHillPC.exe: " + ex.Message);
            return;
        }

        Close();
    }

    // Check for Updates: always checks the configured branch's LATEST build and
    // offers to update + switch to it — but only when that latest is newer than
    // the highest version ever installed from this branch, so an old build you
    // chose on purpose doesn't nag you. (Switching to a specific build is the
    // Download Build button's job.)
    private async void btnUpdate_Click(object sender, EventArgs e)
    {
        string installDir = AppDomain.CurrentDomain.BaseDirectory;
        var settings = LauncherSettings.Load(config);

        SetUpdateBusy(true);
        SetText(lblUpdateStatus, "Checking for updates...");
        progUpdate.Style   = ProgressBarStyle.Marquee;
        progUpdate.Visible = true;
        try
        {
            var plan = await UpdateChecker.CheckAsync(installDir, ForceLatest(settings));
            if (!plan.HasUpdate) settings.RecordSeenAtLeast(config, plan.RemoteVersion);

            bool available = LauncherSettings.CompareVersions(plan.RemoteVersion, settings.GetSeen(config)) > 0;
            if (!available)
            {
                SetText(btnUpdate, "Check for Updates");
                SetText(lblUpdateStatus, "Up to date ({0}).", plan.RemoteVersion);
                progUpdate.Visible = false;
                MessageBox.Show(this,
                    $"You're up to date!\n\nSource: {plan.RepoLabel}\nLatest: {plan.RemoteVersion}",
                    "Check for Updates", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            SetText(btnUpdate, "Update available!");
            lblUpdateStatus.ForeColor = Color.LightGreen;

            // Always re-promptable: clicking the button offers the update every
            // time, even if you said "no" before. (View Changelog previews the
            // incoming build's notes without downloading it.)
            bool wantUpdate = PromptUpdate("Update available",
                $"A newer build is available: {plan.RemoteVersion}\n" +
                $"Built: {plan.BuildDate}\n" +
                $"Source: {plan.RepoLabel}\n\n" +
                (plan.IsBeta ? "This build is on the BETA branch (newer than the latest alpha/stable build).\n\n" : "") +
                (settings.IsLatestBuild ? "" : $"You currently have build '{settings.Build}' selected.\n") +
                "Update to the latest build and switch to it now?",
                plan.ChangelogUrl);
            if (!wantUpdate)
            {
                SetText(lblUpdateStatus, "Update {0} skipped.", plan.RemoteVersion);
                progUpdate.Visible = false;
                return;
            }

            bool applied = await RunApplyAsync(installDir, plan, showFileConfirm: false);
            if (applied)
            {
                settings.Build = "latest";   // switch to latest
                if (!string.IsNullOrEmpty(plan.MigrateToBranch))
                    settings.Branch = plan.MigrateToBranch;   // auto-migrate alpha -> beta
                settings.Save(config);
                settings.RecordInstalled(config, plan.RemoteVersion);
                SetText(btnUpdate, "Check for Updates");
                SetText(downloadBuild, "Redownload Build"); // latest is now installed
                lblUpdateStatus.ForeColor = Color.LightGray;
                SetText(lblUpdateStatus, "Up to date ({0}).", plan.RemoteVersion);
                MessageBox.Show(this, "Update complete!", "Update", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                SetText(lblUpdateStatus, "Update {0} skipped.", plan.RemoteVersion);
            }
        }
        catch (Exception ex)
        {
            SetText(lblUpdateStatus, "Update failed (see message).");
            MessageBox.Show(this, "Update failed:\n\n" + ex.Message, "Update error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            SetUpdateBusy(false);
            progUpdate.Visible = false;
        }
    }

    // Download Build: download (or re-download) the SELECTED build and replace
    // the differing game files with it, regardless of newer/older. The launcher
    // exe is never downgraded (CheckAsync's gate handles that).
    private async void downloadBuild_Click(object sender, EventArgs e)
    {
        string installDir = AppDomain.CurrentDomain.BaseDirectory;
        var settings = LauncherSettings.Load(config);

        SetUpdateBusy(true);
        SetText(lblUpdateStatus, "Checking selected build...");
        progUpdate.Style   = ProgressBarStyle.Marquee;
        progUpdate.Visible = true;
        try
        {
            var plan = await UpdateChecker.CheckAsync(installDir, settings);

            if (!plan.HasUpdate)
            {
                progUpdate.Visible = false;
                SetText(lblUpdateStatus, "Build {0} is installed.", plan.RemoteVersion);
                MessageBox.Show(this,
                    $"Build {plan.RemoteVersion} is already installed.\n\nSource: {plan.RepoLabel}",
                    "Download Build", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            var others = plan.Changed.Where(f => !UpdateChecker.IsLauncherFile(f.Path)).ToList();
            var sb = new StringBuilder();
            sb.AppendLine($"Download build {plan.RemoteVersion}?");
            sb.AppendLine($"Built: {plan.BuildDate}");
            sb.AppendLine($"Source: {plan.RepoLabel}");
            sb.AppendLine();
            sb.AppendLine($"{others.Count} file(s) will be replaced with this build:");
            int i = 0;
            foreach (var f in others)
            {
                if (i++ < 10) sb.AppendLine($"  • {f.Path}");
                else { sb.AppendLine($"  • ... and {others.Count - 10} more"); break; }
            }
            sb.AppendLine();
            bool hasExisting = File.Exists(Path.Combine(installDir, "SilentHillPC.exe"));
            string promptTitle;
            MessageBoxIcon promptIcon;
            if (hasExisting)
            {
                sb.AppendLine("This will overwrite your existing files. Are you sure you want to continue?");
                promptTitle = "Overwrite existing files?";
                promptIcon  = MessageBoxIcon.Warning;
            }
            else
            {
                sb.AppendLine("Download and install now?");
                promptTitle = "Download Build";
                promptIcon  = MessageBoxIcon.Information;
            }
            if (MessageBox.Show(this, sb.ToString(), promptTitle,
                    MessageBoxButtons.YesNo, promptIcon) != DialogResult.Yes)
            {
                SetText(lblUpdateStatus, "Download cancelled.");
                progUpdate.Visible = false;
                return;
            }

            bool applied = await RunApplyAsync(installDir, plan, showFileConfirm: false);
            if (applied)
            {
                settings.RecordInstalled(config, plan.RemoteVersion);
                lblUpdateStatus.ForeColor = Color.LightGray;
                SetText(lblUpdateStatus, "Build {0} installed.", plan.RemoteVersion);
                MessageBox.Show(this, $"Build {plan.RemoteVersion} installed!", "Download Build",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                SilentAutoCheckForUpdates(); // refresh the update indicator
            }
            else
            {
                SetText(lblUpdateStatus, "Download cancelled.");
            }
        }
        catch (Exception ex)
        {
            SetText(lblUpdateStatus, "Download failed (see message).");
            MessageBox.Show(this, "Download failed:\n\n" + ex.Message, "Download error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            SetUpdateBusy(false);
            progUpdate.Visible = false;
        }
    }

    private void SetUpdateBusy(bool busy)
    {
        btnUpdate.Enabled     = !busy;
        downloadBuild.Enabled = !busy;
        btnPlay.Enabled       = !busy;
    }

    // Shared apply: a SEPARATE launcher self-update confirm (only when the
    // incoming launcher is strictly newer), an optional file-list confirm, then
    // the download/install with progress. Returns true if anything was installed.
    private async Task<bool> RunApplyAsync(string installDir, UpdateChecker.UpdatePlan plan, bool showFileConfirm)
    {
        var launcherEntry = plan.LauncherEntry;
        bool applyLauncher = false;
        if (launcherEntry != null && plan.LauncherIsNewer)
        {
            var lmsg =
                "Launcher has an update available, are you sure you want to update?\n\n" +
                $"New launcher version: {plan.LauncherVersion}\n" +
                $"Current version:      {LauncherSettings.OwnLauncherVersion()}\n" +
                $"Build:  {plan.RemoteVersion}  ({plan.BuildDate})\n" +
                $"Source: {plan.RepoLabel}\n\n" +
                "The launcher is replaced and the new version loads the next time you open it.";
            applyLauncher = MessageBox.Show(this, lmsg, "Launcher update",
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;
        }

        var others = plan.Changed.Where(f => !UpdateChecker.IsLauncherFile(f.Path)).ToList();
        bool applyOthers = others.Count > 0;
        if (applyOthers && showFileConfirm)
        {
            var sb = new StringBuilder();
            sb.AppendLine($"{others.Count} file(s) to {(plan.Mode == "zip" ? "install (from zip)" : "download")}:");
            int i = 0;
            foreach (var f in others)
            {
                if (i++ < 10) sb.AppendLine($"  • {f.Path}");
                else { sb.AppendLine($"  • ... and {others.Count - 10} more"); break; }
            }
            sb.AppendLine();
            sb.AppendLine("Continue?");
            applyOthers = MessageBox.Show(this, sb.ToString(), "Update",
                MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes;
        }

        var apply = new List<UpdateChecker.FileEntry>();
        if (applyLauncher && launcherEntry != null) apply.Add(launcherEntry);
        if (applyOthers) apply.AddRange(others);
        if (apply.Count == 0) return false;
        plan.Changed = apply;

        progUpdate.Style = ProgressBarStyle.Continuous;
        progUpdate.Minimum = 0;
        progUpdate.Maximum = 100;
        await UpdateChecker.ApplyAsync(installDir, plan, (frac, msg) =>
        {
            // Marshal back to UI thread — the progress callback fires on
            // whatever thread HttpClient happens to be on.
            BeginInvoke((Action)(() =>
            {
                if (frac >= 0 && frac <= 1) progUpdate.Value = (int)(frac * 100);
                lblUpdateStatus.Text = Loc.T(msg ?? "");
            }));
        });

        if (applyLauncher)
            MessageBox.Show(this,
                "The launcher itself was updated — the new version loads the next time you open it.",
                "Launcher updated", MessageBoxButtons.OK, MessageBoxIcon.Information);
        return true;
    }

    private void btnChangelog_Click(object sender, EventArgs e)
    {
        string changelogPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "CHANGELOG.md");
        string text = File.Exists(changelogPath)
            ? File.ReadAllText(changelogPath, System.Text.Encoding.UTF8)
            : "CHANGELOG.md not found next to the launcher.\n\n" +
              "Run 'Check for Updates' to download the latest build which includes the changelog.";
        ChangelogViewer.Show(this, "Silent Hill PC Port — Changelog (installed)", text);
    }

    // Update prompt with a "View Changelog" button that previews the INCOMING
    // build's notes (fetched from the release, not the local copy). Returns true
    // if the user chose to update.
    private bool PromptUpdate(string title, string message, string changelogUrl)
    {
        bool update = false;
        using (var dlg = new Form
        {
            Text            = title,
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition   = FormStartPosition.CenterParent,
            MaximizeBox     = false,
            MinimizeBox     = false,
            ShowInTaskbar   = false,
            ClientSize      = new Size(432, 195),
            BackColor       = Color.FromArgb(32, 32, 32),
            ForeColor       = Color.Gainsboro,
        })
        {
            var lbl    = new Label  { Text = message, Left = 14, Top = 14, Width = 404, Height = 112, AutoSize = false };
            var btnCl  = new Button { Text = "View Changelog", Left = 14,  Top = 150, Width = 120, Height = 28 };
            var btnYes = new Button { Text = "Update",         Left = 246, Top = 150, Width = 80,  Height = 28 };
            var btnNo  = new Button { Text = "Not now",        Left = 332, Top = 150, Width = 86,  Height = 28, DialogResult = DialogResult.Cancel };
            btnYes.Click += (s, e) => { update = true; dlg.Close(); };
            btnCl.Enabled = !string.IsNullOrWhiteSpace(changelogUrl);
            btnCl.Click += async (s, e) =>
            {
                btnCl.Enabled = false;
                try { ChangelogViewer.Show(dlg, "Changelog — incoming build", await UpdateChecker.FetchTextAsync(changelogUrl)); }
                catch (Exception ex) { MessageBox.Show(dlg, "Couldn't load the changelog:\n\n" + ex.Message, "Changelog", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
                finally { btnCl.Enabled = true; }
            };

            dlg.Controls.Add(lbl);
            dlg.Controls.Add(btnCl);
            dlg.Controls.Add(btnYes);
            dlg.Controls.Add(btnNo);
            dlg.AcceptButton = btnYes;
            dlg.CancelButton = btnNo;
            dlg.ShowDialog(this);
        }
        return update;
    }

    private void ApplyDarkMode()
    {
        Color back = Color.FromArgb(30, 30, 30);
        Color panelBack = Color.FromArgb(45, 45, 45);
        Color text = Color.White;

        this.BackColor = back;
        this.ForeColor = text;

        foreach (Control c in this.Controls)
            ApplyDarkToControl(c, back, panelBack, text);
    }

    private void ApplyDarkToControl(Control c, Color back, Color panelBack, Color text)
    {
        if (c is Panel)
            c.BackColor = panelBack;
        else if (c is ComboBox)
            c.BackColor = panelBack;
        else if (c is Button)
            c.BackColor = panelBack;
        else
            c.BackColor = back;

        c.ForeColor = text;

        // Recursively apply to children
        foreach (Control child in c.Controls)
            ApplyDarkToControl(child, back, panelBack, text);
    }


    private void banner_Click(object sender, EventArgs e)
    {
        /* Belt and braces: Control only raises Click off WM_LBUTTONUP, so the
         * right-click that opens the background menu should never reach here --
         * but About popping up behind that menu would be a nasty surprise. */
        MouseEventArgs me = e as MouseEventArgs;
        if (me != null && me.Button != MouseButtons.Left)
            return;

        string about =
    "This port is based on the decompiled Silent Hill 1 for PSX Source Code:\n\n" +
    "https://github.com/Vatuu/silent-hill-decomp\n\n" +
    "This launcher and port were created by Chris Hardin aka KushAstronaut " +
    "(kushastronaut@icloud.com), with many thanks and a lot of help from " +
    "Psycross, Claude Code, and the wonderful decompilation community.\n\n" +
    "The source code is available here:\n\n" +
    "https://github.com/SlickAmogus/silent-hill-decomp\n\n" +
    "You should never pay for this software, and you should always provide " +
    "your own legally obtained game data.\n\n" +
    "Silent Hill is copyright © KONAMI";
        MessageBox.Show(about, "About Silent Hill PC Port",
            MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    private void comboResolution_SelectedIndexChanged(object sender, EventArgs e)
    {

    }

    private void btnManager_Click(object sender, EventArgs e)
    {
        using (var f = new SilentHillPC_Launcher.ModManagerForm(config, AppDomain.CurrentDomain.BaseDirectory))
        {
            f.ShowDialog(this);
        }
    }

    private void btnManager_MouseDown(object sender, MouseEventArgs e)
    {
        if (e.Button == MouseButtons.Left)
            btnManager.BackgroundImage = SilentHillPC_Launcher.Properties.Resources.manager_clicked;
    }

    private void btnManager_MouseUp(object sender, MouseEventArgs e)
    {
        btnManager.BackgroundImage = SilentHillPC_Launcher.Properties.Resources.manager;
    }

    private void btnManager_MouseLeave(object sender, EventArgs e)
    {
        btnManager.BackgroundImage = SilentHillPC_Launcher.Properties.Resources.manager;
    }

    private void comboRefresh_SelectedIndexChanged(object sender, EventArgs e)
    {

    }

    private void Form1_Load(object sender, EventArgs e)
    {

        //ApplyDarkMode();
        LoadConfig();
        BannerInit();
    }

    // ==========================================================
    // Custom banner (right-click the banner image)
    // ==========================================================

    /* The chosen image is re-encoded to one fixed PNG beside the launcher, so its
     * presence alone is the "customised" flag -- nothing to keep in sync in the
     * config, and Reset is just a delete. Re-encoding also means we do not care
     * what container the user picked. */
    private static string BannerCustomPath =>
        Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "launcher_banner.png");

    private Image             _bannerCustom;
    private ContextMenuStrip  _bannerMenu;
    private ToolStripMenuItem _bannerMenuReset;
    private ToolStripMenuItem _bannerMenuDark;
    private bool _darkMode;
    /* Captured before the first darkening so the toggle can go back. Without it
     * "off" would have to guess at the designer's per-control colours. */
    private Dictionary<Control, Color[]> _lightColors;

    private void BannerInit()
    {
        banner.MouseUp += banner_MouseUp;
        BannerApplyCustomIfPresent();

        if (config != null && config.Get("launcher_dark_mode", "0").Trim() == "1")
            DarkModeSet(true, false);
    }

    // ==========================================================
    // Dark mode (toggled from the banner right-click menu)
    // ==========================================================

    private void DarkModeSet(bool on, bool persist = true)
    {
        if (on)
        {
            if (_lightColors == null)
            {
                _lightColors = new Dictionary<Control, Color[]>();
                DarkModeCapture(this);
                foreach (Control c in this.Controls) DarkModeCapture(c);
            }
            ApplyDarkMode();
        }
        else
        {
            DarkModeRestore();
        }

        _darkMode = on;

        if (persist && config != null)
        {
            try
            {
                config.EnsureLauncherSection();
                config.Set("launcher_dark_mode", on ? "1" : "0");
                config.Save();
            }
            catch { /* a read-only install must not break the toggle */ }
        }
    }

    private void DarkModeCapture(Control c)
    {
        if (!_lightColors.ContainsKey(c))
            _lightColors[c] = new Color[] { c.BackColor, c.ForeColor };

        foreach (Control child in c.Controls)
            DarkModeCapture(child);
    }

    private void DarkModeRestore()
    {
        if (_lightColors == null) return;

        foreach (var kv in _lightColors)
        {
            kv.Key.BackColor = kv.Value[0];
            kv.Key.ForeColor = kv.Value[1];
        }
    }

    /* ContextMenuStrip is a Component, not a Control, so the Controls walk in
     * ApplyDarkMode never reaches it. */
    private void DarkModeStyleMenu(ContextMenuStrip menu)
    {
        Color back = _darkMode ? Color.FromArgb(45, 45, 45) : SystemColors.Control;
        Color text = _darkMode ? Color.White              : SystemColors.ControlText;

        menu.BackColor = back;
        menu.ForeColor = text;
        menu.RenderMode = _darkMode ? ToolStripRenderMode.System : ToolStripRenderMode.ManagerRenderMode;

        foreach (ToolStripItem it in menu.Items)
        {
            it.BackColor = back;
            it.ForeColor = text;
        }
    }

    private void BannerApplyCustomIfPresent()
    {
        if (!File.Exists(BannerCustomPath))
            return;

        try
        {
            /* Image.FromFile keeps the file locked for the lifetime of the Image,
             * which would make Reset fail with a sharing violation. Copy the pixels
             * and let the handle go. */
            Image loaded;
            using (var fs = new FileStream(BannerCustomPath, FileMode.Open, FileAccess.Read))
            using (var tmp = Image.FromStream(fs))
                loaded = new Bitmap(tmp);

            banner.Image = loaded;
            if (_bannerCustom != null) _bannerCustom.Dispose();
            _bannerCustom = loaded;
        }
        catch (Exception ex)
        {
            /* A corrupt or unreadable file must not stop the launcher opening --
             * fall back to the built-in banner and say why. */
            MessageBox.Show(this,
                "The custom launcher background could not be loaded, so the default is being used.\n\n" + ex.Message,
                "Custom background", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private void banner_MouseUp(object sender, MouseEventArgs e)
    {
        if (e.Button != MouseButtons.Right)
            return;

        /* Built once and reused. Disposing a ContextMenuStrip from its own Closed
         * handler races the item's Click, which may not have run yet. */
        if (_bannerMenu == null)
        {
            _bannerMenu = new ContextMenuStrip();
            _bannerMenu.Items.Add("Change background", null, (s2, e2) => BannerChange());
            _bannerMenuReset = (ToolStripMenuItem)_bannerMenu.Items.Add("Reset", null, (s2, e2) => BannerReset());
            _bannerMenu.Items.Add(new ToolStripSeparator());
            _bannerMenuDark = (ToolStripMenuItem)_bannerMenu.Items.Add("Dark mode", null, (s2, e2) => DarkModeSet(!_darkMode));
            _bannerMenu.Items.Add("Cancel", null, (s2, e2) => { });
        }

        _bannerMenuReset.Visible = File.Exists(BannerCustomPath);
        _bannerMenuDark.Checked  = _darkMode;
        DarkModeStyleMenu(_bannerMenu);
        _bannerMenu.Show(banner, e.Location);
    }

    private void BannerChange()
    {
        using (var dlg = new OpenFileDialog())
        {
            dlg.Title  = "Choose a launcher background";
            dlg.Filter = "Image files|*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff|All files|*.*";

            if (dlg.ShowDialog(this) != DialogResult.OK)
                return;

            try
            {
                /* Decode fully before touching the destination: if the pick turns
                 * out not to be an image, the existing background is untouched. */
                Bitmap copy;
                using (var fs = new FileStream(dlg.FileName, FileMode.Open, FileAccess.Read))
                using (var src = Image.FromStream(fs))
                    copy = new Bitmap(src);

                using (copy)
                    copy.Save(BannerCustomPath, System.Drawing.Imaging.ImageFormat.Png);

                BannerApplyCustomIfPresent();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this,
                    "That file could not be used as a background.\n\n" + ex.Message,
                    "Change background", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
    }

    private void BannerReset()
    {
        if (MessageBox.Show(this, "Are you sure?", "Reset background",
                            MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;

        try
        {
            banner.Image = SilentHillPC_Launcher.Properties.Resources.launcher;

            if (_bannerCustom != null)
            {
                _bannerCustom.Dispose();
                _bannerCustom = null;
            }

            if (File.Exists(BannerCustomPath))
                File.Delete(BannerCustomPath);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this,
                "The custom background could not be removed.\n\n" + ex.Message,
                "Reset background", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    /* Parallel to comboRender's items. Index is the only link between the two,
     * so re-ordering one without the other silently writes the wrong backend. */
    private static readonly string[] RendererValues =
    {
        "gl", "gles", "d3d11", "vulkan", "warp", "software",
    };

    /* Parallel to comboShadow's items, same contract as RendererValues. */
    private static readonly string[] ShadowResValues =
    {
        "256", "512", "1024", "2048", "4096",
    };

    private static int ParseIntOr(string s, int fallback)
    {
        int v;
        return int.TryParse((s ?? "").Trim(), out v) ? v : fallback;
    }

    private void radioPreloadYes_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void preloadPanel_Paint(object sender, PaintEventArgs e)
    {

    }

    private void progUpdate_Click(object sender, EventArgs e)
    {

    }

    private void consoleLabel_Click(object sender, EventArgs e)
    {

    }

    private void btnControls_Click(object sender, EventArgs e)
    {
        // Controls button: open the keyboard/controller binding window. Shares
        // this form's ConfigManager so its Save lands in the same config.cfg.
        using (var dlg = new ControlsForm(config))
        {
            dlg.ShowDialog(this);
        }
    }

    // RetroAchievements tooltip lives on its own ToolTip so it can be refreshed
    // after sign-in/out without rebuilding every other tip in SetupTooltips.
    private readonly ToolTip _raTip = new ToolTip
    {
        AutoPopDelay = 12000,
        InitialDelay = 400,
        ReshowDelay  = 200,
        ShowAlways   = true,
    };

    private void RefreshRaTooltip()
    {
        if (btnRA == null) return;
        bool signedIn = !string.IsNullOrEmpty(config.Get("ra_token", ""));
        bool enabled  = config.Get("retroachievements", "0") == "1";
        string user   = config.Get("ra_username", "");

        string text;
        if (signedIn)
        {
            text = "RetroAchievements: signed in as " + user +
                   (enabled ? "." : " (achievements currently disabled).") +
                   "\r\nClick to sign out or switch accounts.";
        }
        else
        {
            text = "RetroAchievements: not signed in.\r\n" +
                   "Click to sign in and earn achievements on your real RA account " +
                   "(softcore, matched to your disc).";
        }
        _raTip.SetToolTip(btnRA, text);
    }

    private void btnRA_Click(object sender, EventArgs e)
    {
        // Straight to the dialog whether signed in or not. It already reports
        // the signed-in account and carries sign-out, so the confirm prompt
        // only stood between the user and the other settings on that page.
        using (var dlg = new RetroAchievementsForm(config))
        {
            dlg.ShowDialog(this);
        }
        RefreshRaTooltip();
    }

    private void btnBuildSettings_Click(object sender, EventArgs e)
    {
        // Build Settings: choose the repo/branch/build the launcher tracks for
        // updates (saved into config.cfg's ## Launcher section). Re-probe the
        // silent status afterward so the label reflects the new selection.
        using (var dlg = new BuildSettingsForm(config))
        {
            dlg.ShowDialog(this);
        }
        SilentAutoCheckForUpdates();
    }

    private void pgxpYes_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void pillarboxPanel_Paint(object sender, PaintEventArgs e)
    {

    }

    // Help button (btnHelp): contact info + a quick tip about debug controls.
    // Uses a custom dialog with a LinkLabel so "@KushAstronaut" and the Discord
    // invite are clickable (a plain MessageBox can't host links).
    private void button1_Click(object sender, EventArgs e)
    {
        const string discordProfile = "https://discord.com/users/363942283348934656";
        const string discordInvite = "https://discord.com/invite/JWuNzVsQbr";
        const string emailiCloudURL = "mailto:kushastronaut@icloud.com";
        const string emailgmailURL = "mailto:kushastronaut@gmail.com";
        const string emailiCloud = "kushastronaut@icloud.com";
        const string emailgmail = "kushastronaut@gmail.com";
        const string profileToken   = "@KushAstronaut";

        string text =
            "If you need help, please reach out to me on Discord " + profileToken + ", " +
            "or by email at " + emailiCloud + " or " + emailgmail + ". " +
            "I have created a Discord server for this port that you can join as well, " +
            "here is the link: " + discordInvite + " - It will have helpful info, " +
            "update news, and sometimes early releases.\r\n\r\n" +
            "Tip: There are a lot of cheats and debug commands available, and to enable " +
            "them you have to turn on the debug controls setting in the controls menu of " +
            "the launcher. When they're on, you can hold ~ to toggle the console, and " +
            "press ~ again with it open to input a command. Type help or debug to get a " +
            "list of different controls and commands!";

        using (var dlg = new Form())
        {
            dlg.Text = "Help";
            dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
            dlg.StartPosition = FormStartPosition.CenterParent;
            dlg.MaximizeBox = false;
            dlg.MinimizeBox = false;
            dlg.ShowInTaskbar = false;
            dlg.ClientSize = new System.Drawing.Size(430, 235);

            var link = new LinkLabel
            {
                Text = text,
                AutoSize = false,
                Location = new System.Drawing.Point(0, 0),
                Size = new System.Drawing.Size(430, 198),
                Padding = new Padding(12),
                LinkBehavior = LinkBehavior.HoverUnderline,
            };

            void AddLink(string token, string url)
            {
                int idx = text.IndexOf(token, StringComparison.Ordinal);
                if (idx >= 0)
                    link.Links.Add(idx, token.Length, url);
            }
            AddLink(profileToken, discordProfile);
            AddLink(emailgmail, emailgmailURL);
            AddLink(emailiCloud, emailiCloudURL);
            AddLink(discordInvite, discordInvite);

            link.LinkClicked += (s2, e2) =>
            {
                var url = e2.Link.LinkData as string;
                if (string.IsNullOrEmpty(url)) return;
                try { Process.Start(url); } catch { /* no browser / blocked */ }
            };

            var ok = new Button
            {
                Text = "OK",
                DialogResult = DialogResult.OK,
                Size = new System.Drawing.Size(75, 23),
                Location = new System.Drawing.Point(343, 203),
            };

            dlg.Controls.Add(link);
            dlg.Controls.Add(ok);
            dlg.AcceptButton = ok;
            dlg.ShowDialog(this);
        }
    }

    // Report Bug button (btnBug): open the GitHub issues page in the browser.
    private void button2_Click(object sender, EventArgs e)
    {
        try
        {
            Process.Start("https://github.com/SlickAmogus/silent-hill-decomp/issues");
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "Couldn't open the browser:\n" + ex.Message,
                "Report Bug", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    // Reset button (btnReset): confirm, then overwrite config.cfg with the
    // built-in default (the embedded copy of pc_port/config.cfg) and reload.
    private void button1_Click_1(object sender, EventArgs e)
    {
        var result = MessageBox.Show(this,
            "Are you sure you would like to reset all settings to the default?",
            "Reset Settings", MessageBoxButtons.YesNo, MessageBoxIcon.Question);
        if (result != DialogResult.Yes)
            return;

        string cfgPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.cfg");
        try
        {
            var asm = System.Reflection.Assembly.GetExecutingAssembly();
            using (var s = asm.GetManifestResourceStream("SilentHillPC_Launcher.DefaultConfig.cfg"))
            {
                if (s == null)
                {
                    MessageBox.Show(this, "Built-in default config is missing; nothing was changed.",
                        "Reset Settings", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                using (var f = File.Create(cfgPath))
                    s.CopyTo(f);
            }
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, "Couldn't write config.cfg:\n" + ex.Message,
                "Reset Settings", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        // Reload the freshly written defaults into the UI.
        LoadConfig();
    }

    private void regionLabel_Click(object sender, EventArgs e)
    {

    }

    private void checkBox1_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void lblUncensored_Click(object sender, EventArgs e)
    {

    }

    private void chkUncensored_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void label2_Click(object sender, EventArgs e)
    {

    }
}
