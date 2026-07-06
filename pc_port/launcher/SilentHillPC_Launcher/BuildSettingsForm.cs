using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Build Settings modal: pick the GitHub branch + build the launcher tracks
    /// for updates, and (via Repo Settings) point at a different repo. Saves into
    /// config.cfg's "## Launcher" section. Hand-coded (no Designer) to match
    /// ControlsForm. Shares Form1's ConfigManager so edits land in the same file.
    /// </summary>
    public class BuildSettingsForm : Form
    {
        private readonly ConfigManager _config;
        private readonly LauncherSettings _settings;
        private readonly CancellationTokenSource _cts = new CancellationTokenSource();

        private TextBox  _txtRepo;
        private ComboBox _cmbBranch;
        private ComboBox _cmbBuild;
        private Label    _lblStatus;
        private bool     _suppressBranchChange;

        private Button _btnDownloadMac;
        private Button _btnDownloadLinux;
        private ToolTip _tips;

        private class Item
        {
            public string Display;
            public string Value;
            public override string ToString() => Display;
        }

        public BuildSettingsForm(ConfigManager config)
        {
            _config   = config;
            _settings = LauncherSettings.Load(config);
            BuildUi();
        }

        private void BuildUi()
        {
            Text            = "Build Settings";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition   = FormStartPosition.CenterParent;
            MaximizeBox     = false;
            MinimizeBox     = false;
            ShowInTaskbar   = false;
            ClientSize      = new Size(424, 250);
            BackColor       = Color.FromArgb(32, 32, 32);
            ForeColor       = Color.Gainsboro;

            _tips = new ToolTip { AutoPopDelay = 20000, InitialDelay = 350, ReshowDelay = 80, ShowAlways = true };

            var lblRepo = new Label { Text = "Repository:", Left = 12, Top = 16, AutoSize = true };
            _txtRepo = new TextBox
            {
                Left = 95, Top = 13, Width = 205, ReadOnly = true, Text = _settings.RepoUrl,
                BackColor = Color.FromArgb(48, 48, 48), ForeColor = Color.Gainsboro
            };
            var btnRepo = new Button { Text = "Repo Settings...", Left = 306, Top = 12, Width = 106, Height = 23 };
            btnRepo.Click += BtnRepo_Click;

            var lblBranch = new Label { Text = "Branch:", Left = 12, Top = 54, AutoSize = true };
            _cmbBranch = new ComboBox { Left = 95, Top = 51, Width = 317, DropDownStyle = ComboBoxStyle.DropDownList };
            _cmbBranch.SelectedIndexChanged += async (s, e) =>
            {
                if (_suppressBranchChange) return;
                await LoadBuildsAsync();
            };

            var lblBuild = new Label { Text = "Build:", Left = 12, Top = 91, AutoSize = true };
            _cmbBuild = new ComboBox { Left = 95, Top = 88, Width = 317, DropDownStyle = ComboBoxStyle.DropDownList };

            _lblStatus = new Label { Left = 12, Top = 124, Width = 400, Height = 44, ForeColor = Color.Gray };

            var lblArchives = new Label { Text = "Download archives for:", Left = 12, Top = 176, AutoSize = true };
            _btnDownloadMac = new Button { Text = "macOS", Left = 160, Top = 170, Width = 84, Height = 24 };
            _btnDownloadLinux = new Button { Text = "Linux", Left = 250, Top = 170, Width = 84, Height = 24 };
            _btnDownloadMac.Click += async (s, e) =>
                await DownloadArchiveAsync(_btnDownloadMac, "macOS", UpdateChecker.MacArchiveName);
            _btnDownloadLinux.Click += async (s, e) =>
                await DownloadArchiveAsync(_btnDownloadLinux, "Linux", UpdateChecker.LinuxArchiveName);
            const string archiveTip =
                "Downloads the Linux/macOS build of the SELECTED branch/build (above) into this launcher's folder, " +
                "next to the .exe -- overwriting any archive already downloaded for that platform (you'll be asked " +
                "to confirm first). These are separate standalone builds for other platforms; Windows still plays " +
                "from the normal SilentHillPC.exe. Only available once that platform's CI build has been attached " +
                "to the selected release.";
            _tips.SetToolTip(lblArchives, archiveTip);
            _tips.SetToolTip(_btnDownloadMac, archiveTip);
            _tips.SetToolTip(_btnDownloadLinux, archiveTip);

            var btnApply = new Button { Text = "Apply", Left = 232, Top = 213, Width = 80, Height = 26 };
            btnApply.Click += BtnApply_Click;
            var btnClose = new Button { Text = "Close", Left = 322, Top = 213, Width = 80, Height = 26, DialogResult = DialogResult.Cancel };
            var btnChangelog = new Button { Text = "View Changelog", Left = 12, Top = 213, Width = 120, Height = 26 };
            btnChangelog.Click += BtnChangelog_Click;

            Controls.Add(lblRepo);  Controls.Add(_txtRepo);  Controls.Add(btnRepo);
            Controls.Add(lblBranch); Controls.Add(_cmbBranch);
            Controls.Add(lblBuild);  Controls.Add(_cmbBuild);
            Controls.Add(_lblStatus);
            Controls.Add(lblArchives); Controls.Add(_btnDownloadMac); Controls.Add(_btnDownloadLinux);
            Controls.Add(btnChangelog); Controls.Add(btnApply); Controls.Add(btnClose);
            CancelButton = btnClose;

            Load        += async (s, e) => await LoadBranchesAsync();
            FormClosing += (s, e) => { try { _cts.Cancel(); } catch { } };
        }

        private async Task DownloadArchiveAsync(Button btn, string platformLabel, string assetName)
        {
            string destPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, assetName);
            bool overwriting = File.Exists(destPath);
            if (overwriting)
            {
                var confirm = MessageBox.Show(this,
                    "A " + platformLabel + " archive (" + assetName + ") is already downloaded in this folder.\n\n" +
                    "Download the version from the selected branch/build and overwrite it?",
                    "Overwrite " + platformLabel + " archive?", MessageBoxButtons.YesNo, MessageBoxIcon.Warning);
                if (confirm != DialogResult.Yes) return;
            }

            btn.Enabled = false;
            string branch = SelectedValue(_cmbBranch);
            string build  = string.IsNullOrEmpty(SelectedValue(_cmbBuild)) ? "latest" : SelectedValue(_cmbBuild);
            try
            {
                _lblStatus.Text = "Locating " + platformLabel + " build...";
                string url = await UpdateChecker.GetCrossPlatformAssetUrlAsync(_settings, branch, build, assetName, _cts.Token);
                if (string.IsNullOrEmpty(url))
                {
                    _lblStatus.Text = "";
                    MessageBox.Show(this,
                        "No " + platformLabel + " build is available for the selected branch/build.",
                        platformLabel + " build unavailable", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }

                await UpdateChecker.DownloadFileAsync(url, destPath, (frac, msg) =>
                {
                    if (IsDisposed) return;
                    BeginInvoke((Action)(() => _lblStatus.Text = "Downloading " + platformLabel + "... " + msg));
                }, _cts.Token);

                _lblStatus.Text = platformLabel + (overwriting ? " archive updated: " : " archive downloaded: ") + assetName;
            }
            catch (Exception ex)
            {
                _lblStatus.Text = "Couldn't download " + platformLabel + " build: " + ShortMsg(ex);
            }
            finally
            {
                btn.Enabled = true;
            }
        }

        private async Task LoadBranchesAsync()
        {
            _suppressBranchChange = true;
            _cmbBranch.Items.Clear();
            _cmbBuild.Items.Clear();
            _lblStatus.Text = "Loading branches...";
            try
            {
                var branches = await UpdateChecker.ListBranchesAsync(_settings, _cts.Token);
                // Beta is the actively-maintained/latest stream on the official repo —
                // list it first so it's both visually primary and the fallback
                // selection (SelectComboValue falls back to index 0) for a config
                // with no branch saved yet.
                foreach (var b in OrderBranchesForDisplay(branches))
                    _cmbBranch.Items.Add(new Item { Display = DisplayNameForBranch(b), Value = b });
                _lblStatus.Text = "";
            }
            catch (Exception ex)
            {
                _lblStatus.Text = "Could not load branches: " + ShortMsg(ex);
            }
            if (_cmbBranch.Items.Count == 0)
                _cmbBranch.Items.Add(new Item { Display = "default", Value = "" });
            SelectComboValue(_cmbBranch, _settings.Branch ?? "");
            _suppressBranchChange = false;
            await LoadBuildsAsync();
        }

        // Beta first (the actively-maintained/latest stream), then main, then
        // anything else alphabetically — so the dropdown's default (index-0)
        // selection is Beta.
        private static List<string> OrderBranchesForDisplay(List<string> branches)
        {
            string beta = branches.FirstOrDefault(b => string.Equals(b, "beta", StringComparison.OrdinalIgnoreCase));
            string main = branches.FirstOrDefault(b => string.Equals(b, "main", StringComparison.OrdinalIgnoreCase));
            var result = new List<string>();
            if (beta != null) result.Add(beta);
            if (main != null) result.Add(main);
            result.AddRange(branches.Where(b => b != beta && b != main).OrderBy(b => b, StringComparer.OrdinalIgnoreCase));
            return result;
        }

        // "beta"/"main" are the official nightly repo's two branches (beta = the
        // actively-maintained latest stream, main = the older alpha stream) —
        // label them clearly. Anything else (a custom repo via Repo Settings) is
        // shown as its raw branch name.
        private static string DisplayNameForBranch(string branch)
        {
            if (string.Equals(branch, "beta", StringComparison.OrdinalIgnoreCase)) return "Beta (Latest)";
            if (string.Equals(branch, "main", StringComparison.OrdinalIgnoreCase)) return "Main (Alpha)";
            return branch;
        }

        private async Task LoadBuildsAsync()
        {
            _cmbBuild.Items.Clear();
            _cmbBuild.Items.Add(new Item { Display = "latest", Value = "latest" });
            string branch = SelectedValue(_cmbBranch);
            _lblStatus.Text = "Loading builds...";
            try
            {
                var builds = await UpdateChecker.ListBuildsAsync(_settings, branch, _cts.Token);
                foreach (var b in builds)
                    _cmbBuild.Items.Add(new Item { Display = b.Label, Value = b.Tag });
                _lblStatus.Text = builds.Count + " build(s) on this branch. \"latest\" tracks the newest automatically.";
            }
            catch (Exception ex)
            {
                _lblStatus.Text = "Could not load builds: " + ShortMsg(ex);
            }
            if (!SelectComboValue(_cmbBuild, _settings.Build) && _cmbBuild.Items.Count > 0)
                _cmbBuild.SelectedIndex = 0; // latest
        }

        private async void BtnRepo_Click(object sender, EventArgs e)
        {
            using (var dlg = new RepoSettingsForm(_settings.RepoUrl))
            {
                if (dlg.ShowDialog(this) == DialogResult.OK && dlg.RepoUrl != _settings.RepoUrl)
                {
                    _settings.RepoUrl = dlg.RepoUrl;
                    _txtRepo.Text = _settings.RepoUrl;
                    _settings.Save(_config);      // persist the repo immediately
                    await LoadBranchesAsync();    // refresh against the new repo
                }
            }
        }

        private async void BtnChangelog_Click(object sender, EventArgs e)
        {
            // Preview the changelog of the currently SELECTED build (the dropdowns,
            // not what's saved), fetched from its release — no full download.
            var sel = new LauncherSettings
            {
                RepoUrl        = _settings.RepoUrl,
                Branch         = SelectedValue(_cmbBranch),
                Build          = string.IsNullOrEmpty(SelectedValue(_cmbBuild)) ? "latest" : SelectedValue(_cmbBuild),
                OldBuildWarned = _settings.OldBuildWarned,
            };
            var btn = sender as Button;
            if (btn != null) btn.Enabled = false;
            _lblStatus.Text = "Loading changelog...";
            try
            {
                string text = await UpdateChecker.GetChangelogTextAsync(sel, _cts.Token);
                ChangelogViewer.Show(this, "Changelog — " + (sel.IsLatestBuild ? "latest" : sel.Build), text);
                _lblStatus.Text = "";
            }
            catch (Exception ex)
            {
                _lblStatus.Text = "Couldn't load changelog: " + ShortMsg(ex);
            }
            finally
            {
                if (btn != null) btn.Enabled = true;
            }
        }

        private void BtnApply_Click(object sender, EventArgs e)
        {
            string newBranch = SelectedValue(_cmbBranch);
            string newBuild  = string.IsNullOrEmpty(SelectedValue(_cmbBuild)) ? "latest" : SelectedValue(_cmbBuild);

            // First time the user pins a specific (older) build, warn once about
            // bugs / save corruption and tell them to back up their saves.
            bool pinningSpecific = !newBuild.Equals("latest", StringComparison.OrdinalIgnoreCase);
            if (pinningSpecific && !_settings.OldBuildWarned)
            {
                MessageBox.Show(this,
                    "Heads up — switching to an older build can cause unexpected bugs and may even " +
                    "break your save data if you aren't careful.\n\n" +
                    "Back up your gamedata\\save folder first, just in case.\n\n" +
                    "This message will not be shown again.",
                    "Old build", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                _settings.OldBuildWarned = true;
            }

            _settings.Branch = newBranch;
            _settings.Build  = newBuild;
            _settings.Save(_config);
            _lblStatus.Text = "Saved. The launcher will check this branch/build for updates.";
            DialogResult = DialogResult.OK;
            Close();
        }

        private static string SelectedValue(ComboBox cmb)
        {
            var it = cmb.SelectedItem as Item;
            return it != null ? (it.Value ?? "") : "";
        }

        private static bool SelectComboValue(ComboBox cmb, string value)
        {
            for (int i = 0; i < cmb.Items.Count; i++)
            {
                var it = cmb.Items[i] as Item;
                if (it != null && string.Equals(it.Value ?? "", value ?? "", StringComparison.OrdinalIgnoreCase))
                {
                    cmb.SelectedIndex = i;
                    return true;
                }
            }
            if (cmb.Items.Count > 0) cmb.SelectedIndex = 0;
            return false;
        }

        private static string ShortMsg(Exception ex)
        {
            var m = ex.Message ?? "";
            return m.Length > 120 ? m.Substring(0, 120) + "..." : m;
        }
    }
}
