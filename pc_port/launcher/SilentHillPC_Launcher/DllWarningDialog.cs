using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Consent dialog shown before a DLL (gameplay / total conversion) mod is
    /// imported. WinForms MessageBox cannot label three buttons, hence a form.
    /// "Don't show me again" is persisted by the caller (ModManager.DllWarningAck).
    /// </summary>
    public static class DllWarningDialog
    {
        public enum Result { Continue, Cancel, DontShowAgain }

        public static Result Show(IWin32Window owner) { return Show(owner, null); }

        public static Result Show(IWin32Window owner, string extraWarning)
        {
            using (var f = new Form())
            {
                f.Text            = "DLL Mod Warning";
                f.FormBorderStyle = FormBorderStyle.FixedDialog;
                f.StartPosition   = FormStartPosition.CenterParent;
                f.MinimizeBox     = false;
                f.MaximizeBox     = false;
                f.ShowInTaskbar   = false;
                bool tall = !string.IsNullOrEmpty(extraWarning);
                f.ClientSize      = new Size(480, tall ? 280 : 170);

                var icon = new PictureBox
                {
                    Image    = SystemIcons.Warning.ToBitmap(),
                    SizeMode = PictureBoxSizeMode.CenterImage,
                    Bounds   = new Rectangle(14, 18, 40, 40)
                };
                f.Controls.Add(icon);

                var lbl = new Label
                {
                    Text = "Warning: You are about to install a DLL mod. These mods alter the " +
                           "source code of the game, and technically execute code on your machine. " +
                           "Please make sure you trust the person or site you have downloaded it from.",
                    Bounds = new Rectangle(64, 14, 402, 96),
                    AutoSize = false
                };
                f.Controls.Add(lbl);

                if (tall)
                {
                    var lbl2 = new Label
                    {
                        Text      = extraWarning,
                        ForeColor = Color.Firebrick,
                        Bounds    = new Rectangle(64, 112, 402, 106),
                        AutoSize  = false
                    };
                    f.Controls.Add(lbl2);
                }

                Result result = Result.Cancel;

                int by = tall ? 232 : 122;
                var btnContinue = new Button { Text = "Continue", Bounds = new Rectangle(120, by, 90, 30) };
                btnContinue.Click += (s, e) => { result = Result.Continue; f.DialogResult = DialogResult.OK; };
                f.Controls.Add(btnContinue);

                var btnCancel = new Button { Text = "Cancel", Bounds = new Rectangle(218, by, 90, 30) };
                btnCancel.Click += (s, e) => { result = Result.Cancel; f.DialogResult = DialogResult.Cancel; };
                f.Controls.Add(btnCancel);

                var btnNever = new Button { Text = "Don't show me again", Bounds = new Rectangle(316, by, 150, 30) };
                btnNever.Click += (s, e) => { result = Result.DontShowAgain; f.DialogResult = DialogResult.OK; };
                f.Controls.Add(btnNever);

                f.AcceptButton = btnContinue;
                f.CancelButton = btnCancel;

                f.ShowDialog(owner);
                return result;
            }
        }
    }
}
