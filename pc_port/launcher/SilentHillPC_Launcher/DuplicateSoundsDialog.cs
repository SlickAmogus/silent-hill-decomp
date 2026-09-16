/* SPDX-License-Identifier: GPL-3.0-or-later */
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>After a bank is saved: the other banks that carry byte-identical copies
    /// of the samples just replaced, with a checkbox per bank and a per-bank choice of
    /// which file to rewrite. The choice matters because a modder may already have a
    /// half-edited MAP201.VAB in gamedata/load/SND; rewriting the pristine extract
    /// instead would silently throw their earlier work away.</summary>
    internal sealed class DuplicateSoundsDialog : Form
    {
        private readonly ListView _list = new ListView();
        private readonly Label _intro = new Label();
        private readonly Button _btnSource = new Button();
        private readonly Button _btnWrite = new Button();
        private readonly Button _btnSkip = new Button();
        private readonly List<DuplicateTarget> _targets;
        private string _lastDir;

        public DuplicateSoundsDialog(string editedBank, List<DuplicateTarget> targets, string startDir)
        {
            _targets = targets;
            _lastDir = startDir;

            Text = "Audio — the same sounds in other banks";
            ClientSize = new Size(760, 400);
            MinimumSize = new Size(620, 300);
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            try { Icon = Properties.Resources.launchericon; } catch { }

            _intro.Location = new Point(12, 12);
            _intro.Size = new Size(736, 50);
            _intro.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            _intro.Text =
                "The game loads one ambient bank per map, and the disc copies the same sound into " +
                "every bank that needs it. The samples you replaced in " + editedBank + " also exist, " +
                "byte for byte, in the banks below. A replacement only plays where the loaded bank " +
                "has it, so tick the banks to update as well. \"Source\" is the file each rewrite " +
                "starts from; change it if you already have an edited copy somewhere else.";
            Controls.Add(_intro);

            _list.View = View.Details;
            _list.CheckBoxes = true;
            _list.FullRowSelect = true;
            _list.HideSelection = false;
            _list.GridLines = true;
            _list.MultiSelect = false;
            _list.Location = new Point(12, 68);
            _list.Size = new Size(736, 280);
            _list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;
            _list.Columns.Add("Bank", 90, HorizontalAlignment.Left);
            _list.Columns.Add("Samples", 110, HorizontalAlignment.Left);
            _list.Columns.Add("Source", 300, HorizontalAlignment.Left);
            _list.Columns.Add("Note", 220, HorizontalAlignment.Left);
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.ItemChecked += (s, e) => { ((DuplicateTarget)e.Item.Tag).Selected = e.Item.Checked; UpdateButtons(); };
            Controls.Add(_list);

            int y = 360;
            SetupButton(_btnSource, "Change source…", new Point(12, y), (s, e) => ChangeSource());
            _btnSource.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            SetupButton(_btnWrite, "Write ticked banks", new Point(540, y), (s, e) => { DialogResult = DialogResult.OK; Close(); });
            _btnWrite.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            SetupButton(_btnSkip, "Skip", new Point(672, y), (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
            _btnSkip.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            _btnSkip.DialogResult = DialogResult.Cancel;
            CancelButton = _btnSkip;

            Fill();
        }

        private void SetupButton(Button b, string text, Point p, EventHandler onClick)
        {
            b.Text = text;
            b.Location = p;
            b.Size = new Size(Math.Max(84, TextRenderer.MeasureText(text, b.Font).Width + 16), 26);
            b.Click += onClick;
            Controls.Add(b);
        }

        private void Fill()
        {
            _list.BeginUpdate();
            _list.Items.Clear();
            foreach (DuplicateTarget t in _targets)
            {
                var it = new ListViewItem(t.Bank);
                it.SubItems.Add("");
                it.SubItems.Add("");
                it.SubItems.Add("");
                it.Tag = t;
                Describe(it);
                _list.Items.Add(it);
            }
            _list.EndUpdate();
            if (_list.Items.Count > 0) _list.Items[0].Selected = true;
            UpdateButtons();
        }

        /// <summary>Refresh a row from its target, re-reading the source so the note
        /// reflects what is actually in that file right now.</summary>
        private void Describe(ListViewItem it)
        {
            var t = (DuplicateTarget)it.Tag;
            it.SubItems[1].Text = t.IndexList();
            it.SubItems[2].Text = t.SourcePath;

            string err;
            int untouched = t.CountUntouched(out err);
            string note;
            if (untouched < 0)
            {
                note = "cannot read: " + (err ?? "unknown error");
                t.Selected = false;
            }
            else if (untouched == t.Items.Count)
            {
                note = SameFile(t.SourcePath, t.DestPath) ? "rewritten in place" : "writes " + Path.GetFileName(t.DestPath);
                if (!SameFile(t.SourcePath, t.DestPath) && File.Exists(t.DestPath)) note += " (overwrites the copy there)";
            }
            else
            {
                note = (t.Items.Count - untouched) + " of " + t.Items.Count + " already changed in this file";
                t.Selected = false;
            }
            it.SubItems[3].Text = note;
            it.Checked = t.Selected;
            it.ForeColor = untouched < 0 ? SystemColors.GrayText : SystemColors.WindowText;
        }

        private static bool SameFile(string a, string b)
        {
            try { return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        }

        private void UpdateButtons()
        {
            _btnSource.Enabled = _list.SelectedItems.Count == 1;
            int n = 0;
            foreach (DuplicateTarget t in _targets) if (t.Selected) n++;
            _btnWrite.Enabled = n > 0;
            _btnWrite.Text = n > 0 ? "Write " + n + " bank" + (n == 1 ? "" : "s") : "Write ticked banks";
        }

        private void ChangeSource()
        {
            if (_list.SelectedItems.Count != 1) return;
            ListViewItem it = _list.SelectedItems[0];
            var t = (DuplicateTarget)it.Tag;

            using (var d = new OpenFileDialog())
            {
                d.Title = "Bank to start the " + t.Bank + " rewrite from";
                d.Filter = "PSX sound banks (*.vab)|*.vab|All files (*.*)|*.*";
                d.FileName = Path.GetFileName(t.SourcePath);
                string dir = Path.GetDirectoryName(t.SourcePath);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) d.InitialDirectory = dir;
                else if (!string.IsNullOrEmpty(_lastDir) && Directory.Exists(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                string stem = Path.GetFileNameWithoutExtension(d.FileName);
                if (!string.Equals(stem, t.Bank, StringComparison.OrdinalIgnoreCase))
                {
                    // The sample indices were found in THIS bank's layout, so a file
                    // for a different bank would put the sound into an unrelated slot.
                    if (MessageBox.Show(this,
                            "That file is named " + stem + ", not " + t.Bank + ". The sample numbers " +
                            "were found in " + t.Bank + " and may land on different sounds in another " +
                            "bank. Use it anyway?",
                            "Audio", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                        return;
                }

                t.SourcePath = d.FileName;
                t.Selected = true;
                _lastDir = Path.GetDirectoryName(d.FileName);
                Describe(it);
                UpdateButtons();
            }
        }
    }
}
