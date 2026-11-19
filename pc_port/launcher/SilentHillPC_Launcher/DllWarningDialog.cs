using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Note shown the first time each mod with game code of its own (DLLs) is
    /// applied; ModManager.DllModsAcked remembers the mods it was agreed to for.
    /// A form rather than a MessageBox for the second section, which names DLLs
    /// whose imports reach past what game mods use (PeImports: a pointer for the
    /// reader, not a verdict either way).
    /// </summary>
    public static class DllWarningDialog
    {
        /// <summary>"x.dll (imports: a, b, c, d, ...)" cut to three names: a DLL can
        /// import dozens, and a wall of them buries the one line that matters.</summary>
        static string Shorten(string finding)
        {
            const string tag = " (imports: ";
            int i = finding.IndexOf(tag, StringComparison.Ordinal);
            if (i < 0 || !finding.EndsWith(")")) return finding;
            var names = finding.Substring(i + tag.Length, finding.Length - i - tag.Length - 1)
                               .Split(new[] { ", " }, StringSplitOptions.None);
            if (names.Length <= 3) return finding;
            return finding.Substring(0, i) + tag + string.Join(", ", names.Take(3)) + ", " +
                   Loc.F("... and {0} more", names.Length - 3) + ")";
        }

        /// <summary>True = go ahead and apply.</summary>
        public static bool Show(IWin32Window owner, IList<string> mods, IList<string> unusual)
        {
            using (var f = new Form())
            {
                f.Text            = Loc.T("Mods with game code");
                f.FormBorderStyle = FormBorderStyle.FixedDialog;
                f.StartPosition   = FormStartPosition.CenterParent;
                f.MinimizeBox     = false;
                f.MaximizeBox     = false;
                f.ShowInTaskbar   = false;
                f.ClientSize      = new Size(480, 200);

                var icon = new PictureBox
                {
                    Image    = SystemIcons.Information.ToBitmap(),
                    SizeMode = PictureBoxSizeMode.CenterImage,
                    Bounds   = new Rectangle(14, 14, 40, 40)
                };
                f.Controls.Add(icon);

                const int shown = 8;
                string list = string.Join("\n", mods.Take(shown).Select(m => "      " + m)) +
                              (mods.Count > shown ? "\n      " + Loc.F("... and {0} more", mods.Count - shown) : "");
                var labels = new List<Label>
                {
                    new Label
                    {
                        Text = Loc.T(mods.Count == 1 ? "This mod comes with its own game code (DLL files):"
                                                     : "These mods come with their own game code (DLL files):") +
                               "\n\n" + list + "\n\n" +
                               Loc.T("Like any program you download, that code runs on your PC, so only use mods " +
                                     "from people you trust. You'll only see this once for each mod.")
                    }
                };
                if (unusual != null && unusual.Count > 0)
                {
                    labels.Add(new Label
                    {
                        Text = Loc.T("Have a look first: these DLLs use parts of Windows that game mods don't usually need:") +
                               "\n\n - " + string.Join("\n - ", unusual.Take(5).Select(Shorten)) + (unusual.Count > 5 ? "\n - ..." : "") +
                               "\n\n" + Loc.T("Only go ahead if you know why the mod needs them."),
                        ForeColor = Color.Firebrick
                    });
                }

                // Each label as tall as its text at this width, so a long list or a
                // longer translation pushes the buttons down instead of clipping.
                int y = 18;
                foreach (var l in labels)
                {
                    l.AutoSize = false;
                    l.Bounds   = new Rectangle(64, y, 402,
                        TextRenderer.MeasureText(l.Text, f.Font, new Size(402, 0), TextFormatFlags.WordBreak).Height);
                    f.Controls.Add(l);
                    y = l.Bottom + 12;
                }

                bool ok = false;
                var btnEnable = new Button { Text = Loc.T("Enable") };
                btnEnable.Click += (s, e) => { ok = true; f.DialogResult = DialogResult.OK; };
                var btnCancel = new Button { Text = Loc.T("Cancel"), DialogResult = DialogResult.Cancel };

                int btnTop = Math.Max(y + 4, icon.Bottom + 16);
                int right  = f.ClientSize.Width - 14;
                foreach (var b in new[] { btnCancel, btnEnable })
                {
                    b.Size = new Size(Math.Max(90, TextRenderer.MeasureText(b.Text, f.Font).Width + 18), 30);
                    b.Location = new Point(right - b.Width, btnTop);
                    right = b.Left - 8;
                    f.Controls.Add(b);
                }
                f.ClientSize   = new Size(f.ClientSize.Width, btnTop + 30 + 14);
                f.AcceptButton = btnEnable;
                f.CancelButton = btnCancel;

                f.ShowDialog(owner);
                return ok;
            }
        }
    }
}
