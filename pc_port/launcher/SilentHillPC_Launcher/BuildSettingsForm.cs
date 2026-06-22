using System;
using System.Drawing;
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
            ClientSize      = new Size(424, 215);
            BackColor       = Color.FromArgb(32, 32, 32);
            ForeColor       = Color.Gainsboro;

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

            var btnApply = new Button { Text = "Apply", Left = 232, Top = 178, Width = 80, Height = 26 };
            btnApply.Click += BtnApply_Click;
            var btnClose = new Button { Text = "Close", Left = 322, Top = 178, Width = 80, Height = 26, DialogResult = DialogResult.Cancel };
            var btnChangelog = new Button { Text = "View Changelog", Left = 12, Top = 178, Width = 120, Height = 26 };
            btnChangelog.Click += BtnChangelog_Click;

            Controls.Add(lblRepo);  Controls.Add(_txtRepo);  Controls.Add(btnRepo);
            Controls.Add(lblBranch); Controls.Add(_cmbBranch);
            Controls.Add(lblBuild);  Controls.Add(_cmbBuild);
            Controls.Add(_lblStatus); Controls.Add(btnChangelog); Controls.Add(btnApply); Controls.Add(btnClose);
            CancelButton = btnClose;

            Load        += async (s, e) => await LoadBranchesAsync();
            FormClosing += (s, e) => { try { _cts.Cancel(); } catch { } };
        }

        private async Task LoadBranchesAsync()
        {
            _suppressBranchChange = true;
            _cmbBranch.Items.Clear();
            _cmbBuild.Items.Clear();
            // The alpha stream (repo default / non-prerelease) is always available
            // (Branch = ""): this launcher build and every loose build before it.
            _cmbBranch.Items.Add(new Item { Display = "alpha (this build & older)", Value = "" });
            _lblStatus.Text = "Loading branches...";
            try
            {
                var branches = await UpdateChecker.ListBranchesAsync(_settings, _cts.Token);
                foreach (var b in branches)
                    _cmbBranch.Items.Add(new Item { Display = b, Value = b });
                _lblStatus.Text = "";
            }
            catch (Exception ex)
            {
                _lblStatus.Text = "Could not load branches: " + ShortMsg(ex);
            }
            SelectComboValue(_cmbBranch, _settings.Branch ?? "");
            _suppressBranchChange = false;
            await LoadBuildsAsync();
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
