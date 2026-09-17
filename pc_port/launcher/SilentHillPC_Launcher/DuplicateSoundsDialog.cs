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

            Text = "Audio — save " + editedBank;
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
                ? "The game loads one ambient bank per map, and the disc copies the same sound into " +
                  "every bank that needs it. The samples you replaced also exist, byte for byte, in the " +
                  "other banks below; a replacement only plays where the loaded bank has it. Each bank is " +
                  "written into the folder as <bank>.VAB, starting from its Source. A copy already in that " +
                  "folder is used as the source automatically, so earlier edits are kept."
                : "The bank is written into the folder as " + editedBank + ".VAB, starting from its Source. " +
                  "A copy already in that folder is used as the source automatically, so earlier edits " +
                  "to it are kept. No other bank on the disc carries the samples you replaced.";
            Controls.Add(_intro);

            var fl = new Label
            {
                Text = "Save into:",
                Location = new Point(12, 72),
                Size = new Size(70, 20),
                TextAlign = ContentAlignment.MiddleLeft
            };
            Controls.Add(fl);
            _folder.Location = new Point(84, 70);
            _folder.Size = new Size(590, 22);
            _folder.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            _folder.Text = initialFolder ?? "";
            _folder.TextChanged += (s, e) => Retarget();
            Controls.Add(_folder);
            _btnBrowse.Text = "Browse…";
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
            _list.Columns.Add("Bank", 130, HorizontalAlignment.Left);
            _list.Columns.Add("Samples", 100, HorizontalAlignment.Left);
            _list.Columns.Add("Source", 300, HorizontalAlignment.Left);
            _list.Columns.Add("Note", 210, HorizontalAlignment.Left);
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.ItemCheck += (s, e) =>
            {
                // The edited bank is the point of the save; it cannot be unticked.
                if (((DuplicateTarget)_list.Items[e.Index].Tag).IsPrimary) e.NewValue = CheckState.Checked;
            };
            _list.ItemChecked += (s, e) => { ((DuplicateTarget)e.Item.Tag).Selected = e.Item.Checked; UpdateButtons(); };
            Controls.Add(_list);

            int y = 400;
            SetupButton(_btnSource, "Change source…", new Point(12, y), (s, e) => ChangeSource());
            _btnSource.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            SetupButton(_btnWrite, "Save", new Point(560, y), (s, e) => Accept());
            _btnWrite.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            SetupButton(_btnCancel, "Cancel", new Point(684, y), (s, e) => { DialogResult = DialogResult.Cancel; Close(); });
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
                var it = new ListViewItem(t.IsPrimary ? t.Bank + "  (this file)" : t.Bank);
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
            int untouched = t.CountUntouched(out err);
            int changed = untouched < 0 ? 0 : t.Items.Count - untouched;
            string note;
            bool inPlace = t.DestPath != null && SameFile(t.SourcePath, t.DestPath);
            bool destExists = t.DestPath != null && File.Exists(t.DestPath);

            if (t.DestPath == null)
            {
                note = "choose a folder";
            }
            else if (untouched < 0)
            {
                note = "cannot read: " + (err ?? "unknown error");
                if (!t.IsPrimary) t.Selected = false;
            }
            else if (t.IsPrimary)
            {
                note = inPlace ? "merged into the existing file"
                     : destExists ? "overwrites the copy there" : "new file";
                if (changed > 0) note += ", " + changed + " earlier edit" + (changed == 1 ? "" : "s") + " replaced";
            }
            else if (changed == 0)
            {
                note = inPlace ? "merged into the existing file"
                     : destExists ? "overwrites the copy there" : "new file";
            }
            else
            {
                // A source already edited at these very samples most likely holds a
                // deliberate different sound; leave that to the user to tick.
                note = changed + " of " + t.Items.Count + " already changed in the source";
                t.Selected = false;
            }

            it.SubItems[3].Text = note;
            it.Checked = t.IsPrimary || t.Selected;
            it.ForeColor = untouched < 0 ? SystemColors.GrayText : SystemColors.WindowText;
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
            _btnWrite.Text = n > 1 ? "Save " + n + " banks" : "Save";
        }

        private void Accept()
        {
            string folder = Folder;
            try { Path.GetFullPath(folder); }
            catch
            {
                MessageBox.Show(this, "That is not a usable folder path.", "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            foreach (DuplicateTarget t in _targets)
            {
                if (!t.Selected) continue;
                string err;
                if (t.CountUntouched(out err) < 0)
                {
                    MessageBox.Show(this, t.Bank + ": the source cannot be read.\n\n" + err,
                        "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
                d.Description = "Folder to save the banks into (the game reads gamedata\\load\\SND)";
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
                d.Title = "Bank to start the " + t.Bank + " rewrite from";
                d.Filter = "PSX sound banks (*.vab)|*.vab|All files (*.*)|*.*";
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
                            "That file is named " + stem + ", not " + t.Bank + ". The sample numbers " +
                            "belong to " + t.Bank + " and may land on different sounds in another " +
                            "bank. Use it anyway?",
                            "Audio", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
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
