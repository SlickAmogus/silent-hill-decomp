using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Mod manager modal (opened from Form1's manager button). Lists texture mods
    /// (managed in place in gamedata/texturemods/) and load/FMV mods (the mods/
    /// library), lets the user enable/disable, set load order, drag-and-drop new
    /// mods in, and name/describe them, then commits it all on Apply (texture packs
    /// toggled in place; load/FMV deployed into the override dirs). Code-generated
    /// (no Designer), sharing Form1's ConfigManager so the allow_loose_files /
    /// texture_packs writes land in the same config.cfg.
    /// </summary>
    public class ModManagerForm : Form
    {
        private readonly ModManager _mgr;
        private readonly ConfigManager _config;
        private readonly string _gameRoot;

        private ListView  _list;
        private CheckBox  _chkLoose;
        private ToolTip   _btnTips;

        public ModManagerForm(ConfigManager config, string gameRoot)
        {
            _config   = config;
            _gameRoot = gameRoot;
            _mgr      = new ModManager(gameRoot, config);

            Text            = "Mod Manager";
            ClientSize      = new Size(600, 480);
            FormBorderStyle = FormBorderStyle.FixedDialog;
            StartPosition   = FormStartPosition.CenterParent;
            MaximizeBox     = false;
            MinimizeBox     = false;
            AllowDrop       = true;
            try { Icon = Properties.Resources.launchericon; } catch { }

            DragEnter += OnDragEnter;
            DragDrop  += OnDragDrop;

            BuildUi();
            _chkLoose.Checked = _config.Get("allow_loose_files", "0") == "1";

            // Extraction/indexing happens after the window is up (with a progress
            // window) so opening the manager never freezes on a big archive.
            Shown += OnShownInitialLoad;
        }

        private void OnShownInitialLoad(object sender, EventArgs e)
        {
            Shown -= OnShownInitialLoad;
            ExtractThenScan();
        }

        /// <summary>Extract any pending archives (with progress) then re-index and repopulate.</summary>
        private void ExtractThenScan()
        {
            if (_mgr.AnyPending())
                ProgressDialog.Run(this, "Extracting archives…", r => _mgr.Prepare(r));
            _mgr.Scan();
            Populate();
        }

        private void BuildUi()
        {
            var help = new Label
            {
                AutoSize = false,
                Location = new Point(12, 10),
                Size     = new Size(576, 34),
                Text     = "Drag mod folders or .zip / .rar archives here. Check to enable (Apply commits); " +
                           "top of the list = highest priority (wins conflicts). Right-click for name/notes or delete."
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
                AllowDrop     = true,
                ShowItemToolTips = true,
                HeaderStyle   = ColumnHeaderStyle.Nonclickable
            };
            _list.Columns.Add("Mod", 176);
            _list.Columns.Add("Type", 120);
            _list.Columns.Add("State", 78);
            _list.Columns.Add("Notes", 112);
            _list.DragEnter += OnDragEnter;
            _list.DragDrop  += OnDragDrop;
            _list.MouseDown += OnListMouseDown;
            _list.ItemCheck += OnItemCheck;
            Controls.Add(_list);

            _list.ContextMenuStrip = BuildContextMenu();

            var btnUp = new Button { Text = "Move Up",     Location = new Point(510, 50),  Size = new Size(78, 28) };
            var btnDn = new Button { Text = "Move Down",   Location = new Point(510, 82),  Size = new Size(78, 28) };
            var btnRe = new Button { Text = "Refresh",     Location = new Point(510, 122), Size = new Size(78, 28) };
            var btnOp = new Button { Text = "Open Folder", Location = new Point(510, 154), Size = new Size(78, 28) };
            btnUp.Click += (s, e) => MoveSelected(-1);
            btnDn.Click += (s, e) => MoveSelected(1);
            btnRe.Click += (s, e) => Rescan();
            btnOp.Click += (s, e) => _mgr.OpenModsFolder();
            Controls.Add(btnUp);
            Controls.Add(btnDn);
            Controls.Add(btnRe);
            Controls.Add(btnOp);

            // Asset tooling: unpack a disc image, and TIM -> PNG conversion (single
            // + recursive) for the loose-texture workflow. Drag a .bin onto the
            // window for the same extract flow (OnDragDrop).
            var btnEx = new Button { Text = "Extract BIN…", Location = new Point(510, 194), Size = new Size(78, 28) };
            var btnTp = new Button { Text = "TIM → PNG…",   Location = new Point(510, 226), Size = new Size(78, 28) };
            var btnBp = new Button { Text = "Bulk → PNG…",  Location = new Point(510, 258), Size = new Size(78, 28) };
            _btnTips = new ToolTip();
            _btnTips.SetToolTip(btnEx, "Unpack a Silent Hill .bin disc image into the loose asset tree.");
            _btnTips.SetToolTip(btnTp, "Convert individual .TIM texture files to .png.");
            _btnTips.SetToolTip(btnBp, "Recursively convert every .TIM under a folder to .png in place.");
            btnEx.Click += (s, e) => OnExtractBin();
            btnTp.Click += (s, e) => OnConvertTim();
            btnBp.Click += (s, e) => OnBulkPng();
            Controls.Add(btnEx);
            Controls.Add(btnTp);
            Controls.Add(btnBp);

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

        private ContextMenuStrip BuildContextMenu()
        {
            var menu = new ContextMenuStrip();
            menu.Items.Add("Edit name && notes…", null, (s, e) => EditSelected());
            menu.Items.Add("Open containing folder", null, (s, e) =>
            {
                var m = SelectedMod();
                if (m != null && m.Source == ModSource.TextureMods) _mgr.OpenTextureModsFolder();
                else _mgr.OpenModsFolder();
            });
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add("Delete mod…", null, (s, e) => RemoveSelected());
            menu.Opening += (s, e) =>
            {
                // Only meaningful with a real mod row selected.
                if (SelectedMod() == null) e.Cancel = true;
            };
            return menu;
        }

        private void Populate()
        {
            _list.BeginUpdate();
            _list.Items.Clear();
            foreach (var m in _mgr.Mods)
            {
                var item = new ListViewItem(m.Label) { Checked = m.Enabled, Tag = m };
                item.SubItems.Add(m.TypeLabel);
                item.SubItems.Add(m.StateLabel);
                item.SubItems.Add(m.Description ?? "");
                if (!string.IsNullOrEmpty(m.Description)) item.ToolTipText = m.Description;
                if (m.Type == ModType.Unknown) item.ForeColor = Color.Gray;
                _list.Items.Add(item);
            }
            _list.EndUpdate();
            if (_mgr.Mods.Count == 0)
            {
                var hint = new ListViewItem("(no mods yet — drag folders/.zip/.rar here or click Open Folder)");
                hint.SubItems.Add(""); hint.SubItems.Add(""); hint.SubItems.Add("");
                hint.ForeColor = Color.Gray;
                _list.Items.Add(hint);
            }
        }

        /// <summary>Block toggling the placeholder hint row (no backing mod).</summary>
        private void OnItemCheck(object sender, ItemCheckEventArgs e)
        {
            if (e.NewValue != CheckState.Checked) return;
            if (!(_list.Items[e.Index].Tag is ModEntry)) e.NewValue = CheckState.Unchecked;
        }

        private ModEntry SelectedMod()
        {
            if (_list.SelectedItems.Count == 0) return null;
            return _list.SelectedItems[0].Tag as ModEntry;
        }

        private void OnListMouseDown(object sender, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Right) return;
            var hit = _list.HitTest(e.Location);
            if (hit.Item != null) { hit.Item.Selected = true; hit.Item.Focused = true; }
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

        /// <summary>Persist the current UI state, then extract/re-index the library from disk.</summary>
        private void Rescan()
        {
            CommitOrderAndState();
            _mgr.SaveState();
            ExtractThenScan();
        }

        private void EditSelected()
        {
            var m = SelectedMod();
            if (m == null) return;
            if (PromptNameNotes(m))
            {
                _mgr.SaveState();
                // Refresh just the edited row.
                var it = _list.SelectedItems.Count > 0 ? _list.SelectedItems[0] : null;
                if (it != null)
                {
                    it.Text = m.Label;
                    it.SubItems[3].Text = m.Description ?? "";
                    it.ToolTipText = m.Description ?? "";
                }
            }
        }

        private void RemoveSelected()
        {
            var m = SelectedMod();
            if (m == null) return;

            string where = m.Source == ModSource.TextureMods
                ? "This permanently deletes it from gamedata\\texturemods."
                : "This permanently deletes it from the mods folder (it stays deployed until you Apply).";
            if (MessageBox.Show(this,
                    "Delete \"" + m.Label + "\"?\n\n" + where,
                    "Delete Mod", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                return;

            CommitOrderAndState();
            if (m.Source == ModSource.TextureMods) _mgr.DeleteTexture(m);
            else _mgr.DeleteLibrary(m);
            _mgr.SaveState();
            Populate();
        }

        /// <summary>Small inline dialog: friendly name + multiline notes for a mod.</summary>
        private bool PromptNameNotes(ModEntry m)
        {
            using (var dlg = new Form())
            {
                dlg.Text            = "Edit Mod";
                dlg.ClientSize      = new Size(380, 230);
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition   = FormStartPosition.CenterParent;
                dlg.MaximizeBox     = false;
                dlg.MinimizeBox     = false;

                dlg.Controls.Add(new Label { Text = "Folder: " + m.Name, Location = new Point(12, 10),
                                             AutoSize = true, ForeColor = Color.Gray });
                dlg.Controls.Add(new Label { Text = "Display name:", Location = new Point(12, 36), AutoSize = true });
                var txtName = new TextBox { Location = new Point(12, 54), Size = new Size(356, 22),
                                            Text = m.DisplayName ?? "" };
                dlg.Controls.Add(txtName);

                dlg.Controls.Add(new Label { Text = "Notes / description:", Location = new Point(12, 84), AutoSize = true });
                var txtDesc = new TextBox { Location = new Point(12, 102), Size = new Size(356, 78),
                                            Multiline = true, ScrollBars = ScrollBars.Vertical,
                                            Text = m.Description ?? "" };
                dlg.Controls.Add(txtDesc);

                var ok     = new Button { Text = "OK",     Location = new Point(196, 192), Size = new Size(80, 28),
                                          DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel", Location = new Point(288, 192), Size = new Size(80, 28),
                                          DialogResult = DialogResult.Cancel };
                dlg.Controls.Add(ok);
                dlg.Controls.Add(cancel);
                dlg.AcceptButton = ok;
                dlg.CancelButton = cancel;

                if (dlg.ShowDialog(this) != DialogResult.OK) return false;
                m.DisplayName = txtName.Text.Trim();
                m.Description = txtDesc.Text.Trim();
                return true;
            }
        }

        // --- drag & drop ------------------------------------------------------

        private void OnDragEnter(object sender, DragEventArgs e)
        {
            e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
        }

        private void OnDragDrop(object sender, DragEventArgs e)
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            var paths = (string[])e.Data.GetData(DataFormats.FileDrop);

            // A dropped disc image goes to the extractor, not the mod library.
            var bins = paths.Where(p => File.Exists(p) &&
                           Path.GetExtension(p).Equals(".bin", StringComparison.OrdinalIgnoreCase)).ToList();
            foreach (var bin in bins)
            {
                if (MessageBox.Show(this, "Extract \"" + Path.GetFileName(bin) + "\"?",
                        "Extract BIN", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
                    RunExtract(bin);
            }

            var others = paths.Where(p => !bins.Contains(p)).ToArray();
            if (others.Length == 0) return;

            CommitOrderAndState();
            _mgr.SaveState();

            int imported = 0;
            ProgressDialog.Run(this, "Importing mods…", r =>
            {
                foreach (var p in others)
                    if (_mgr.Import(p, r) == ModManager.ImportResult.Added) imported++;
                _mgr.Prepare(r); // extract any .zip (library) / .rar (texture) we just imported
            });

            _mgr.Scan();
            Populate();

            if (imported == 0)
                MessageBox.Show(this, "Nothing added. Drop a disc .bin to extract, or mod folders / .zip / .rar archives.",
                    "Mod Manager", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        // --- asset extraction / TIM conversion --------------------------------

        /// <summary>"Extract BIN…" button: browse for a disc image (defaults to gamedata), then extract.</summary>
        private void OnExtractBin()
        {
            string gamedata = Path.Combine(_gameRoot, "gamedata");
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a Silent Hill disc image";
                ofd.Filter = "Disc image (*.bin)|*.bin|All files (*.*)|*.*";
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                RunExtract(ofd.FileName);
            }
        }

        /// <summary>Prompt for an output folder + PNG option, then extract with progress.</summary>
        private void RunExtract(string binPath)
        {
            string outDir;
            bool convertPng;
            bool deleteTim;
            if (!PromptExtractOptions(binPath, out outDir, out convertPng, out deleteTim)) return;

            try { Directory.CreateDirectory(outDir); }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Cannot create the output folder:\n\n" + ex.Message,
                    "Extract BIN", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            BinExtractor.ExtractResult res = null;
            try
            {
                ProgressDialog.Run(this, "Extracting " + Path.GetFileName(binPath) + "…",
                    r => { res = BinExtractor.Extract(binPath, outDir, convertPng, deleteTim, r); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Extraction failed:\n\n" + ex.Message,
                    "Extract BIN", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !res.Ok)
            {
                MessageBox.Show(this, "Extraction failed:\n\n" + (res != null ? res.Error : "unknown error"),
                    "Extract BIN", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = "Extracted " + res.Files + " files";
            if (!string.IsNullOrEmpty(res.ReleaseId)) msg += " from " + res.ReleaseId;
            msg += ".\n";
            if (convertPng)
            {
                msg += "Converted " + res.Textures + " textures to PNG.\n";
                if (deleteTim) msg += "Deleted " + res.TexturesDeleted + " original .TIM file(s).\n";
            }
            msg += "\nOutput folder:\n" + outDir;
            if (res.Warnings.Count > 0)
                msg += "\n\nWarnings (" + res.Warnings.Count + "):\n - " +
                       string.Join("\n - ", res.Warnings.Take(8)) +
                       (res.Warnings.Count > 8 ? "\n - …" : "");

            MessageBox.Show(this, msg, "Extract BIN", MessageBoxButtons.OK,
                res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        }

        /// <summary>Modal: choose where to extract + whether to also dump textures as PNG.</summary>
        private bool PromptExtractOptions(string binPath, out string outDir, out bool convertPng, out bool deleteTim)
        {
            outDir = null; convertPng = false; deleteTim = false;
            string defaultOut = Path.Combine(
                Path.GetDirectoryName(binPath) ?? _gameRoot,
                Path.GetFileNameWithoutExtension(binPath) + "_extracted");

            using (var dlg = new Form())
            {
                dlg.Text            = "Extract Disc Image";
                dlg.ClientSize      = new Size(460, 172);
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition   = FormStartPosition.CenterParent;
                dlg.MaximizeBox     = false;
                dlg.MinimizeBox     = false;

                dlg.Controls.Add(new Label { Text = "Source: " + Path.GetFileName(binPath),
                                             Location = new Point(12, 12), AutoSize = true, ForeColor = Color.Gray });
                dlg.Controls.Add(new Label { Text = "Extract to:", Location = new Point(12, 42), AutoSize = true });
                var txtOut = new TextBox { Location = new Point(12, 60), Size = new Size(346, 22), Text = defaultOut };
                var btnBrowse = new Button { Text = "Browse…", Location = new Point(364, 59), Size = new Size(84, 24) };
                btnBrowse.Click += (s, e) =>
                {
                    using (var fbd = new FolderBrowserDialog())
                    {
                        fbd.Description = "Choose the folder to extract into";
                        if (Directory.Exists(txtOut.Text)) fbd.SelectedPath = txtOut.Text;
                        if (fbd.ShowDialog(dlg) == DialogResult.OK)
                            txtOut.Text = Path.Combine(fbd.SelectedPath, Path.GetFileNameWithoutExtension(binPath) + "_extracted");
                    }
                };
                dlg.Controls.Add(txtOut);
                dlg.Controls.Add(btnBrowse);

                var chk = new CheckBox { Text = "Convert textures (TIM) to PNG", Location = new Point(12, 96), AutoSize = true };
                var chkDel = new CheckBox { Text = "Delete original TIM?", Location = new Point(250, 96), AutoSize = true, Enabled = false };
                // "Delete original" only makes sense when converting; keep it gated + reset.
                chk.CheckedChanged += (s, e) =>
                {
                    chkDel.Enabled = chk.Checked;
                    if (!chk.Checked) chkDel.Checked = false;
                };
                dlg.Controls.Add(chk);
                dlg.Controls.Add(chkDel);

                var ok     = new Button { Text = "Extract", Location = new Point(276, 132), Size = new Size(84, 28), DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel",  Location = new Point(364, 132), Size = new Size(84, 28), DialogResult = DialogResult.Cancel };
                dlg.Controls.Add(ok);
                dlg.Controls.Add(cancel);
                dlg.AcceptButton = ok;
                dlg.CancelButton = cancel;

                if (dlg.ShowDialog(this) != DialogResult.OK) return false;
                outDir = txtOut.Text.Trim();
                convertPng = chk.Checked;
                deleteTim = chk.Checked && chkDel.Checked;
                return !string.IsNullOrEmpty(outDir);
            }
        }

        /// <summary>"TIM → PNG…" button: convert one or more .TIM files to .png beside them.</summary>
        private void OnConvertTim()
        {
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select TIM texture(s) to convert";
                ofd.Filter = "TIM textures (*.tim)|*.tim|All files (*.*)|*.*";
                ofd.Multiselect = true;
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;

                int ok = 0;
                var failures = new System.Collections.Generic.List<string>();
                foreach (var tim in ofd.FileNames)
                {
                    string png = Path.Combine(Path.GetDirectoryName(tim),
                                              Path.GetFileNameWithoutExtension(tim) + ".png");
                    string err;
                    if (TimConverter.ConvertFileToPng(tim, png, out err)) ok++;
                    else failures.Add(Path.GetFileName(tim) + ": " + err);
                }

                string msg = "Converted " + ok + " of " + ofd.FileNames.Length + " file(s) to PNG.";
                if (failures.Count > 0) msg += "\n\nFailed:\n - " + string.Join("\n - ", failures.Take(8));
                MessageBox.Show(this, msg, "TIM → PNG", MessageBoxButtons.OK,
                    failures.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
        }

        /// <summary>"Bulk → PNG…" button: recursively convert every .TIM under a folder, in place.</summary>
        private void OnBulkPng()
        {
            string folder;
            using (var fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Choose a folder — every .TIM under it is converted to .png";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) fbd.SelectedPath = gamedata;
                if (fbd.ShowDialog(this) != DialogResult.OK) return;
                folder = fbd.SelectedPath;
            }

            var del = MessageBox.Show(this,
                "Delete each original .TIM after it is converted?\n\n" +
                "Yes = convert then delete the .TIM (keep only the .png)\n" +
                "No  = keep both the .TIM and the new .png",
                "Bulk → PNG", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (del == DialogResult.Cancel) return;
            bool deleteOriginals = del == DialogResult.Yes;

            TimConverter.BulkResult res = null;
            try
            {
                ProgressDialog.Run(this, "Converting textures…",
                    r => { res = TimConverter.BulkConvert(folder, deleteOriginals, r); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Conversion failed:\n\n" + ex.Message,
                    "Bulk → PNG", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = "Converted " + res.Converted + " TIM file(s) to PNG.";
            if (deleteOriginals) msg += "\nDeleted " + res.Deleted + " original .TIM file(s).";
            if (res.Failed > 0)
                msg += "\n\nFailed (" + res.Failed + "):\n - " + string.Join("\n - ", res.Failures.Take(8)) +
                       (res.Failures.Count > 8 ? "\n - …" : "");
            MessageBox.Show(this, msg, "Bulk → PNG", MessageBoxButtons.OK,
                res.Failed > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        }

        private void OnApply(object sender, EventArgs e)
        {
            CommitOrderAndState();
            try
            {
                ModManager.ApplyResult r = null;
                ProgressDialog.Run(this, "Applying mods…", rep => { r = _mgr.Apply(_chkLoose.Checked, rep); });
                _chkLoose.Checked = r.LooseEnabled;
                Populate(); // reflect enable/disable state changes

                string msg = string.Format(
                    "Applied.\n\nActive texture packs: {0}\nLoad-folder mods: {1}\nFMV mods: {2}\n" +
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
