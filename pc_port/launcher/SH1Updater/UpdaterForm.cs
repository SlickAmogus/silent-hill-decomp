using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using SilentHillPC_Launcher; // UpdateChecker — linked source, shared with the launcher

namespace SH1Updater
{
    /// <summary>
    /// Standalone updater for the Silent Hill PC port. Lives next to
    /// SilentHillPC.exe / SilentHillPC_Launcher.exe and updates the game,
    /// the launcher AND itself from the nightly manifest (version.json on
    /// SlickAmogus/silent-hill-pc-nightly). The launcher hands off to this
    /// exe so its own file isn't locked while being replaced; anything still
    /// locked (this exe updating itself) is moved aside to *.old and swapped
    /// in place — see UpdateChecker.ReplaceFile. Stale *.old files are
    /// cleaned up on the next run.
    /// </summary>
    public class UpdaterForm : Form
    {
        private static readonly Color Back      = Color.FromArgb(30, 30, 30);
        private static readonly Color PanelBack = Color.FromArgb(45, 45, 45);

        private Label       lblStatus;
        private ProgressBar prog;
        private TextBox     txtLog;
        private Button      btnInstall;
        private Button      btnLauncher;
        private Button      btnClose;

        private UpdateChecker.UpdatePlan _plan;
        private readonly string _installDir = AppDomain.CurrentDomain.BaseDirectory;

        public UpdaterForm()
        {
            BuildUi();
            Shown += async (s, e) =>
            {
                CleanupOldFiles();
                EnsureGameDataFolder();
                await RunCheckAsync();
            };
        }

        private void BuildUi()
        {
            Text            = "Silent Hill PC — Updater";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox     = false;
            StartPosition   = FormStartPosition.CenterScreen;
            BackColor       = Back;
            ForeColor       = Color.White;
            Font            = new Font("Segoe UI", 9f);
            ClientSize      = new Size(540, 330);

            lblStatus = new Label
            {
                Left = 14, Top = 12, Width = ClientSize.Width - 28,
                Text = "Starting...",
            };
            Controls.Add(lblStatus);

            prog = new ProgressBar
            {
                Left = 14, Top = 38, Width = ClientSize.Width - 28, Height = 16,
                Style = ProgressBarStyle.Marquee, Visible = false,
            };
            Controls.Add(prog);

            txtLog = new TextBox
            {
                Left = 14, Top = 62, Width = ClientSize.Width - 28,
                Height = ClientSize.Height - 62 - 52,
                Multiline = true, ReadOnly = true,
                ScrollBars = ScrollBars.Vertical,
                BackColor = PanelBack, ForeColor = Color.Gainsboro,
                BorderStyle = BorderStyle.FixedSingle,
            };
            Controls.Add(txtLog);

            btnInstall  = MakeButton("Install Update", 14);
            btnLauncher = MakeButton("Open Launcher", ClientSize.Width - 250);
            btnClose    = MakeButton("Close", ClientSize.Width - 110);

            btnInstall.Enabled = false;
            btnInstall.Click  += async (s, e) => await RunInstallAsync();
            btnLauncher.Click += (s, e) => OpenLauncher();
            btnClose.Click    += (s, e) => Close();
        }

        private Button MakeButton(string text, int x)
        {
            var b = new Button
            {
                Text = text, Left = x, Top = ClientSize.Height - 40,
                Width = 100, Height = 28,
                BackColor = PanelBack, ForeColor = Color.White,
                FlatStyle = FlatStyle.Flat,
            };
            if (text.Length > 12) b.Width = 130;
            Controls.Add(b);
            return b;
        }

        private void Log(string line)
        {
            txtLog.AppendText(line + Environment.NewLine);
        }

        /// <summary>
        /// First-run bootstrap: the updater can download everything except the
        /// disc image. Create gamedata/ and tell the user where the BIN goes.
        /// </summary>
        private void EnsureGameDataFolder()
        {
            string gamedata = Path.Combine(_installDir, "gamedata");
            if (Directory.Exists(gamedata)) return;

            try { Directory.CreateDirectory(gamedata); }
            catch { return; }

            Log("Created gamedata folder.");
            MessageBox.Show(this,
                "Please put Silent Hill (USA).bin in the gamedata folder!",
                "Silent Hill PC — Updater",
                MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        /// <summary>Delete *.old leftovers from previous in-place exe swaps.</summary>
        private void CleanupOldFiles()
        {
            try
            {
                foreach (var f in Directory.GetFiles(_installDir, "*.old"))
                    try { File.Delete(f); } catch { /* still locked — next time */ }

                string maps = Path.Combine(_installDir, "maps");
                if (Directory.Exists(maps))
                    foreach (var f in Directory.GetFiles(maps, "*.old"))
                        try { File.Delete(f); } catch { }
            }
            catch { /* best effort */ }
        }

        private async System.Threading.Tasks.Task RunCheckAsync()
        {
            lblStatus.Text = "Checking for updates...";
            prog.Style     = ProgressBarStyle.Marquee;
            prog.Visible   = true;

            try
            {
                _plan = await UpdateChecker.CheckAsync(_installDir);
                prog.Visible = false;

                if (!_plan.HasUpdate)
                {
                    lblStatus.Text = $"Up to date (latest: {_plan.RemoteVersion}).";
                    Log("Everything is up to date.");
                    return;
                }

                lblStatus.Text = $"Update available: {_plan.RemoteVersion} ({_plan.Changed.Count} file(s))";
                Log($"Update {_plan.RemoteVersion} (built {_plan.BuildDate}) — files to download:");
                foreach (var f in _plan.Changed)
                    Log("  " + f.Path);
                btnInstall.Enabled = true;
            }
            catch (Exception ex)
            {
                prog.Visible   = false;
                lblStatus.Text = "Update check failed.";
                Log("Check failed: " + ex.Message);
            }
        }

        private async System.Threading.Tasks.Task RunInstallAsync()
        {
            if (_plan == null || !_plan.HasUpdate) return;

            btnInstall.Enabled  = false;
            btnLauncher.Enabled = false;
            btnClose.Enabled    = false;
            prog.Style   = ProgressBarStyle.Continuous;
            prog.Minimum = 0;
            prog.Maximum = 100;
            prog.Visible = true;

            try
            {
                await UpdateChecker.ApplyAsync(_installDir, _plan, (frac, msg) =>
                {
                    BeginInvoke((Action)(() =>
                    {
                        if (frac >= 0 && frac <= 1)
                            prog.Value = (int)(frac * 100);
                        lblStatus.Text = msg ?? "";
                    }));
                });

                lblStatus.Text = $"Updated to {_plan.RemoteVersion}.";
                Log("Update complete.");
                _plan = null;
            }
            catch (Exception ex)
            {
                lblStatus.Text = "Update failed.";
                Log("Update failed: " + ex.Message);
                MessageBox.Show(this, "Update failed:\n\n" + ex.Message, "Update error",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                btnInstall.Enabled = _plan != null && _plan.HasUpdate;
            }
            finally
            {
                btnLauncher.Enabled = true;
                btnClose.Enabled    = true;
                prog.Visible        = false;
            }
        }

        private void OpenLauncher()
        {
            string launcher = Path.Combine(_installDir, "SilentHillPC_Launcher.exe");
            if (File.Exists(launcher))
            {
                try { Process.Start(launcher); }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Couldn't start the launcher:\n\n" + ex.Message,
                        "Updater", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                Close();
            }
            else
            {
                MessageBox.Show(this, "SilentHillPC_Launcher.exe not found next to the updater.",
                    "Updater", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }
    }
}
