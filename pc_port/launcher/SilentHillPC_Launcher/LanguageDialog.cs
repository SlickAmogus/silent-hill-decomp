using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// The small popup behind the flag button: one dropdown, and the flag of
    /// whatever is picked. Applies immediately so the change is visible while
    /// the box is still open.
    /// </summary>
    public class LanguageDialog : Form
    {
        readonly ComboBox _combo;
        readonly Label _flag;

        public LauncherLang Selected { get; private set; }

        public LanguageDialog(LauncherLang current)
        {
            Selected = current;

            Text = Loc.T("Launcher language");
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = MaximizeBox = false;
            ShowInTaskbar = false;
            ClientSize = new Size(268, 96);

            _flag = new Label
            {
                Location = new Point(12, 14),
                Size = new Size(38, 30),
                TextAlign = ContentAlignment.MiddleCenter,
                // Segoe UI Emoji is what renders the regional-indicator pairs as
                // flags on Windows; the default UI font draws two letters.
                Font = new Font("Segoe UI Emoji", 15f),
                Text = Loc.Flag(current)
            };

            _combo = new ComboBox
            {
                Location = new Point(56, 18),
                Size = new Size(196, 24),
                DropDownStyle = ComboBoxStyle.DropDownList,
                Font = new Font("Segoe UI", 9.75f)
            };
            foreach (var l in Loc.All)
                _combo.Items.Add(Loc.NativeName(l));
            _combo.SelectedIndex = Array.IndexOf(Loc.All, current);
            _combo.SelectedIndexChanged += (s, e) =>
            {
                if (_combo.SelectedIndex < 0) return;
                Selected = Loc.All[_combo.SelectedIndex];
                _flag.Text = Loc.Flag(Selected);
                Loc.Set(Selected);      // live: the launcher behind updates now
                Text = Loc.T("Launcher language");
                Loc.Apply(this);
            };

            var ok = new Button
            {
                Text = Loc.T("OK"),
                DialogResult = DialogResult.OK,
                Location = new Point(160, 58),
                Size = new Size(92, 26)
            };
            AcceptButton = ok;
            CancelButton = ok;   // no destructive path: the change already applied

            Controls.Add(_flag);
            Controls.Add(_combo);
            Controls.Add(ok);
        }
    }
}
