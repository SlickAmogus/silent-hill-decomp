using System;
using System.Drawing;
using System.Linq;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Mod manager modal (opened from Form1's manager button). Lists the mods in
    /// the self-owned <c>mods/</c> library, lets the user enable/disable and set
    /// load order, and deploys them into the game's override dirs on Apply.
    /// Code-generated (no Designer), sharing Form1's ConfigManager so the
    /// allow_loose_files / texture_packs writes land in the same config.cfg.
    /// </summary>
    public class ModManagerForm : Form
    {
        private readonly ModManager _mgr;
        private readonly ConfigManager _config;

        private ListView  _list;
        private CheckBox  _chkLoose;

        public ModManagerForm(ConfigManager config, string gameRoot)
        {
            _config = config;
            _mgr    = new ModManager(gameRoot, config);

            Text            = "Mod Manager";
            ClientSize      = new Size(600, 480);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition   = FormStartPosition.CenterParent;
            MaximizeBox     = false;
            MinimizeBox     = false;
            try { Icon = Properties.Resources.launchericon; } catch { }

            BuildUi();
            _mgr.Scan();
            Populate();
            _chkLoose.Checked = _config.Get("allow_loose_files", "0") == "1";
        }

        private void BuildUi()
        {
            var help = new Label
            {
                AutoSize  = false,
                Location  = new Point(12, 10),
                Size      = new Size(576, 34),
                Text      = "Drop mod folders or .zip archives into the mods folder. Check the ones to enable. " +
                            "Top of the list = highest priority (loads last, wins conflicts)."
            };
            Controls.Add(help);

            _list = new ListView
            {
                Location      = new Point(12, 50),
                Size          = new Size(490, 340),
                View          = View.Details,
                CheckBoxes    = true,
                FullRowSelect = true,
                HideSelection = false,
                MultiSelect   = false,
                HeaderStyle   = ColumnHeaderStyle.Nonclickable
            };
            _list.Columns.Add("Mod", 340);
            _list.Columns.Add("Type", 140);
            Controls.Add(_list);

            var btnUp = new Button { Text = "Move Up",   Location = new Point(510, 50), Size = new Size(78, 28) };
            var btnDn = new Button { Text = "Move Down", Location = new Point(510, 82), Size = new Size(78, 28) };
            var btnRe = new Button { Text = "Refresh",   Location = new Point(510, 122), Size = new Size(78, 28) };
            var btnOp = new Button { Text = "Open Folder", Location = new Point(510, 154), Size = new Size(78, 28) };
            btnUp.Click += (s, e) => MoveSelected(-1);
            btnDn.Click += (s, e) => MoveSelected(1);
            btnRe.Click += (s, e) => { CommitOrderAndState(); _mgr.Scan(); Populate(); };
            btnOp.Click += (s, e) => _mgr.OpenModsFolder();
            Controls.Add(btnUp);
            Controls.Add(btnDn);
            Controls.Add(btnRe);
            Controls.Add(btnOp);

            _chkLoose = new CheckBox
            {
                Text     = "Enable loose file support (required for load-folder mods)",
                Location = new Point(12, 398),
                AutoSize = true
            };
            Controls.Add(_chkLoose);

            var btnApply = new Button { Text = "Apply", Location = new Point(414, 440), Size = new Size(84, 30) };
            var btnClose = new Button { Text = "Close", Location = new Point(504, 440), Size = new Size(84, 30) };
            btnApply.Click += OnApply;
            btnClose.Click += (s, e) => { CommitOrderAndState(); _mgr.SaveState(); Close(); };
            Controls.Add(btnApply);
            Controls.Add(btnClose);

            AcceptButton = btnApply;
            CancelButton = btnClose;
        }

        private void Populate()
        {
            _list.BeginUpdate();
            _list.Items.Clear();
            foreach (var m in _mgr.Mods)
            {
                var item = new ListViewItem(m.Name) { Checked = m.Enabled, Tag = m };
                item.SubItems.Add(m.TypeLabel);
                if (m.Type == ModType.Unknown)
                    item.ForeColor = Color.Gray;
                _list.Items.Add(item);
            }
            _list.EndUpdate();
            if (_mgr.Mods.Count == 0)
            {
                // Nothing yet — leave a hint row (unchecked, non-mod).
                var hint = new ListViewItem("(no mods found — click Open Folder to add some)");
                hint.SubItems.Add("");
                hint.ForeColor = Color.Gray;
                _list.Items.Add(hint);
            }
        }

        private void MoveSelected(int delta)
        {
            if (_list.SelectedIndices.Count == 0) return;
            int i = _list.SelectedIndices[0];
            int j = i + delta;
            if (j < 0 || j >= _list.Items.Count) return;
            if (!(_list.Items[i].Tag is ModEntry) || !(_list.Items[j].Tag is ModEntry)) return;

            var item = _list.Items[i];
            _list.Items.RemoveAt(i);
            _list.Items.Insert(j, item);
            item.Selected = true;
            item.Focused  = true;
            _list.Focus();
        }

        /// <summary>Push the ListView's order + checkboxes back into the manager model.</summary>
        private void CommitOrderAndState()
        {
            var ordered = _list.Items.Cast<ListViewItem>()
                                     .Select(it => it.Tag as ModEntry)
                                     .Where(m => m != null)
                                     .ToList();
            foreach (ListViewItem it in _list.Items)
            {
                var m = it.Tag as ModEntry;
                if (m != null) m.Enabled = it.Checked;
            }
            _mgr.Mods = ordered;
        }

        private void OnApply(object sender, EventArgs e)
        {
            CommitOrderAndState();
            try
            {
                var r = _mgr.Apply(_chkLoose.Checked);
                _chkLoose.Checked = r.LooseEnabled;

                string msg = string.Format(
                    "Applied.\n\nTexture packs: {0}\nLoad-folder mods: {1}\nFMV mods: {2}\n" +
                    "Loose file support: {3}",
                    r.Texture, r.Load, r.Fmv, r.LooseEnabled ? "on" : "off");
                if (r.Warnings.Count > 0)
                    msg += "\n\nWarnings:\n - " + string.Join("\n - ", r.Warnings);

                MessageBox.Show(this, msg, "Mod Manager",
                    MessageBoxButtons.OK,
                    r.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Failed to apply mods:\n\n" + ex.Message, "Mod Manager",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }
}
