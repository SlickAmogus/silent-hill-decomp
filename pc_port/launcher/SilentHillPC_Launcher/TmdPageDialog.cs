using System;
using System.Drawing;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Picks which of TIM01..06 to decode an item model's textures with.
    ///
    /// This has to be asked rather than inferred: VRAM page 14 holds ONE of the six
    /// per-map key-item texture banks at a time, and which one is a property of the
    /// map the player is in, not of the TMD — the model only stores "page 14". The
    /// choice affects the exported preview PNGs and the MTL only; the geometry, and
    /// therefore the OBJ → TMD round-trip, is identical whatever is picked.
    /// </summary>
    internal sealed class TmdPageDialog : Form
    {
        private readonly ComboBox _combo;

        public string Selected
        {
            get { return _combo.SelectedItem as string ?? TmdViewSceneBuilder.Tpage14Candidates[0]; }
        }

        public TmdPageDialog()
        {
            Text = "Item texture bank";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition = FormStartPosition.CenterParent;
            MinimizeBox = false;
            MaximizeBox = false;
            ClientSize = new Size(430, 168);

            var lbl = new Label
            {
                Location = new Point(12, 12),
                Size = new Size(406, 74),
                Text = "Key-item textures live on one VRAM page that the game refills per map, " +
                       "so a .TMD cannot say which bank its texture is in.\r\n\r\n" +
                       "Pick the one for the area this item belongs to. It only changes the " +
                       "preview images written beside the OBJ — the model itself is unaffected.",
            };
            _combo = new ComboBox
            {
                Location = new Point(12, 92),
                Size = new Size(190, 24),
                DropDownStyle = ComboBoxStyle.DropDownList,
            };
            foreach (string s in TmdViewSceneBuilder.Tpage14Candidates) _combo.Items.Add(s);
            _combo.SelectedIndex = 0;

            var ok = new Button { Text = "OK", DialogResult = DialogResult.OK, Location = new Point(252, 128), Size = new Size(80, 26) };
            var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, Location = new Point(338, 128), Size = new Size(80, 26) };

            Controls.Add(lbl);
            Controls.Add(_combo);
            Controls.Add(ok);
            Controls.Add(cancel);
            AcceptButton = ok;
            CancelButton = cancel;
        }
    }
}
