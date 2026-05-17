using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
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
        this.Shown += (s, e) => SilentAutoCheckForUpdates();
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
    private async void SilentAutoCheckForUpdates()
    {
        try
        {
            lblUpdateStatus.Text = "Checking for updates...";
            var plan = await UpdateChecker.CheckAsync(AppDomain.CurrentDomain.BaseDirectory);
            if (plan.HasUpdate)
            {
                lblUpdateStatus.Text = $"Update available: {plan.RemoteVersion} ({plan.Changed.Count} file(s))";
                lblUpdateStatus.ForeColor = Color.LightGreen;
                btnUpdate.Text = "Update available!";
            }
            else
            {
                lblUpdateStatus.Text = $"Up to date ({plan.RemoteVersion}).";
                lblUpdateStatus.ForeColor = Color.LightGray;
            }
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
            "Run the game fullscreen at the chosen resolution.\n" +
            "Off = windowed at the chosen resolution.";
        Set(fullscreenLabel,    fullscreenTip);
        Set(radioFullscreenYes, fullscreenTip);
        Set(radioFullscreenNo,  fullscreenTip);

        const string vsyncTip =
            "Synchronize frame presentation to your monitor's refresh rate.\n" +
            "On = no tearing, may add a frame of input latency.\n" +
            "Off = lowest latency, possible tearing.";
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
            "WORK IN PROGRESS — leave Off for now.\n" +
            "PGXP gives sub-pixel-precision GTE coords (no PSX vertex jitter)\n" +
            "and perspective-correct texture mapping. Many prim emit sites\n" +
            "are still being migrated, so most of the game looks worse with\n" +
            "this on until the wiring is finished.";
        Set(pgxpLabel,  pgxpTip);
        Set(pgxpYes,    pgxpTip);
        Set(pgxpNo,     pgxpTip);

        const string cullingTip =
            "Disable the game's distance/frustum culling.\n" +
            "On = renders everything regardless of distance (debug aid;\n" +
            "      useful when tracking down vanishing geometry).\n" +
            "Off = original PSX behavior (recommended).";
        Set(cullLabel,        cullingTip);
        Set(radioCullingYes,  cullingTip);
        Set(radioCullingNo,   cullingTip);

        const string preloadTip =
            "Preload all map chunks at level start instead of streaming\n" +
            "them in as you walk. Eliminates pop-in but uses more memory\n" +
            "and lengthens the initial load. Off = original streaming.";
        Set(chunksLabel,      preloadTip);
        Set(radioPreloadYes,  preloadTip);
        Set(radioPreloadNo,   preloadTip);

        const string introTip =
            "Skip the boot logos and the intro FMV — jump straight to the\n" +
            "main menu. Convenient during testing.";
        Set(introLabel,  introTip);
        Set(introYes,    introTip);
        Set(introNo,     introTip);

        const string loggingTip =
            "Write SH_DBG output to SilentHill.log next to the executable.\n" +
            "Required for diagnosing crashes/regressions; small disk-write\n" +
            "overhead. Leave on if you might report a bug.";
        Set(loggingLabel,  loggingTip);
        Set(loggingYes,    loggingTip);
        Set(loggingNo,     loggingTip);

        const string consoleTip =
            "Open a secondary console window that mirrors SH_DBG_ECHO lines\n" +
            "live as the game runs. Useful for watching state transitions\n" +
            "without tailing the log file.";
        Set(consoleLabel,  consoleTip);
        Set(consoleYes,    consoleTip);
        Set(consoleNo,     consoleTip);

        const string looseTip =
            "Allow the game to load replacement assets from\n" +
            "gamedata/load/{folder}/{name}.{ext} instead of the packed\n" +
            "originals. Enables texture mods. Off = packed assets only.";
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
    }

    /// <summary>
    /// Parse `# mapX_sY  Description text` lines from config.cfg comments
    /// into a map of id → description so the dropdown can show both.
    /// </summary>
    private Dictionary<string, string> LoadMapDescriptions(string cfgPath)
    {
        var result = new Dictionary<string, string>();
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
        catch { /* best-effort; missing descs just fall back to plain id */ }
        return result;
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
        config = new ConfigManager(cfgPath);

        // fullscreen
        radioFullscreenYes.Checked = config.Get("fullscreen", "0") == "1";
        radioFullscreenNo.Checked = config.Get("fullscreen", "0") == "0";

        // vsync
        radioVsyncYes.Checked = config.Get("vsync", "0") == "1";
        radioVsyncNo.Checked = config.Get("vsync", "0") == "0";

        // skip intro
        introYes.Checked = config.Get("skip_intros", "0") == "1";
        introNo.Checked = !introYes.Checked;

        // enable debug logging (writes SH_DBG output to SilentHill.log)
        loggingYes.Checked = config.Get("enable_debug_log", "0") == "1";
        loggingNo.Checked = !loggingYes.Checked;

        // show secondary console window (mirrors SH_DBG_ECHO lines live)
        consoleYes.Checked = config.Get("show_console", "0") == "1";
        consoleNo.Checked = !consoleYes.Checked;

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

        // refresh rate
        comboRefresh.SelectedItem = config.Get("refresh_rate", "0");

        // disable_culling
        radioCullingYes.Checked = config.Get("disable_culling", "0") == "1";
        radioCullingNo.Checked = !radioCullingYes.Checked;

        // preload_chunks
        radioPreloadYes.Checked = config.Get("preload_chunks", "0") == "1";
        radioPreloadNo.Checked = !radioPreloadYes.Checked;

    }

    private void SaveConfig()
    {
        config.Set("fullscreen", radioFullscreenYes.Checked ? "1" : "0");
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

        // refresh rate
        if (comboRefresh.SelectedItem != null)
            config.Set("refresh_rate", comboRefresh.SelectedItem.ToString());

        // disable_culling
        config.Set("disable_culling", radioCullingYes.Checked ? "1" : "0");

        // preload_chunks
        config.Set("preload_chunks", radioPreloadYes.Checked ? "1" : "0");

        // skip intros
        config.Set("skip_intros", introYes.Checked ? "1" : "0");

        // enable debug logging
        config.Set("enable_debug_log", loggingYes.Checked ? "1" : "0");

        // show secondary console window
        config.Set("show_console", consoleYes.Checked ? "1" : "0");

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

    private void btnPlay_Click(object sender, EventArgs e)
    {
        SaveConfig();

        string exePath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SilentHillPC.exe");

        if (!File.Exists(exePath))
        {
            MessageBox.Show("SilentHillPC.exe not found.");
            return;
        }

        Process.Start(exePath);
        Close();
    }

    // ------------------------------------------------------------------
    // Check-for-Updates handler. Pulls version.json from the nightly repo
    // (UpdateChecker.cs), diffs SHA-256 hashes against local files, and
    // downloads only the changed ones. Safe to run while game exe isn't
    // executing — the launcher always runs before the game, so file
    // overwrites of SilentHillPC.exe don't hit a "file in use" lock.
    // ------------------------------------------------------------------
    private async void btnUpdate_Click(object sender, EventArgs e)
    {
        string installDir = AppDomain.CurrentDomain.BaseDirectory;
        btnUpdate.Enabled = false;
        btnPlay.Enabled   = false;
        lblUpdateStatus.Text = "Checking for updates...";
        progUpdate.Style   = ProgressBarStyle.Marquee;
        progUpdate.Visible = true;

        try
        {
            var plan = await UpdateChecker.CheckAsync(installDir);

            if (!plan.HasUpdate)
            {
                lblUpdateStatus.Text = $"Up to date (latest: {plan.RemoteVersion}).";
                progUpdate.Visible = false;
                return;
            }

            // Build a human summary for the confirm dialog. Cap at ~10 files
            // so we don't blow out a MessageBox on a large changeset.
            var sb = new StringBuilder();
            sb.AppendLine($"Update available: {plan.RemoteVersion}");
            sb.AppendLine($"Built: {plan.BuildDate}");
            sb.AppendLine();
            sb.AppendLine($"{plan.Changed.Count} file(s) to download:");
            int i = 0;
            foreach (var f in plan.Changed)
            {
                if (i++ < 10) sb.AppendLine($"  • {f.Path}");
                else { sb.AppendLine($"  • ... and {plan.Changed.Count - 10} more"); break; }
            }
            sb.AppendLine();
            sb.AppendLine("Download and install now?");

            var resp = MessageBox.Show(this, sb.ToString(), "Update available",
                MessageBoxButtons.YesNo, MessageBoxIcon.Information);
            if (resp != DialogResult.Yes)
            {
                lblUpdateStatus.Text = $"Update {plan.RemoteVersion} skipped.";
                progUpdate.Visible = false;
                return;
            }

            progUpdate.Style = ProgressBarStyle.Continuous;
            progUpdate.Minimum = 0;
            progUpdate.Maximum = 100;

            await UpdateChecker.ApplyAsync(installDir, plan, (frac, msg) =>
            {
                // Marshal back to UI thread — the progress callback fires on
                // whatever thread HttpClient happens to be on.
                BeginInvoke((Action)(() =>
                {
                    if (frac >= 0 && frac <= 1)
                        progUpdate.Value = (int)(frac * 100);
                    lblUpdateStatus.Text = msg ?? "";
                }));
            });

            lblUpdateStatus.Text = $"Updated to {plan.RemoteVersion}.";
        }
        catch (Exception ex)
        {
            lblUpdateStatus.Text = "Update failed (see message).";
            MessageBox.Show(this, "Update failed:\n\n" + ex.Message, "Update error",
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally
        {
            btnUpdate.Enabled = true;
            btnPlay.Enabled   = true;
            progUpdate.Visible = false;
        }
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
}
