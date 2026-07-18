using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Shared read-only changelog viewer — used for the installed CHANGELOG.md
    /// and for previewing a remote build's changelog before downloading it.
    /// </summary>
    public static class ChangelogViewer
    {
        public static void Show(IWin32Window owner, string title, string text)
        {
            using (var dlg = new Form
            {
                Text          = title,
                Size          = new Size(680, 520),
                MinimumSize   = new Size(400, 300),
                StartPosition = FormStartPosition.CenterParent,
                BackColor     = Color.FromArgb(30, 30, 30),
                ForeColor     = Color.White,
                Font          = new Font("Consolas", 9f),
                ShowInTaskbar = false,
            })
            {
                var tb = new RichTextBox
                {
                    Dock        = DockStyle.Fill,
                    ReadOnly    = true,
                    Text        = text ?? "",
                    BackColor   = Color.FromArgb(30, 30, 30),
                    ForeColor   = Color.White,
                    BorderStyle = BorderStyle.None,
                    ScrollBars  = RichTextBoxScrollBars.Vertical,
                    Font        = new Font("Consolas", 9f),
                    WordWrap    = true,
                };
                var closeBtn = new Button
                {
                    Text      = "Close",
                    Dock      = DockStyle.Bottom,
                    Height    = 28,
                    BackColor = Color.FromArgb(60, 60, 60),
                    ForeColor = Color.White,
                    FlatStyle = FlatStyle.Flat,
                };
                closeBtn.Click += (s, e) => dlg.Close();

                dlg.Controls.Add(tb);
                dlg.Controls.Add(closeBtn);
                dlg.ShowDialog(owner);
            }
        }
    }
}
