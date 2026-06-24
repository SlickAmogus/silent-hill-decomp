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

    public Form1()
    {
        InitializeComponent();
        this.Icon = SilentHillPC_Launcher.Properties.Resources.launchericon;
        PopulateDisplayOptions();
        LoadConfig();
        SetupTooltips();
        this.Shown += (s, e) =>
        {
            CleanupOldFiles();
            UpdateChecker.CleanupStaleTemp();
            CheckDiscImage();
            SilentAutoCheckForUpdates();
        };
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
    /// Warn at startup when the disc image is missing (creating gamedata/ if
    /// needed). Gated on SilentHillPC.exe being present so dev runs from
    /// bin\Release don't nag or scaffold stray folders.
    /// </summary>
    private void CheckDiscImage()
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        if (!File.Exists(Path.Combine(dir, "SilentHillPC.exe"))) return;

        string gamedata = Path.Combine(dir, "gamedata");
        try { if (!Directory.Exists(gamedata)) Directory.CreateDirectory(gamedata); }
        catch { return; }

        if (!File.Exists(Path.Combine(gamedata, "Silent Hill (USA).bin")))
        {
            MessageBox.Show(this,
                "Please put Silent Hill (USA).bin in the gamedata folder!",
                "Silent Hill PC",
                MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
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
        btnUpdate.Text     = "Check for Updates";
        downloadBuild.Text = "Download Build";
        try
        {
            lblUpdateStatus.Text = "Checking for updates...";
            var plan = await UpdateChecker.CheckAsync(installDir, ForceLatest(settings));

            // If the install already matches the latest, remember that so it
            // isn't mis-reported as an update later.
            if (!plan.HasUpdate) settings.RecordSeenAtLeast(config, plan.RemoteVersion);

            bool available = LauncherSettings.CompareVersions(plan.RemoteVersion, settings.GetSeen(config)) > 0;
            if (available)
            {
                btnUpdate.Text = "Update available!";
                lblUpdateStatus.ForeColor = Color.LightGreen;
                lblUpdateStatus.Text = $"Update available: {plan.RemoteVersion}";
            }
            else
            {
                lblUpdateStatus.ForeColor = Color.LightGray;
                lblUpdateStatus.Text = $"Up to date ({plan.RemoteVersion}).";
            }

            // "Download Build" vs "Redownload Build": is the SELECTED build already
            // installed? For "latest" reuse the check above; for a pinned build do
            // a quick second check.
            bool selectedInstalled = settings.IsLatestBuild
                ? !plan.HasUpdate
                : !(await UpdateChecker.CheckAsync(installDir, settings)).HasUpdate;
            downloadBuild.Text = selectedInstalled ? "Redownload Build" : "Download Build";
        }
        catch
        {
            // Silent — no internet, no nightly repo yet, etc.
            lblUpdateStatus.Text = "";
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

        void Set(Control c, string text)
        {
            if (c == null) return;
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

        const string pgxpTip =
            "On: sub-pixel-precision vertices and perspective-correct textures\n" +
            "(reduced PSX vertex jitter and texture warping).\n" +
            "Off: authentic PSX look (affine textures, vertex snapping).\n" +
            "Press F1 in-game to toggle on the fly.";
        Set(pgxpLabel,  pgxpTip);
        Set(pgxpYes,    pgxpTip);
        Set(pgxpNo,     pgxpTip);

        const string cullingTip =
            "Disable the game's aggressive PSX view culling.\n" +
            "Yes = render everything (recommended) — stops fences, trees and\n" +
            "      other geometry from popping in/out as the camera turns.\n" +
            "No = original PSX behavior (objects culled by view angle).";
        Set(cullLabel,        cullingTip);
        Set(radioCullingYes,  cullingTip);
        Set(radioCullingNo,   cullingTip);

        const string preloadTip =
            "Preload all map chunks at level start instead of streaming\n" +
            "them in as you walk. Eliminates pop-in but uses more memory\n" +
            "and lengthens the initial load. Yes is recommended; No =\n" +
            "original PSX streaming.";
        Set(chunksLabel,      preloadTip);
        Set(radioPreloadYes,  preloadTip);
        Set(radioPreloadNo,   preloadTip);

        const string pillarboxTip =
            "Pillarbox 2D screens (menus, save/load, the Harry-running load\n" +
            "screen) with 4:3 black bars instead of stretching them to fill a\n" +
            "widescreen window. Only applies on widescreen (16:9 / wider)\n" +
            "displays — on a 4:3 window there are no bars either way.\n" +
            "Only affects 2D UI; 3D gameplay is unchanged.";
        Set(refreshLabel,        pillarboxTip);
        Set(radioPillarboxYes,   pillarboxTip);
        Set(radioPillarboxNo,    pillarboxTip);

        const string introTip =
            "Skip the boot logos and the intro FMV — jump straight to the\n" +
            "main menu. Convenient during testing.";
        Set(introLabel,  introTip);
        Set(introYes,    introTip);
        Set(introNo,     introTip);

        const string loggingTip =
            "Write SH_DBG output to SilentHill.log next to the executable.\n" +
            "Required for diagnosing crashes/regressions; small disk-write\n" +
            "overhead. Leave set to Yes if you might report a bug.";
        Set(loggingLabel,  loggingTip);
        Set(loggingYes,    loggingTip);
        Set(loggingNo,     loggingTip);

        const string consoleTip =
            "Off: no console output.\n" +
            "External: secondary console window mirrors SH_DBG_ECHO lines live.\n" +
            "Ingame: [ and ] key markers appear as an in-game overlay.\n" +
            "Ingame + External: overlay gets SH_DBG_ECHO too, plus external window.";
        Set(consoleLabel,   consoleTip);
        Set(comboConsole,   consoleTip);

        const string looseTip =
            "Allow the game to load replacement assets from\n" +
            "gamedata/load/{folder}/{name}.{ext} instead of the packed\n" +
            "originals. Enables texture mods. No = packed assets only.";
        Set(looseLabel,  looseTip);
        Set(looseYes,    looseTip);
        Set(looseNo,     looseTip);

        const string levelTip =
            "Which map to load when you start a New Game. Default map0_s00\n" +
            "is the intro alley. Useful for jumping straight to a specific\n" +
            "scene during testing.";
        Set(levelLabel,  levelTip);
        Set(comboMap,    levelTip);

        Set(btnPlay, "Save current settings to config.cfg and launch SilentHillPC.exe.");
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
        var modes = DisplayModes.GetModes();

        // Unique resolutions
        var resolutions = new HashSet<string>();
        foreach (var m in modes)
            resolutions.Add($"{m.width}x{m.height}");

        comboResolution.Items.AddRange(resolutions.ToArray());

        // Unique refresh rates
        var refreshRates = new HashSet<int>();
        foreach (var m in modes)
            refreshRates.Add(m.hz);

        foreach (var hz in refreshRates)
            comboRefresh.Items.Add(hz.ToString());
    }


    private void LoadConfig()
    {
        string cfgPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.cfg");
        EnsureConfigExists(cfgPath);
        config = new ConfigManager(cfgPath);

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

        // skip intro
        introYes.Checked = config.Get("skip_intros", "0") == "1";
        introNo.Checked = !introYes.Checked;

        // enable debug logging (writes SH_DBG output to SilentHill.log)
        loggingYes.Checked = config.Get("enable_debug_log", "0") == "1";
        loggingNo.Checked = !loggingYes.Checked;

        // debug console mode: 0=off, 1=external, 2=ingame, 3=ingame+external
        int consoleMode;
        if (!int.TryParse(config.Get("show_console", "0"), out consoleMode) || consoleMode < 0 || consoleMode > 3)
            consoleMode = 0;
        comboConsole.SelectedIndex = consoleMode;

        // allow loose files (texture mod support: gamedata/load/{folder}/{name}.{ext})
        looseYes.Checked = config.Get("allow_loose_files", "0") == "1";
        looseNo.Checked = !looseYes.Checked;

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

        // map dropdown -- parse descriptions from config.cfg `# mapX_sY  Desc` lines
        string[] mapIds = {
            "map0_s00","map0_s01","map0_s02",
            "map1_s00","map1_s01","map1_s02","map1_s03","map1_s04","map1_s05","map1_s06",
            "map2_s00","map2_s01","map2_s02","map2_s03","map2_s04",
            "map3_s00","map3_s01","map3_s02","map3_s03","map3_s04","map3_s05","map3_s06",
            "map4_s00","map4_s01","map4_s02","map4_s03","map4_s04","map4_s05","map4_s06",
            "map5_s00","map5_s01","map5_s02","map5_s03",
            "map6_s00","map6_s01","map6_s02","map6_s03","map6_s04","map6_s05",
            "map7_s00","map7_s01","map7_s02","map7_s03"
        };
        Dictionary<string, string> mapDescs = LoadMapDescriptions(cfgPath);
        // LoadConfig runs from both the constructor and Form1_Load; without
        // clearing first, the second pass duplicated every map entry.
        comboMap.Items.Clear();
        foreach (var id in mapIds)
        {
            string desc;
            comboMap.Items.Add(mapDescs.TryGetValue(id, out desc) ? $"{id}  -  {desc}" : id);
        }
        // Widen the dropdown list so descriptions don't get clipped
        comboMap.DropDownWidth = 400;

        string savedMap = config.Get("map", "map0_s00");
        // Find the dropdown entry whose id prefix matches the saved map id
        for (int i = 0; i < comboMap.Items.Count; i++)
        {
            string item = comboMap.Items[i].ToString();
            string itemId = item.Split(new[] { "  -  " }, StringSplitOptions.None)[0];
            if (itemId == savedMap) { comboMap.SelectedIndex = i; break; }
        }

        // resolution
        string w = config.Get("width", "640");
        string h = config.Get("height", "480");
        comboResolution.SelectedItem = $"{w}x{h}";

        // refresh rate is config-only now (auto-detected by default); keep the
        // hidden combo populated harmlessly so its references stay valid.
        comboRefresh.SelectedItem = config.Get("refresh_rate", "0");

        // pillarboxing (menu_pillarbox) — replaces the refresh-rate UI slot
        radioPillarboxYes.Checked = config.Get("menu_pillarbox", "1") == "1";
        radioPillarboxNo.Checked = !radioPillarboxYes.Checked;

        // disable_culling (recommended: Yes — matches engine default)
        radioCullingYes.Checked = config.Get("disable_culling", "1") == "1";
        radioCullingNo.Checked = !radioCullingYes.Checked;

        // preload_chunks (recommended: Yes — matches engine default)
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
        // Persist only the map id, not the displayed " - description" suffix
        if (comboMap.SelectedItem != null)
        {
            string sel = comboMap.SelectedItem.ToString();
            string mapId = sel.Split(new[] { "  -  " }, StringSplitOptions.None)[0];
            config.Set("map", mapId);
        }
        // resolution
        if (comboResolution.SelectedItem != null)
        {
            var parts = comboResolution.SelectedItem.ToString().Split('x');
            config.Set("width", parts[0]);
            config.Set("height", parts[1]);
        }

        // refresh rate is config-only now (auto-detected by default) — do NOT
        // write it from the launcher so a hand-set value is preserved.

        // pillarboxing
        config.Set("menu_pillarbox", radioPillarboxYes.Checked ? "1" : "0");

        // disable_culling
        config.Set("disable_culling", radioCullingYes.Checked ? "1" : "0");

        // preload_chunks
        config.Set("preload_chunks", radioPreloadYes.Checked ? "1" : "0");

        // skip intros
        config.Set("skip_intros", introYes.Checked ? "1" : "0");

        // enable debug logging
        config.Set("enable_debug_log", loggingYes.Checked ? "1" : "0");

        // debug console mode (0=off, 1=external, 2=ingame, 3=ingame+external)
        config.Set("show_console", comboConsole.SelectedIndex.ToString());

        // allow loose files (texture mod support)
        config.Set("allow_loose_files", looseYes.Checked ? "1" : "0");

        // PGXP toggle (work-in-progress)
        config.Set("use_pgxp", pgxpYes.Checked ? "1" : "0");

        if (comboFps.SelectedItem != null)
            config.Set("fps_cap", comboFps.SelectedItem.ToString());

        // Filtering: dropdown index (0=Off, 1=Dithering, 2=Bilinear) -> int
        if (comboFiltering.SelectedIndex >= 0)
            config.Set("psx_dither", comboFiltering.SelectedIndex.ToString());

        config.Save();
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

    private void btnPlay_Click(object sender, EventArgs e)
    {
        SaveConfig();

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
        lblUpdateStatus.Text = "Checking for updates...";
        progUpdate.Style   = ProgressBarStyle.Marquee;
        progUpdate.Visible = true;
        try
        {
            var plan = await UpdateChecker.CheckAsync(installDir, ForceLatest(settings));
            if (!plan.HasUpdate) settings.RecordSeenAtLeast(config, plan.RemoteVersion);

            bool available = LauncherSettings.CompareVersions(plan.RemoteVersion, settings.GetSeen(config)) > 0;
            if (!available)
            {
                btnUpdate.Text = "Check for Updates";
                lblUpdateStatus.Text = $"Up to date ({plan.RemoteVersion}).";
                progUpdate.Visible = false;
                MessageBox.Show(this,
                    $"You're up to date!\n\nSource: {plan.RepoLabel}\nLatest: {plan.RemoteVersion}",
                    "Check for Updates", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            btnUpdate.Text = "Update available!";
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
                lblUpdateStatus.Text = $"Update {plan.RemoteVersion} skipped.";
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
                btnUpdate.Text = "Check for Updates";
                downloadBuild.Text = "Redownload Build"; // latest is now installed
                lblUpdateStatus.ForeColor = Color.LightGray;
                lblUpdateStatus.Text = $"Up to date ({plan.RemoteVersion}).";
                MessageBox.Show(this, "Update complete!", "Update", MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            else
            {
                lblUpdateStatus.Text = $"Update {plan.RemoteVersion} skipped.";
            }
        }
        catch (Exception ex)
        {
            lblUpdateStatus.Text = "Update failed (see message).";
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
        lblUpdateStatus.Text = "Checking selected build...";
        progUpdate.Style   = ProgressBarStyle.Marquee;
        progUpdate.Visible = true;
        try
        {
            var plan = await UpdateChecker.CheckAsync(installDir, settings);

            if (!plan.HasUpdate)
            {
                progUpdate.Visible = false;
                lblUpdateStatus.Text = $"Build {plan.RemoteVersion} is installed.";
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
                lblUpdateStatus.Text = "Download cancelled.";
                progUpdate.Visible = false;
                return;
            }

            bool applied = await RunApplyAsync(installDir, plan, showFileConfirm: false);
            if (applied)
            {
                settings.RecordInstalled(config, plan.RemoteVersion);
                lblUpdateStatus.ForeColor = Color.LightGray;
                lblUpdateStatus.Text = $"Build {plan.RemoteVersion} installed.";
                MessageBox.Show(this, $"Build {plan.RemoteVersion} installed!", "Download Build",
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                SilentAutoCheckForUpdates(); // refresh the update indicator
            }
            else
            {
                lblUpdateStatus.Text = "Download cancelled.";
            }
        }
        catch (Exception ex)
        {
            lblUpdateStatus.Text = "Download failed (see message).";
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
                lblUpdateStatus.Text = msg ?? "";
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

    private void comboMap_SelectedIndexChanged(object sender, EventArgs e)
    {

    }

    private void comboRefresh_SelectedIndexChanged(object sender, EventArgs e)
    {

    }

    private void radioCullingYes_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void Form1_Load(object sender, EventArgs e)
    {

        //ApplyDarkMode();
        LoadConfig();
    }

    private void introYes_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void radioPreloadYes_CheckedChanged(object sender, EventArgs e)
    {

    }

    private void introPanel_Paint(object sender, PaintEventArgs e)
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


}
