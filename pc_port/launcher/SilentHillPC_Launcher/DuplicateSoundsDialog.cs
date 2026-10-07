/* SPDX-License-Identifier: GPL-3.0-or-later */
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>The Audio tool's save step. One row per bank that will be written: the
    /// bank being edited first, then every other bank carrying byte-identical copies
    /// of the samples just replaced. Each row has a checkbox and a Source, the file
    /// the rewrite starts from.
    ///
    /// The source is what makes repeat edits work. A modder who replaced the Groaner
    /// in eight banks last week and now wants a new footstep opens the PRISTINE bank
    /// (an edited copy no longer matches anything, so it cannot reveal duplicates),
    /// and would then have to overwrite last week's work. Defaulting each row's source
    /// to the copy already in the destination folder turns the save into a merge.</summary>
    internal sealed class DuplicateSoundsDialog : Form
    {
        private readonly ListView _list = new ListView();
        private readonly Label _intro = new Label();
        private readonly TextBox _folder = new TextBox();
        private readonly Button _btnBrowse = new Button();
        private readonly Button _btnSource = new Button();
        private readonly Button _btnWrite = new Button();
        private readonly Button _btnCancel = new Button();
        private readonly List<DuplicateTarget> _targets;
        private readonly string _loadSnd;
        private string _lastDir;

        public string Folder { get { return _folder.Text.Trim(); } }

        public DuplicateSoundsDialog(string editedBank, List<DuplicateTarget> targets, string initialFolder, string loadSnd)
        {
            _targets = targets;
            _loadSnd = loadSnd;
            _lastDir = initialFolder;

            Text = Loc.F("Audio — save {0}", editedBank);
            ClientSize = new Size(780, 440);
            MinimumSize = new Size(640, 340);
            StartPosition = FormStartPosition.CenterParent;
            MaximizeBox = false;
            MinimizeBox = false;
            ShowInTaskbar = false;
            try { Icon = Properties.Resources.launchericon; } catch { }

            _intro.Location = new Point(12, 12);
            _intro.Size = new Size(756, 50);
            _intro.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            _intro.Text = targets.Count > 1
                ? Loc.T("The game loads one ambient bank per map, and the disc copies the same sound into " +
                        "every bank that needs it. The samples you replaced also exist, byte for byte, in the " +
                        "other banks below; a replacement only plays where the loaded bank has it. Each bank is " +
                        "written into the folder as <bank>.VAB, starting from its Source. A copy already in that " +
                        "folder is used as the source automatically, so earlier edits are kept.")
                : Loc.F("The bank is written into the folder as {0}.VAB, starting from its Source. " +
                        "A copy already in that folder is used as the source automatically, so earlier edits " +
                        "to it are kept. No other bank on the disc carries the samples you replaced.", editedBank);
            Controls.Add(_intro);

            var fl = new Label
            {
                Text = Loc.T("Save into:"),
                Location = new Point(12, 72),
                Size = new Size(70, 20),
                TextAlign = ContentAlignment.MiddleLeft
            };
            fl.Width = Math.Max(fl.Width, fl.PreferredWidth);
            Controls.Add(fl);
            _folder.Location = new Point(Math.Max(84, fl.Right + 2), 70);
            _folder.Size = new Size(674 - _folder.Left, 22);
            _folder.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            _folder.Text = initialFolder ?? "";
            _folder.TextChanged += (s, e) => Retarget();
            Controls.Add(_folder);
            _btnBrowse.Text = Loc.T("Browse…");
            _btnBrowse.Location = new Point(684, 68);
            _btnBrowse.Size = new Size(84, 26);
            _btnBrowse.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _btnBrowse.Click += (s, e) => BrowseFolder();
            Controls.Add(_btnBrowse);

            _list.View = View.Details;
            _list.CheckBoxes = true;
            _list.FullRowSelect = true;
            _list.HideSelection = false;
            _list.GridLines = true;
            _list.MultiSelect = false;
            _list.Location = new Point(12, 104);
            _list.Size = new Size(756, 284);
            _list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;
            _list.Columns.Add(Loc.T("Bank"), 130, HorizontalAlignment.Left);
            _list.Columns.Add(Loc.T("Samples"), 100, HorizontalAlignment.Left);
            _list.Columns.Add(Loc.T("Source"), 300, HorizontalAlignment.Left);
            _list.Columns.Add(Loc.T("Note"), 210, HorizontalAlignment.Left);
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.ItemCheck += (s, e) =>
            {
                // The edited bank is the point of the save; it cannot be unticked.
                if (((DuplicateTarget)_list.Items[e.Index].Tag).IsPrimary) e.NewValue = CheckState.Checked;
            };
            _list.ItemChecked += (s, e) => { ((DuplicateTarget)e.Item.Tag).Selected = e.Item.Checked; UpdateButtons(); };
            Controls.Add(_list);

            int y = 400;
            SetupButton(_btnSource, Loc.T("Change source…"), new Point(12, y), (s, e) => ChangeSource());
            _btnSource.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            SetupButton(_btnWrite, Loc.T("Save"), new Point(560, y), (s, e) => Accept());
            _btnWrite.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            SetupButton(_btnCancel, Loc.T("Cancel"), new Point(684, y), (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
            _btnCancel.Left = 768 - _btnCancel.Width;

            // The intro is three lines of English; a translation that wraps further pushes
            // everything below it down rather than being clipped.
            int introH = TextRenderer.MeasureText(_intro.Text, _intro.Font, new Size(_intro.Width, 0),
                                                  TextFormatFlags.WordBreak).Height;
            if (introH > _intro.Height)
            {
                int grow = introH - _intro.Height;
                _intro.Height = introH;
                foreach (Control c in new Control[] { fl, _folder, _btnBrowse, _list }) c.Top += grow;
                foreach (Control c in new Control[] { _btnSource, _btnWrite, _btnCancel }) c.Top += grow;
                ClientSize = new Size(ClientSize.Width, ClientSize.Height + grow);
            }
            _btnCancel.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            _btnCancel.DialogResult = DialogResult.Cancel;
            CancelButton = _btnCancel;

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
                var it = new ListViewItem(t.IsPrimary ? t.Bank + "  " + Loc.T("(this file)") : t.Bank);
                it.SubItems.Add("");
                it.SubItems.Add("");
                it.SubItems.Add("");
                it.Tag = t;
                if (t.IsPrimary) it.Font = new Font(_list.Font, FontStyle.Bold);
                _list.Items.Add(it);
            }
            _list.EndUpdate();
            Retarget();
            if (_list.Items.Count > 0) _list.Items[0].Selected = true;
        }

        /// <summary>Point every row at the current folder: destination always, source
        /// unless the user picked one by hand.</summary>
        private void Retarget()
        {
            string folder = Folder;
            bool ok = folder.Length > 0;
            try { if (ok) Path.GetFullPath(folder); } catch { ok = false; }

            foreach (ListViewItem it in _list.Items)
            {
                var t = (DuplicateTarget)it.Tag;
                string file = t.Bank + ".VAB";
                t.DestPath = ok ? Path.Combine(folder, file) : null;
                if (!t.SourcePinned)
                {
                    string inFolder = ok ? Path.Combine(folder, file) : null;
                    string inLoad = string.IsNullOrEmpty(_loadSnd) ? null : Path.Combine(_loadSnd, file);
                    if (inFolder != null && File.Exists(inFolder)) t.SourcePath = inFolder;
                    else if (!t.IsPrimary && inLoad != null && File.Exists(inLoad)) t.SourcePath = inLoad;
                    else t.SourcePath = t.OriginalPath;
                }
                Describe(it);
            }
            UpdateButtons();
        }

        /// <summary>Refresh a row from its target, re-reading the source so the note
        /// reflects what is actually in that file right now.</summary>
        private void Describe(ListViewItem it)
        {
            var t = (DuplicateTarget)it.Tag;
            it.SubItems[1].Text = t.IndexList();
            it.SubItems[2].Text = t.SourcePath ?? "";

            string err;
            int sameEdit;
            int untouched = t.Classify(t.IsPrimary ? null : PrimaryKeys(), out sameEdit, out err);
            int changed = untouched < 0 ? 0 : t.Items.Count - untouched;
            int different = changed - sameEdit;
            string note;
            bool inPlace = t.DestPath != null && SameFile(t.SourcePath, t.DestPath);
            bool destExists = t.DestPath != null && File.Exists(t.DestPath);

            if (t.DestPath == null)
            {
                note = Loc.T("choose a folder");
            }
            else if (untouched < 0)
            {
                note = Loc.F("cannot read: {0}", err ?? Loc.T("unknown error"));
                if (!t.IsPrimary) t.Selected = false;
            }
            else if (t.IsPrimary)
            {
                note = Loc.T(inPlace ? "merged into the existing file"
                           : destExists ? "overwrites the copy there" : "new file");
                if (changed > 0) note += ", " + Loc.F("{0} earlier edit(s) replaced", changed);
            }
            else if (different == 0)
            {
                note = Loc.T(inPlace ? "merged into the existing file"
                           : destExists ? "overwrites the copy there" : "new file");
                if (sameEdit > 0) note += ", " + Loc.F("same earlier edit as {0} replaced", _targets[0].Bank);
                t.Selected = true;
            }
            else
            {
                // Neither the disc's sound nor the edited bank's: a deliberate
                // different replacement, so it is the user's call to overwrite it.
                note = Loc.F("{0} of {1} hold a different edit, left alone unless ticked", different, t.Items.Count);
                t.Selected = false;
            }

            it.SubItems[3].Text = note;
            it.Checked = t.IsPrimary || t.Selected;
            it.ForeColor = untouched < 0 ? SystemColors.GrayText : SystemColors.WindowText;
        }

        private Dictionary<int, string> PrimaryKeys()
        {
            DuplicateTarget p = _targets.Count > 0 && _targets[0].IsPrimary ? _targets[0] : null;
            return p == null || string.IsNullOrEmpty(p.SourcePath) ? null : p.CurrentKeys();
        }

        private static bool SameFile(string a, string b)
        {
            if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) return false;
            try { return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        }

        private void UpdateButtons()
        {
            _btnSource.Enabled = _list.SelectedItems.Count == 1;
            int n = 0;
            foreach (DuplicateTarget t in _targets) if (t.Selected) n++;
            bool folderOk = Folder.Length > 0;
            _btnWrite.Enabled = folderOk && n > 0;
            _btnWrite.Text = n > 1 ? Loc.F("Save {0} banks", n) : Loc.T("Save");
            int right = _btnWrite.Right;
            _btnWrite.Width = Math.Max(_btnWrite.Width, TextRenderer.MeasureText(_btnWrite.Text, _btnWrite.Font).Width + 16);
            _btnWrite.Left = right - _btnWrite.Width;
        }

        private void Accept()
        {
            string folder = Folder;
            try { Path.GetFullPath(folder); }
            catch
            {
                MessageBox.Show(this, Loc.T("That is not a usable folder path."), Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            foreach (DuplicateTarget t in _targets)
            {
                if (!t.Selected) continue;
                string err;
                int sameEdit;
                if (t.Classify(null, out sameEdit, out err) < 0)
                {
                    MessageBox.Show(this, Loc.F("{0}: the source cannot be read.\n\n{1}", t.Bank, err),
                        Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
            }
            DialogResult = DialogResult.OK;
            Close();
        }

        private void BrowseFolder()
        {
            using (var d = new FolderBrowserDialog())
            {
                d.Description = Loc.T("Folder to save the banks into (the game reads gamedata\\load\\SND)");
                string cur = Folder;
                if (cur.Length > 0 && Directory.Exists(cur)) d.SelectedPath = cur;
                else if (!string.IsNullOrEmpty(_lastDir) && Directory.Exists(_lastDir)) d.SelectedPath = _lastDir;
                if (d.ShowDialog(this) == DialogResult.OK) _folder.Text = d.SelectedPath;
            }
        }

        private void ChangeSource()
        {
            if (_list.SelectedItems.Count != 1) return;
            ListViewItem it = _list.SelectedItems[0];
            var t = (DuplicateTarget)it.Tag;

            using (var d = new OpenFileDialog())
            {
                d.Title = Loc.F("Bank to start the {0} rewrite from", t.Bank);
                d.Filter = Loc.T("PSX sound banks") + " (*.vab)|*.vab|" + Loc.T("All files") + " (*.*)|*.*";
                d.FileName = Path.GetFileName(t.SourcePath ?? (t.Bank + ".VAB"));
                string dir = string.IsNullOrEmpty(t.SourcePath) ? null : Path.GetDirectoryName(t.SourcePath);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) d.InitialDirectory = dir;
                else if (!string.IsNullOrEmpty(_lastDir) && Directory.Exists(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                string stem = Path.GetFileNameWithoutExtension(d.FileName);
                if (!string.Equals(stem, t.Bank, StringComparison.OrdinalIgnoreCase))
                {
                    // The sample indices were found in THIS bank's layout, so a file
                    // for a different bank would put the sound into an unrelated slot.
                    if (MessageBox.Show(this,
                            Loc.F("That file is named {0}, not {1}. The sample numbers " +
                                  "belong to {1} and may land on different sounds in another " +
                                  "bank. Use it anyway?", stem, t.Bank),
                            Loc.T("Audio"), MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                        return;
                }

                t.SourcePath = d.FileName;
                t.SourcePinned = true;
                t.Selected = true;
                _lastDir = Path.GetDirectoryName(d.FileName);
                Describe(it);
                UpdateButtons();
            }
        }
    }
}
