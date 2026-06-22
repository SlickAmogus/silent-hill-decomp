using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Small dialog to point the launcher at a different GitHub repo for updates.
    /// Accepts a full URL (https://github.com/Owner/Repo) or a bare "Owner/Repo".
    /// </summary>
    public class RepoSettingsForm : Form
    {
        private readonly TextBox _txt;

        public string RepoUrl =>
            string.IsNullOrWhiteSpace(_txt.Text) ? LauncherSettings.DefaultRepoUrl : _txt.Text.Trim();

        public RepoSettingsForm(string current)
        {
            Text            = "Repo Settings";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition   = FormStartPosition.CenterParent;
            MaximizeBox     = false;
            MinimizeBox     = false;
            ShowInTaskbar   = false;
            ClientSize      = new Size(440, 135);
            BackColor       = Color.FromArgb(32, 32, 32);
            ForeColor       = Color.Gainsboro;

            var lbl = new Label
            {
                Text = "GitHub repository (URL or Owner/Repo):",
                Left = 12, Top = 14, AutoSize = true
            };
            _txt = new TextBox
            {
                Left = 12, Top = 38, Width = 416, Text = current ?? "",
                BackColor = Color.FromArgb(48, 48, 48), ForeColor = Color.Gainsboro
            };
            var lblHint = new Label
            {
                Text = "Default: " + LauncherSettings.DefaultRepoUrl,
                Left = 12, Top = 66, AutoSize = true, ForeColor = Color.Gray
            };

            var btnReset  = new Button { Text = "Default", Left = 12,  Top = 98, Width = 80, Height = 26 };
            var btnOk     = new Button { Text = "OK",      Left = 262, Top = 98, Width = 80, Height = 26 };
            var btnCancel = new Button { Text = "Cancel",  Left = 348, Top = 98, Width = 80, Height = 26,
                                         DialogResult = DialogResult.Cancel };

            btnReset.Click += (s, e) => _txt.Text = LauncherSettings.DefaultRepoUrl;
            btnOk.Click += (s, e) =>
            {
                var test = new LauncherSettings
                {
                    RepoUrl = string.IsNullOrWhiteSpace(_txt.Text) ? LauncherSettings.DefaultRepoUrl : _txt.Text.Trim()
                };
                string o, r;
                if (!test.TryGetOwnerRepo(out o, out r))
                {
                    MessageBox.Show(this,
                        "That doesn't look like a GitHub repo.\n\nUse a URL like\n  https://github.com/Owner/Repo\nor just\n  Owner/Repo",
                        "Repo Settings", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                DialogResult = DialogResult.OK;
                Close();
            };

            Controls.Add(lbl);
            Controls.Add(_txt);
            Controls.Add(lblHint);
            Controls.Add(btnReset);
            Controls.Add(btnOk);
            Controls.Add(btnCancel);
            AcceptButton = btnOk;
            CancelButton = btnCancel;
        }
    }
}
