using System;
using System.Collections.Generic;
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
            ClientSize      = new Size(600, 572);
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
                Size          = new Size(490, 428),
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
            var btnRef = new Button { Text = "Reference…",  Location = new Point(510, 290), Size = new Size(78, 28) };
            var btnReb = new Button { Text = "Rebuild…",    Location = new Point(510, 322), Size = new Size(78, 28) };
            var btnMo = new Button { Text = "Model → OBJ…", Location = new Point(510, 354), Size = new Size(78, 28) };
            var btnOm = new Button { Text = "OBJ → Model…", Location = new Point(510, 386), Size = new Size(78, 28) };
            var btnVw = new Button { Text = "View Model…",  Location = new Point(510, 418), Size = new Size(78, 28) };
            var btnHelp = new Button { Text = "Help…",      Location = new Point(510, 450), Size = new Size(78, 28) };
            _btnTips = new ToolTip();
            _btnTips.SetToolTip(btnEx, "Unpack a Silent Hill .bin disc image into the loose asset tree.");
            _btnTips.SetToolTip(btnTp, "Convert individual .TIM texture files to .png.");
            _btnTips.SetToolTip(btnBp, "Recursively convert every .TIM under a folder to .png in place.");
            _btnTips.SetToolTip(btnRef, "Build Reference: a character draws its regions through several CLUT palettes, so no " +
                "single .TIM/pNN PNG looks right. This reads the .ILM model + .TIM and assembles ONE correct composite " +
                "image (how it really looks in-game) for you to paint over.");
            _btnTips.SetToolTip(btnReb, "Rebuild Textures: slice your edited reference image back into the per-row " +
                "NAME.TIM.pNN.png files the game loads (gamedata/load/<FOLDER>/). No 16-colour-per-region limit — paint freely.");
            _btnTips.SetToolTip(btnMo, "Model → OBJ: write a character model out as a .obj (plus .mtl and .ilmmeta.json) you can " +
                "open in Blender. Each 'o' object is ONE rigid animated body part — reshape its vertices freely, but do NOT " +
                "rename, add or remove objects: that list is the rig, and changing it breaks the animation.");
            _btnTips.SetToolTip(btnOm, "OBJ → Model: fold your .obj back into a new .ILM. Needs the ORIGINAL .ILM it came " +
                "from plus the .ilmmeta.json written beside the .obj. It asks first: Edit existing (reshape the current " +
                "character — patches in place, grows, or rebuilds within the vertex limit) or High-poly replace (a v7 " +
                "model with NO vertex cap, for swapping in a whole new mesh like a ripped model). Either way the object " +
                "list must still match the original's parts — parts are bones.");
            _btnTips.SetToolTip(btnVw, "View Model: preview a .ILM (or an edited .obj before importing it) in a 3D window — " +
                "textured with its real in-game palettes. Drag to orbit, right-drag to pan, wheel to zoom.");
            _btnTips.SetToolTip(btnHelp, "How to make and install loose-file texture mods.");
            btnEx.Click += (s, e) => OnExtractBin();
            btnTp.Click += (s, e) => OnConvertTim();
            btnBp.Click += (s, e) => OnBulkPng();
            btnRef.Click += (s, e) => OnBuildReference();
            btnReb.Click += (s, e) => OnRebuildTextures();
            btnMo.Click += (s, e) => OnExportModel();
            btnOm.Click += (s, e) => OnImportModel();
            btnVw.Click += (s, e) => OnViewModel();
            btnHelp.Click += (s, e) => ShowLooseModHelp();
            Controls.Add(btnEx);
            Controls.Add(btnTp);
            Controls.Add(btnBp);
            Controls.Add(btnRef);
            Controls.Add(btnReb);
            Controls.Add(btnMo);
            Controls.Add(btnOm);
            Controls.Add(btnVw);
            Controls.Add(btnHelp);

            _chkLoose = new CheckBox
            {
                Text     = "Enable loose file support (required for load-folder mods)",
                Location = new Point(12, 486),
                AutoSize = true
            };
            Controls.Add(_chkLoose);

            var btnApply = new Button { Text = "Apply", Location = new Point(414, 532), Size = new Size(84, 30) };
            var btnClose = new Button { Text = "Close", Location = new Point(504, 532), Size = new Size(84, 30) };
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
            bool buildRefs;
            if (!PromptExtractOptions(binPath, out outDir, out convertPng, out deleteTim, out buildRefs)) return;

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

            ClutComposer.ComposeAllResult refRes = null;
            if (buildRefs)
            {
                try
                {
                    ProgressDialog.Run(this, "Building character reference composites…",
                        r => { refRes = ClutComposer.ComposeAll(outDir, r); });
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Extraction finished, but building reference composites failed:\n\n" + ex.Message,
                        "Extract BIN", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                }
            }

            string msg = "Extracted " + res.Files + " files";
            if (!string.IsNullOrEmpty(res.ReleaseId)) msg += " from " + res.ReleaseId;
            msg += ".\n";
            if (convertPng)
            {
                msg += "Converted " + res.Textures + " textures to PNG.\n";
                if (deleteTim) msg += "Deleted " + res.TexturesDeleted + " original .TIM file(s).\n";
            }
            if (buildRefs && refRes != null)
                msg += "Built " + refRes.Made + " character reference composite(s).\n";
            msg += "\nOutput folder:\n" + outDir;
            if (res.Warnings.Count > 0)
                msg += "\n\nWarnings (" + res.Warnings.Count + "):\n - " +
                       string.Join("\n - ", res.Warnings.Take(8)) +
                       (res.Warnings.Count > 8 ? "\n - …" : "");

            MessageBox.Show(this, msg, "Extract BIN", MessageBoxButtons.OK,
                res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
        }

        /// <summary>Modal: choose where to extract + whether to also dump textures as PNG.</summary>
        private bool PromptExtractOptions(string binPath, out string outDir, out bool convertPng, out bool deleteTim, out bool buildRefs)
        {
            outDir = null; convertPng = false; deleteTim = false; buildRefs = false;
            string defaultOut = Path.Combine(
                Path.GetDirectoryName(binPath) ?? _gameRoot,
                Path.GetFileNameWithoutExtension(binPath) + "_extracted");

            using (var dlg = new Form())
            {
                dlg.Text            = "Extract Disc Image";
                dlg.ClientSize      = new Size(460, 200);
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
                var chkRef = new CheckBox { Text = "Build character reference composites (.ILM + .TIM)",
                                           Location = new Point(12, 122), AutoSize = true };
                dlg.Controls.Add(chk);
                dlg.Controls.Add(chkDel);
                dlg.Controls.Add(chkRef);

                var ok     = new Button { Text = "Extract", Location = new Point(276, 160), Size = new Size(84, 28), DialogResult = DialogResult.OK };
                var cancel = new Button { Text = "Cancel",  Location = new Point(364, 160), Size = new Size(84, 28), DialogResult = DialogResult.Cancel };
                dlg.Controls.Add(ok);
                dlg.Controls.Add(cancel);
                dlg.AcceptButton = ok;
                dlg.CancelButton = cancel;

                if (dlg.ShowDialog(this) != DialogResult.OK) return false;
                outDir = txtOut.Text.Trim();
                convertPng = chk.Checked;
                deleteTim = chk.Checked && chkDel.Checked;
                buildRefs = chkRef.Checked;
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

                int ok = 0, pngs = 0;
                var failures = new System.Collections.Generic.List<string>();
                foreach (var tim in ofd.FileNames)
                {
                    string err;
                    var written = TimConverter.ConvertFileToPngSet(tim, out err);
                    if (written.Count > 0) { ok++; pngs += written.Count; }
                    else failures.Add(Path.GetFileName(tim) + ": " + err);
                }

                string msg = "Converted " + ok + " of " + ofd.FileNames.Length + " file(s) to "
                             + pngs + " PNG(s).\n(Multi-palette textures emit one PNG per palette row, "
                             + "e.g. DOB.TIM.p00.png … — edit the row for the body region you want.)";
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

        /// <summary>"Reference…" button: compose a character's true in-game look from
        /// its .ILM model + .TIM into one editable PNG.</summary>
        private void OnBuildReference()
        {
            string ilm;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a character model (.ILM)";
                ofd.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }

            string tim = FindTimBeside(ilm);
            if (tim == null)
            {
                using (var ofd = new OpenFileDialog())
                {
                    ofd.Title = "Select the matching .TIM texture for " + Path.GetFileName(ilm);
                    ofd.Filter = "TIM textures (*.tim)|*.tim|All files (*.*)|*.*";
                    ofd.InitialDirectory = Path.GetDirectoryName(ilm);
                    if (ofd.ShowDialog(this) != DialogResult.OK) return;
                    tim = ofd.FileName;
                }
            }

            string outPng;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save reference image";
                sfd.Filter = "PNG image (*.png)|*.png";
                sfd.InitialDirectory = Path.GetDirectoryName(ilm);
                sfd.FileName = Path.GetFileNameWithoutExtension(ilm) + "_reference.png";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                outPng = sfd.FileName;
            }

            bool ok = false; string err = null;
            try
            {
                ProgressDialog.Run(this, "Building reference…",
                    r => { ok = ClutComposer.Compose(ilm, tim, outPng, out err); });
            }
            catch (Exception ex) { err = ex.Message; ok = false; }

            if (!ok)
            {
                MessageBox.Show(this, "Could not build the reference:\n\n" + err,
                    "Build Reference", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (MessageBox.Show(this,
                    "Reference image written:\n" + outPng +
                    "\n\nEdit it in any image editor (keep the same pixel size), then use \"Rebuild…\" " +
                    "to turn it back into the per-row PNGs the game loads.\n\nOpen it now?",
                    "Build Reference", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outPng); } catch { }
            }
        }

        /// <summary>"Rebuild…" button: slice an edited reference PNG back into the
        /// per-row NAME.TIM.pNN.png loose-override set.</summary>
        private void OnRebuildTextures()
        {
            string edited;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select your edited reference image (.png)";
                ofd.Filter = "PNG image (*.png)|*.png|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                edited = ofd.FileName;
            }

            string ilm = GuessIlmFor(edited);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select the character model (.ILM) this image came from";
                ofd.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                if (ilm != null) { ofd.InitialDirectory = Path.GetDirectoryName(ilm); ofd.FileName = Path.GetFileName(ilm); }
                else ofd.InitialDirectory = Path.GetDirectoryName(edited);
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }

            string tim = FindTimBeside(ilm);
            if (tim == null)
            {
                using (var ofd = new OpenFileDialog())
                {
                    ofd.Title = "Select the matching .TIM texture for " + Path.GetFileName(ilm);
                    ofd.Filter = "TIM textures (*.tim)|*.tim|All files (*.*)|*.*";
                    ofd.InitialDirectory = Path.GetDirectoryName(ilm);
                    if (ofd.ShowDialog(this) != DialogResult.OK) return;
                    tim = ofd.FileName;
                }
            }

            string outDir;
            using (var fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Output folder for the per-row PNGs (usually gamedata/load/CHARA)";
                string load = Path.Combine(_gameRoot, "gamedata", "load");
                if (Directory.Exists(load)) fbd.SelectedPath = load;
                else if (Directory.Exists(Path.Combine(_gameRoot, "gamedata")))
                    fbd.SelectedPath = Path.Combine(_gameRoot, "gamedata");
                if (fbd.ShowDialog(this) != DialogResult.OK) return;
                outDir = fbd.SelectedPath;
            }

            ClutComposer.SplitResult res = null;
            try
            {
                ProgressDialog.Run(this, "Rebuilding textures…",
                    r => { res = ClutComposer.Split(edited, ilm, tim, outDir); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Rebuild failed:\n\n" + ex.Message,
                    "Rebuild Textures", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(this, "Rebuild failed:\n\n" + (res != null ? res.Error : "unknown error"),
                    "Rebuild Textures", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = "Wrote " + res.Written.Count + " per-row PNG(s) to:\n" + outDir +
                "\n\nPalette rows this model uses: " + string.Join(", ", res.RowsUsed) +
                "\n\nDrop these into gamedata/load/<FOLDER>/ (e.g. CHARA) and set " +
                "allow_loose_files = 1 in config.cfg.";
            if (MessageBox.Show(this, msg + "\n\nOpen the output folder?",
                    "Rebuild Textures", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outDir); } catch { }
            }
        }

        /// <summary>"Model → OBJ…" button: write a character model out as an editable
        /// .obj + .mtl + .ilmmeta.json set.</summary>
        private void OnExportModel()
        {
            string ilm;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a character model (.ILM)";
                ofd.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }

            string outObj;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save model as OBJ";
                sfd.Filter = "Wavefront OBJ (*.obj)|*.obj|All files (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(ilm);
                sfd.FileName = Path.GetFileNameWithoutExtension(ilm) + ".obj";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                outObj = sfd.FileName;
            }

            IlmObjConverter.ExportResult res = null;
            try
            {
                ProgressDialog.Run(this, "Exporting model…",
                    r => { res = IlmObjConverter.Export(ilm, outObj); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Export failed:\n\n" + ex.Message,
                    "Model → OBJ", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                MessageBox.Show(this, "Export failed:\n\n" + (res != null ? res.Error : "unknown error"),
                    "Model → OBJ", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Paint the real in-game texture onto the MTL so the mesh shows textured in
            // Blender: the raw exporter writes a colourless MTL because a character's
            // texture is a paletted CLUT composite, not a plain image. Import reads only
            // the OBJ + .ilmmeta.json and never the MTL, so a map_Kd here cannot affect
            // the round-trip. Silently skipped when no .TIM sits beside the .ILM.
            string texNote = TextureExportedObj(ilm, res.ObjPath, res.MtlPath);

            string msg = "Wrote " + res.Parts + " body part(s), " + res.Vertices + " vertices, " +
                res.Prims + " face(s) and " + res.Materials + " material(s):\n" + res.ObjPath +
                "\n\nBeside it: " + Path.GetFileName(res.MtlPath) + " and " + Path.GetFileName(res.MetaPath) +
                " — keep the .ilmmeta.json, \"OBJ → Model…\" needs it.\n\n" +
                "In Blender, each object is one rigid animated body part: move and reshape " +
                "vertices freely, but do not rename, add or remove objects.";
            if (texNote != null) msg += "\n\n" + texNote;
            if (res.Dangling > 0)
                msg += "\n\nNote: " + res.Dangling + " face corner(s) point at a vertex outside their own part, " +
                       "so the OBJ substitutes that part's first vertex there — those few corners look wrong in " +
                       "Blender and are restored on import.";
            // res.Warnings carries the exporter's rest-pose diagnosis: when res.AnmName is null it explains
            // that every part is in its own local space and will pile on the origin in Blender — a failure the
            // user cannot diagnose from the model itself, so it must not be swallowed by the success dialog.
            if (res.Warnings.Count > 0)
                msg += "\n\nWarnings (" + res.Warnings.Count + "):\n - " +
                       string.Join("\n - ", res.Warnings.Take(8)) +
                       (res.Warnings.Count > 8 ? "\n - …" : "");
            bool warn = res.Dangling > 0 || res.Warnings.Count > 0;
            if (MessageBox.Show(this, msg + "\n\nOpen the output folder?",
                    "Model → OBJ", MessageBoxButtons.YesNo,
                    warn ? MessageBoxIcon.Warning : MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.ObjPath)); } catch { }
            }
        }

        /// <summary>"OBJ → Model…" button: fold an edited .obj back into a new .ILM,
        /// using the original .ILM plus the .ilmmeta.json written beside the .obj.</summary>
        private void OnImportModel()
        {
            var mode = MessageBox.Show(this,
                "Is this a full model REPLACEMENT — swapping in a different character or mesh (e.g. a ripped model)?\n\n" +
                "• Yes — High-poly replace: rebuilds as a v7 model with NO vertex-count limit. Use this for a new model.\n" +
                "• No — Edit existing: reshape the current character (auto patch / grow / replace; vertex-limited).\n\n" +
                "Either way the OBJ's object list must still match the original's parts — parts are bones.",
                "OBJ → Model", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
            if (mode == DialogResult.Cancel) return;
            bool v7 = (mode == DialogResult.Yes);

            string obj;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select your edited model (.obj)";
                ofd.Filter = "Wavefront OBJ (*.obj)|*.obj|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                obj = ofd.FileName;
            }

            string ilm = GuessIlmFor(obj);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select the ORIGINAL model (.ILM) this OBJ was exported from";
                ofd.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                if (ilm != null) { ofd.InitialDirectory = Path.GetDirectoryName(ilm); ofd.FileName = Path.GetFileName(ilm); }
                else ofd.InitialDirectory = Path.GetDirectoryName(obj);
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                ilm = ofd.FileName;
            }

            string outIlm;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save the rebuilt model";
                sfd.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                sfd.InitialDirectory = Path.GetDirectoryName(obj);
                sfd.FileName = Path.GetFileNameWithoutExtension(ilm) + "_new.ILM";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                outIlm = sfd.FileName;
            }

            // High-poly (v7) replacement is a self-contained path: it rebuilds the mesh with no
            // vertex cap and never patches/grows/welds, so it does not share the escalation below.
            if (v7)
            {
                string tmpV7 = outIlm + ".rebuild.tmp";
                IlmObjConverter.ImportResult vres = null;
                try
                {
                    ProgressDialog.Run(this, "Rebuilding high-poly model…",
                        r => { vres = IlmObjConverter.Import(obj, ilm, tmpV7,
                            new IlmObjConverter.ImportOptions { V7 = true }); });
                }
                catch (Exception ex)
                {
                    TryDelete(tmpV7);
                    MessageBox.Show(this, "High-poly rebuild failed:\n\n" + ex.Message,
                        "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                if (vres == null || !string.IsNullOrEmpty(vres.Error))
                {
                    TryDelete(tmpV7);
                    MessageBox.Show(this, "High-poly rebuild failed:\n\n" + (vres != null ? vres.Error : "unknown error"),
                        "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                try
                {
                    if (File.Exists(outIlm)) File.Replace(tmpV7, outIlm, null);
                    else                     File.Move(tmpV7, outIlm);
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "The model rebuilt, but it could not be moved to:\n" + outIlm +
                        "\n\n" + ex.Message + "\n\nThe rebuilt model is here:\n" + tmpV7,
                        "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                vres.IlmPath = outIlm;

                string vmsg = "Rebuilt a HIGH-POLY (v7) model: " + vres.Parts + " part(s), " + vres.Vertices +
                    " vertices, " + vres.Prims + " face(s):\n" + vres.IlmPath +
                    "\n\nDrop it into gamedata\\load\\<FOLDER>\\ under the ORIGINAL name (e.g. " +
                    "gamedata\\load\\CHARA\\HERO.ILM) and set allow_loose_files = 1.\n\n" +
                    "v7 is a PC-port high-poly format: this build loads it, but original hardware / a vanilla decomp " +
                    "build would not.";
                if (vres.Warnings.Count > 0)
                    vmsg += "\n\nWarnings (" + vres.Warnings.Count + "):\n - " +
                            string.Join("\n - ", vres.Warnings.Take(10)) +
                            (vres.Warnings.Count > 10 ? "\n - …" : "");
                var vicon = vres.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information;
                if (MessageBox.Show(this, vmsg + "\n\nOpen the output folder?", "OBJ → Model",
                        MessageBoxButtons.YesNo, vicon) == DialogResult.Yes)
                {
                    try { System.Diagnostics.Process.Start(Path.GetDirectoryName(vres.IlmPath)); } catch { }
                }
                return;
            }

            // Grow-mode is passed unconditionally rather than exposed as a checkbox. The converter
            // falls through to the byte-identical patch-in-place path whenever the OBJ added
            // nothing, so the flag alone can never alter a same-topology rebuild — which means a
            // checkbox would only add two ways to be surprised (forget to tick it and a legitimate
            // grow is rejected as a vertex-count mismatch; tick it on an unchanged mesh and nothing
            // happens for reasons the user cannot see). Detection is exact instead of guessed, and
            // the confirmation below quotes the converter's own per-part deltas.
            //
            // Full replacement is offered the same way and for the same reason, one rung further
            // down: it is tried only after this call has refused, so the safest expressible path
            // always wins and a rebuild is never the quiet answer to a small edit.
            //
            // It builds to a sibling temp file: a refusal (or a declined confirmation) must not
            // leave a half-written or unwanted .ILM at the path the user picked.
            string tmpIlm = outIlm + ".rebuild.tmp";
            IlmObjConverter.ImportResult res = null;
            try
            {
                ProgressDialog.Run(this, "Rebuilding model…",
                    r => { res = IlmObjConverter.Import(obj, ilm, tmpIlm, true); });
            }
            catch (Exception ex)
            {
                TryDelete(tmpIlm);
                MessageBox.Show(this, "Rebuild failed:\n\n" + ex.Message,
                    "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res == null || !string.IsNullOrEmpty(res.Error))
            {
                IlmObjConverter.ImportResult diag = PreferPlainDiagnosis(res, obj, ilm, tmpIlm);
                // Escalate to a full rebuild only after BOTH cheaper paths have refused, and only
                // when they refused because the OBJ's topology no longer matches the template —
                // which is the one thing a rebuild fixes. Anything else (a renamed part, a missing
                // .ilmmeta.json) would refuse identically, so offering a rebuild would just be a
                // second dead end. Ordering the paths cheapest-first is what stops a user who only
                // reshaped a mesh from ever being handed a rebuild: patch-in-place is byte-exact,
                // grow preserves the original geometry and its sharing graph, and only a change
                // neither can express reaches the rebuild — which then still has to be confirmed.
                bool topologyChanged = (res != null && res.ReplaceMayHelp) ||
                                       (diag != null && diag.ReplaceMayHelp);
                IlmObjConverter.ImportResult rep = topologyChanged ? TryReplace(obj, ilm, tmpIlm) : null;
                if (rep == null || !string.IsNullOrEmpty(rep.Error))
                {
                    TryDelete(tmpIlm);
                    ShowImportFailure(diag, rep);
                    return;
                }
                if (!ConfirmReplace(rep, diag)) { TryDelete(tmpIlm); return; }
                res = rep;
            }
            else if (res.Grew && !ConfirmGrow(res)) { TryDelete(tmpIlm); return; }

            try
            {
                // Replace, never delete-then-move: Delete and Move are not atomic, so a Move that
                // loses a race with an AV scanner (or the preview window's own handle on the file
                // just written) after the Delete succeeded leaves NEITHER model on disk. Rebuilding
                // straight onto an existing destination is the documented workflow, not an edge case.
                if (File.Exists(outIlm)) File.Replace(tmpIlm, outIlm, null);
                else                     File.Move(tmpIlm, outIlm);
            }
            catch (Exception ex)
            {
                // The temp is the ONLY copy of the rebuild and the destination may still hold the
                // user's previous model - deleting either here loses work.
                MessageBox.Show(this, "The model rebuilt, but it could not be moved to:\n" + outIlm +
                    "\n\n" + ex.Message + "\n\nThe rebuilt model is here:\n" + tmpIlm,
                    "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            res.IlmPath = outIlm;

            if (res.Replaced)
            {
                ShowReplaceReport(res);
                if (MessageBox.Show(this, "Open the output folder?", "OBJ → Model",
                        MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }
            else if (res.Grew)
            {
                ShowGrowReport(res);
                if (MessageBox.Show(this, "Open the output folder?", "OBJ → Model",
                        MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }
            else
            {
                string msg = "Rebuilt " + res.Parts + " body part(s), " + res.Vertices + " vertices, " +
                    res.Normals + " normals and " + res.Prims + " face(s):\n" + res.IlmPath +
                    "\n\nTo use it, drop it into gamedata\\load\\<FOLDER>\\ under its ORIGINAL name " +
                    "(e.g. gamedata\\load\\CHARA\\DOB.ILM) and set allow_loose_files = 1 in config.cfg.";
                // Same contract as the export dialog: res.Warnings carries seam-edit diagnoses the user
                // cannot see in the written file, so a success box must not swallow them.
                if (res.Warnings.Count > 0)
                    msg += "\n\nWarnings (" + res.Warnings.Count + "):\n - " +
                           string.Join("\n - ", res.Warnings.Take(8)) +
                           (res.Warnings.Count > 8 ? "\n - …" : "");
                if (MessageBox.Show(this, msg + "\n\nOpen the output folder?",
                        "OBJ → Model", MessageBoxButtons.YesNo,
                        res.Warnings.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information) != DialogResult.Yes)
                    return;
            }

            try { System.Diagnostics.Process.Start(Path.GetDirectoryName(res.IlmPath)); } catch { }
        }

        /// <summary>Delete a scratch file, ignoring anything that goes wrong — the caller is
        /// already on an error path and the failure it is reporting is the interesting one.</summary>
        private static void TryDelete(string path)
        {
            try { if (File.Exists(path)) File.Delete(path); } catch { }
        }

        /// <summary>Pick the refusal that names the right part. Grow-detection fires on ANY extra
        /// vertex or face, so an accidental topology change — one stray vertex, a face duplicated by
        /// a mirror or Ctrl+D — is diagnosed by GrowImport's face/vertex pairing check instead of the
        /// plain path's count check. Adding a vertex shifts every later global OBJ index, so the
        /// pairing error blames whichever part the shift lands in, which is almost never the part the
        /// user edited. The plain path's count check names the right part and the right cause, so when
        /// the growth was not deliberate its refusal is the one to show. A ceiling refusal (Report
        /// filled) is already the accurate diagnosis of a REAL grow and must survive untouched.</summary>
        private static IlmObjConverter.ImportResult PreferPlainDiagnosis(
            IlmObjConverter.ImportResult grown, string obj, string ilm, string tmpIlm)
        {
            if (grown == null || grown.Report.Count > 0) return grown;
            IlmObjConverter.ImportResult plain = IlmObjConverter.Import(obj, ilm, tmpIlm, false);
            TryDelete(tmpIlm);
            return (plain != null && !string.IsNullOrEmpty(plain.Error)) ? plain : grown;
        }

        /// <summary>Run the full-rebuild path into the same temp file. Auto-weld needs a rest pose;
        /// when the OBJ was exported without one the converter says so and the user is asked once,
        /// because that model has no other route to a rebuild — and a rebuild without welding is a
        /// real answer, not a workaround: the joints then stay closed on the parts' own overlap,
        /// which is how a model authored with overlapping limbs already works.</summary>
        private IlmObjConverter.ImportResult TryReplace(string obj, string ilm, string tmpIlm)
        {
            var opts = new IlmObjConverter.ImportOptions { Replace = true };
            IlmObjConverter.ImportResult rep = RunReplace(obj, ilm, tmpIlm, opts);
            if (rep == null || string.IsNullOrEmpty(rep.Error) || !rep.WeldNeedsRestPose) return rep;

            string msg = "This model was exported without its animation file, so every body part " +
                "sits on the origin in the .obj rather than where it belongs on the character.\n\n" +
                "Joints can therefore not be found automatically: two vertices being in the same " +
                "place no longer means they meet at a joint, and welding them would fuse parts " +
                "that are nowhere near each other on the real model.\n\n" +
                "Rebuild WITHOUT welding? Each part then carries its own seam vertices, so the " +
                "joints stay closed only while the parts' geometry overlaps — which is fine if you " +
                "modelled them overlapping, and leaves visible gaps if you did not.\n\n" +
                "(The alternative is to answer No, re-export the model with \"Model → OBJ…\" from " +
                "a copy that has its ANIM folder beside it, and redo the edit.)";
            if (MessageBox.Show(this, msg, "OBJ → Model", MessageBoxButtons.YesNo,
                    MessageBoxIcon.Warning) != DialogResult.Yes)
                return rep;

            opts.Weld = false;
            return RunReplace(obj, ilm, tmpIlm, opts);
        }

        /// <summary>Anything the converter throws OUTSIDE its own guards (an unreadable path, a
        /// full disk) comes back as an Error like any refusal, so one caller reports one failure.</summary>
        private IlmObjConverter.ImportResult RunReplace(string obj, string ilm, string tmpIlm,
            IlmObjConverter.ImportOptions opts)
        {
            IlmObjConverter.ImportResult rep = null;
            try
            {
                ProgressDialog.Run(this, "Rebuilding model…",
                    r => { rep = IlmObjConverter.Import(obj, ilm, tmpIlm, opts); });
            }
            catch (Exception ex)
            {
                rep = new IlmObjConverter.ImportResult();
                rep.Error = ex.Message;
            }
            return rep;
        }

        /// <summary>Ask before committing a full rebuild. Everything the modeller needs to judge it
        /// is in here, not in the success dialog: which parts moved somewhere unexpected, which
        /// vertices changed BONE, and how close the seams that did not weld came. Those are the
        /// signals that say "you got it wrong", and a dialog that shows them only after the file is
        /// written is a dialog that shows them too late.</summary>
        private bool ConfirmReplace(IlmObjConverter.ImportResult res, IlmObjConverter.ImportResult why)
        {
            var lines = new System.Collections.Generic.List<string>();
            lines.Add("REBUILD THE WHOLE MODEL?");
            lines.Add("");
            lines.Add("Your .obj no longer carries the same vertices and faces as the original, so it");
            lines.Add("cannot be folded back in piece by piece:");
            lines.Add("");
            foreach (string s in Wrap(why != null ? why.Error : "the geometry no longer matches", 76))
                lines.Add("    " + s);
            lines.Add("");
            lines.Add("It CAN be rebuilt from scratch. The original .ILM then supplies only the rig —");
            lines.Add("bone bindings, draw order, materials and palette rows — and every vertex, face,");
            lines.Add("normal and UV comes from your .obj. Part names and part count still may not");
            lines.Add("change: an 'o' object IS a bone.");
            lines.Add("");
            lines.Add("REBUILT");
            lines.Add("    " + res.Parts + " body part(s), " + res.Vertices + " vertices, " +
                      res.Normals + " normals, " + res.Prims + " face(s).");
            lines.Add("");

            if (res.Warnings.Count > 0)
            {
                lines.Add("!!  GEOMETRY IN AN UNEXPECTED PLACE  (" + res.Warnings.Count + ")");
                lines.Add("");
                foreach (string w in res.Warnings)
                {
                    foreach (string s in Wrap(w, 74)) lines.Add("    " + s);
                    lines.Add("");
                }
            }

            AddBoneChanges(res, lines);
            AddJoints(res, lines);
            lines.Add("");
            lines.Add("REBUILD REPORT");
            AddReport(res, lines);
            lines.Add("");
            lines.Add("Nothing has been written yet.");
            return ShowTextDialog("OBJ → Model — Rebuild?", lines.ToArray(), true, "Rebuild");
        }

        /// <summary>The welds that handed a vertex to a different BONE. A weld inside one bone is
        /// invisible at runtime; one that crosses a bone is the only kind that changes how the
        /// model animates, so it gets its own banner rather than a line buried in the transcript.</summary>
        private static void AddBoneChanges(IlmObjConverter.ImportResult res, System.Collections.Generic.List<string> lines)
        {
            if (res.CrossedBones.Count == 0) return;
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            lines.Add("!!  VERTICES THAT CHANGED BONE  (" + res.CrossedBones.Count + " part pair(s))");
            lines.Add("");
            foreach (string s in Wrap("These vertices were welded onto a part driven by a DIFFERENT bone, so " +
                "they will animate with that part from now on. If those two parts do not actually meet at a " +
                "joint, pull the vertices apart in your modeller and rebuild.", 74))
                lines.Add("    " + s);
            lines.Add("");
            foreach (IlmObjConverter.CrossedBoneInfo c in res.CrossedBones)
                lines.Add("    " + c.Count.ToString(inv).PadLeft(4) + " vertex/vertices of " + c.Reader.PadRight(10) +
                          " moved up to " + c.MaxDistance.ToString("0.000", inv) + " units onto " + c.Owner);
            lines.Add("");
        }

        private static void AddJoints(IlmObjConverter.ImportResult res, System.Collections.Generic.List<string> lines)
        {
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            lines.Add("JOINTS");
            lines.Add("");
            if (!res.WeldEnabled)
            {
                foreach (string s in Wrap("Welding is OFF, so every part keeps its own seam vertices. The " +
                    "joints stay closed only while the parts' geometry overlaps.", 74))
                    lines.Add("    " + s);
                return;
            }
            foreach (string s in Wrap("This format has no skinning and no weights: a seam stays closed only " +
                "when the later-drawn part reads the earlier one's vertex. Vertices of two parts that landed " +
                "in the same place (within " + res.WeldEps.ToString("0.####", inv) + " units) were welded " +
                "that way — " + res.WeldedVertices.ToString(inv) + " vertices across " +
                res.WeldPairs.Count.ToString(inv) + " part pair(s).", 74))
                lines.Add("    " + s);
            lines.Add("");
            foreach (IlmObjConverter.WeldPairInfo p in res.WeldPairs)
                lines.Add("    " + p.Reader.PadRight(10) + " reads " + p.Owner.PadRight(10) + "  " +
                          p.Count.ToString(inv));
            if (res.WeldPairs.Count == 0)
                lines.Add("    (none - no two parts had a vertex in the same place)");
            if (res.NearMissDistance >= 0.0)
            {
                lines.Add("");
                foreach (string s in Wrap("HINT: the closest pair of vertices in different parts that did NOT " +
                    "weld is " + res.NearMissDistance.ToString("0.000", inv) + " units apart. If those were " +
                    "meant to be one seam, snap them to exactly the same place in your modeller and rebuild — " +
                    "widening the radius instead risks welding geometry onto the wrong bone.", 74))
                    lines.Add("    " + s);
            }
        }

        /// <summary>The converter's own transcript. The monospace dialog turns word wrap OFF so its
        /// column-aligned tables line up, so the prose lines in here (HINT, NOTE) are wrapped by
        /// hand — otherwise they run off the right edge and are read by nobody.</summary>
        private static void AddReport(IlmObjConverter.ImportResult res, System.Collections.Generic.List<string> lines)
        {
            foreach (string raw in res.Report)
            {
                if (raw.Length <= 80) { lines.Add(raw); continue; }
                int indent = 0;
                while (indent < raw.Length && raw[indent] == ' ') indent++;
                string pad = new string(' ', indent);
                bool first = true;
                foreach (string s in Wrap(raw.Substring(indent), 78 - indent))
                {
                    lines.Add((first ? pad : pad + "  ") + s);
                    first = false;
                }
            }
        }

        /// <summary>Success dialog for a rebuilt model. Same install rules as a grown model (it is
        /// a full rewrite either way), and the same weld/bone signals as the confirmation, because
        /// this is the copy the user keeps on screen while checking the result in the viewer.</summary>
        private void ShowReplaceReport(IlmObjConverter.ImportResult res)
        {
            string name = Path.GetFileName(res.IlmPath);
            var lines = new System.Collections.Generic.List<string>();
            lines.Add("REBUILT FROM YOUR MESH");
            lines.Add("");
            lines.Add("Written:  " + res.IlmPath);
            lines.Add("Rebuilt " + res.Parts + " body part(s): " + res.Vertices + " vertices, " +
                      res.Normals + " normals, " + res.Prims + " face(s).");
            lines.Add("");
            lines.Add("INSTALLING IT  (both of these or it will not load)");
            lines.Add("");
            lines.Add("  1.  Tick \"Enable loose file support\" at the bottom of the Mod Manager");
            lines.Add("      (allow_loose_files = 1 in config.cfg). A rebuilt model is a full rewrite");
            lines.Add("      and is read ONLY through the loose-file path. Without it the game quietly");
            lines.Add("      keeps the original and nothing looks wrong.");
            lines.Add("");
            lines.Add("  2.  Copy it to   gamedata\\load\\<FOLDER>\\<ORIGINAL NAME>.ILM");
            lines.Add("      e.g.         gamedata\\load\\CHARA\\DOB.ILM");
            lines.Add("      Under the ORIGINAL name, in the folder it came from. The game looks the");
            lines.Add("      file up by that name — " + name + " will never be found.");
            lines.Add("");
            lines.Add("An enabled TEXTURE PACK is no obstacle: a pack's .png for this character");
            lines.Add("overrides that character's TEXTURE only and never interferes with the");
            lines.Add("loose .ILM.");
            lines.Add("");
            lines.Add("Check it before you install: \"View Model…\" opens the rebuilt .ILM.");
            lines.Add("");
            if (res.Warnings.Count > 0)
            {
                lines.Add("!!  GEOMETRY IN AN UNEXPECTED PLACE  (" + res.Warnings.Count + ")");
                lines.Add("");
                foreach (string w in res.Warnings)
                {
                    foreach (string s in Wrap(w, 74)) lines.Add("    " + s);
                    lines.Add("");
                }
            }
            AddBoneChanges(res, lines);
            AddJoints(res, lines);
            lines.Add("");
            lines.Add("REBUILD REPORT");
            AddReport(res, lines);
            ShowTextDialog("OBJ → Model — Rebuilt Model", lines.ToArray(), true);
        }

        /// <summary>Break a long message into fixed-width lines. The monospace dialog turns word
        /// wrap off so its tables line up, which would otherwise clip these sentences.</summary>
        private static string[] Wrap(string s, int width)
        {
            var outp = new System.Collections.Generic.List<string>();
            var line = new System.Text.StringBuilder();
            foreach (string w in s.Split(' '))
            {
                if (w.Length == 0) continue;
                if (line.Length != 0 && line.Length + 1 + w.Length > width)
                {
                    outp.Add(line.ToString());
                    line.Length = 0;
                }
                if (line.Length != 0) line.Append(' ');
                line.Append(w);
            }
            if (line.Length != 0) outp.Add(line.ToString());
            if (outp.Count == 0) outp.Add("");
            return outp.ToArray();
        }

        /// <summary>Import refusal. A ceiling refusal (too many vertices/normals/prims/parts for the
        /// file format's u8 slot indices) fills res.Report with the same budget table a success
        /// produces, so the user can see WHICH part blew WHICH limit — that only reads in the
        /// monospace dialog, so route it there and keep the MessageBox for plain failures.
        ///
        /// `rebuild` is the full-rebuild attempt, when one was made. Its refusal is reported too:
        /// having silently tried a second path and said nothing about why it also failed is how a
        /// user ends up believing the file is unfixable.</summary>
        private void ShowImportFailure(IlmObjConverter.ImportResult res, IlmObjConverter.ImportResult rebuild)
        {
            string err = res != null ? res.Error : "unknown error";
            string alsoTried = (rebuild != null && !string.IsNullOrEmpty(rebuild.Error))
                ? "\n\nRebuilding the model from scratch was tried as well, and refused too:\n\n" + rebuild.Error
                : null;
            if (res == null || res.Report.Count == 0)
            {
                MessageBox.Show(this, "Rebuild failed:\n\n" + err + alsoTried,
                    "OBJ → Model", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            var lines = new System.Collections.Generic.List<string>();
            lines.Add("REBUILD REFUSED — THE MODEL DOES NOT FIT");
            lines.Add("");
            lines.Add(err);
            lines.Add("");
            lines.Add("Nothing was written.");
            lines.Add("");
            lines.Add("A body part addresses its vertices, normals and faces with single bytes, so");
            lines.Add("the limits below are hard: no build of the game can load a part past them.");
            lines.Add("Remove geometry from the part named above — decimate it in Blender, or move");
            lines.Add("some of the detail onto a neighbouring part — and rebuild.");
            lines.Add("");
            lines.Add("BUDGET AT THE POINT OF REFUSAL");
            lines.AddRange(res.Report);
            if (alsoTried != null)
            {
                lines.Add("");
                lines.Add("A FULL REBUILD WAS TRIED TOO, AND REFUSED:");
                lines.Add("");
                foreach (string s in Wrap(rebuild.Error, 76)) lines.Add("  " + s);
            }
            ShowTextDialog("OBJ → Model — Refused", lines.ToArray(), true);
        }

        /// <summary>Ask before committing a grown model: it is a different KIND of output (needs
        /// loose-file support, cannot go back on a disc), and reaching it by accident — one stray
        /// subdivide in Blender — must not be silent. The counts come from the converter's own
        /// per-part delta lines, which are the first GrownParts.Count entries of Report.</summary>
        private bool ConfirmGrow(IlmObjConverter.ImportResult res)
        {
            int n = res.GrownParts.Count;
            string deltas = string.Join("\n", res.Report.Take(Math.Min(n, 12))) +
                            (n > 12 ? "\n   … and " + (n - 12) + " more (full table follows)" : "");
            string msg = "This OBJ ADDS geometry — the rebuilt model is larger than the original.\n\n" +
                "New geometry in " + n + " of " + res.Parts + " body part(s):\n" +
                deltas + "\n\n" +
                "A larger-than-original model loads only through this port's loose-file path with " +
                "\"Enable loose file support\" switched on. If you did not mean to add geometry, " +
                "answer No, undo the change in Blender and rebuild.\n\n" +
                "Rebuild as a larger-than-original model?";
            return MessageBox.Show(this, msg, "OBJ → Model", MessageBoxButtons.YesNo,
                       MessageBoxIcon.Question) == DialogResult.Yes;
        }

        /// <summary>Success dialog for a grown model. The budget table is column-aligned and far
        /// too long for a MessageBox, and the install rules are stricter than the plain path's, so
        /// this goes in the scrollable monospace dialog.</summary>
        private void ShowGrowReport(IlmObjConverter.ImportResult res)
        {
            string name = Path.GetFileName(res.IlmPath);
            var lines = new System.Collections.Generic.List<string>();
            lines.Add("REBUILT AS A LARGER-THAN-ORIGINAL MODEL");
            lines.Add("");
            lines.Add("Written:  " + res.IlmPath);
            lines.Add("Grew " + res.GrownParts.Count + " of " + res.Parts + " body part(s), adding " +
                      res.Prims + " new face(s).");
            lines.Add("");
            lines.Add("INSTALLING IT  (both of these or it will not load)");
            lines.Add("");
            lines.Add("  1.  Tick \"Enable loose file support\" at the bottom of the Mod Manager");
            lines.Add("      (allow_loose_files = 1 in config.cfg). A model bigger than the one on");
            lines.Add("      the disc is read ONLY through the loose-file path. Without it the game");
            lines.Add("      quietly keeps the original and nothing looks wrong.");
            lines.Add("");
            lines.Add("  2.  Copy it to   gamedata\\load\\<FOLDER>\\<ORIGINAL NAME>.ILM");
            lines.Add("      e.g.         gamedata\\load\\CHARA\\DOB.ILM");
            lines.Add("      Under the ORIGINAL name, in the folder it came from. The game looks the");
            lines.Add("      file up by that name — " + name + " will never be found.");
            lines.Add("");
            lines.Add("An enabled TEXTURE PACK is no obstacle: a pack's .png for this character");
            lines.Add("overrides that character's TEXTURE only and never interferes with the");
            lines.Add("loose .ILM.");
            if (res.Warnings.Count > 0)
            {
                lines.Add("");
                lines.Add("WARNINGS (" + res.Warnings.Count + ")");
                foreach (var w in res.Warnings) lines.Add("  - " + w);
            }
            lines.Add("");
            lines.Add("BUDGET  (per part: used / limit, and the pool window it occupies)");
            lines.AddRange(res.Report);
            ShowTextDialog("OBJ → Model — Grown Model", lines.ToArray(), true);
        }

        /// <summary>"View Model…" button: open a .ILM or an edited .obj in the
        /// software-rendered 3D preview window.</summary>
        private void OnViewModel()
        {
            string path;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a model to view (.ILM or .obj)";
                ofd.Filter = "Models (*.ilm;*.obj)|*.ilm;*.obj|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                path = ofd.FileName;
            }
            ModelViewerForm.Open(this, path);
        }

        /// <summary>Composite the character's true in-game texture (each region through its
        /// own CLUT palette row) to a PNG beside the OBJ, and point every material in the
        /// MTL at it via map_Kd, so the mesh shows textured in Blender instead of flat
        /// white. One image serves every material because the composite already bakes each
        /// region's palette — which is also exactly what a single loose-PNG override does
        /// in game (hires_override.c registers one image across all rows). Returns a
        /// one-line note for the success dialog, or null when there is nothing to do.</summary>
        private static string TextureExportedObj(string ilm, string objPath, string mtlPath)
        {
            try
            {
                string tim = FindTimBeside(ilm);
                if (tim == null || !File.Exists(mtlPath)) return null;
                string pngPath = Path.Combine(Path.GetDirectoryName(objPath),
                    Path.GetFileNameWithoutExtension(objPath) + "_texture.png");
                string err;
                if (!ClutComposer.Compose(ilm, tim, pngPath, out err)) return null;

                string pngRef = Path.GetFileName(pngPath);
                var outLines = new List<string>();
                bool wroteForBlock = false;
                foreach (string line in File.ReadAllLines(mtlPath))
                {
                    string t = line.TrimStart();
                    if (t.StartsWith("newmtl ", StringComparison.Ordinal)) wroteForBlock = false;
                    // Drop any stale map_Kd so re-exporting over an old MTL stays clean.
                    if (t.StartsWith("map_Kd ", StringComparison.Ordinal)) continue;
                    outLines.Add(line);
                    // The composite is opaque; a d/Tr line above would make Blender show it
                    // see-through, so add map_Kd right after Kd where neither is affected.
                    if (!wroteForBlock && t.StartsWith("Kd ", StringComparison.Ordinal))
                    {
                        outLines.Add("map_Kd " + pngRef);
                        wroteForBlock = true;
                    }
                }
                File.WriteAllLines(mtlPath, outLines.ToArray());
                return "Textured preview: " + pngRef + " is beside the OBJ and wired into the " +
                       "MTL, so the mesh shows its real in-game texture in Blender. (Import " +
                       "ignores the MTL — this is display only.)";
            }
            catch { return null; }  // a preview convenience must never fail the export
        }

        /// <summary>Find NAME.TIM beside NAME.ILM (either extension case).</summary>
        private static string FindTimBeside(string ilm)
        {
            string c = Path.ChangeExtension(ilm, ".TIM");
            if (File.Exists(c)) return c;
            c = Path.ChangeExtension(ilm, ".tim");
            return File.Exists(c) ? c : null;
        }

        /// <summary>Best-effort guess of the .ILM a reference PNG came from
        /// (NAME_reference.png -> NAME.ILM beside it).</summary>
        private static string GuessIlmFor(string png)
        {
            string dir = Path.GetDirectoryName(png);
            string stem = Path.GetFileNameWithoutExtension(png);
            if (stem.EndsWith("_reference", StringComparison.OrdinalIgnoreCase))
                stem = stem.Substring(0, stem.Length - "_reference".Length);
            foreach (var ext in new[] { ".ILM", ".ilm" })
            {
                string c = Path.Combine(dir ?? ".", stem + ext);
                if (File.Exists(c)) return c;
            }
            return null;
        }

        /// <summary>Help dialog: how to build and install loose-file texture mods.</summary>
        private void ShowLooseModHelp()
        {
            string[] lines =
            {
                "MAKING LOOSE-FILE TEXTURE MODS",
                "",
                "The game can load replacement textures from a \"loose\" folder — no disc",
                "rebuild needed. The whole workflow:",
                "",
                "1.  Extract the textures.",
                "    Click \"Extract BIN…\", pick your Silent Hill .bin, and tick",
                "    \"Convert textures to PNG\". You get the disc's folder tree with every",
                "    texture as a .png — for example the title screen is  TIM\\TITLE_E.png.",
                "",
                "    MONSTERS & CHARACTERS use ONE texture sheet with SEVERAL palettes —",
                "    the head, body and limbs are drawn with different palettes over the",
                "    same pixels. Those TIMs extract to one PNG PER PALETTE, named",
                "    DOB.TIM.p00.png, DOB.TIM.p01.png, …  (a single flat texture stays a",
                "    plain DOB.png). Edit the palette-row PNG for the region you want to",
                "    change. You can ship just the rows you edited, but KEEP p00.png —",
                "    the game uses it to detect a per-palette set (the extractor always",
                "    writes it).",
                "",
                "2.  Edit the ones you want.",
                "    Replace a .png with your own art, keeping the SAME name and folder.",
                "    Only the files you actually change need to ship.",
                "",
                "3.  Install it — either way works:",
                "",
                "    - Drop-in (quick):",
                "      The game loads loose assets from   gamedata\\load",
                "      Copy your edited folder there, mirroring the disc layout — e.g. a",
                "      custom title screen goes to   gamedata\\load\\TIM\\TITLE_E.png",
                "      (Tick \"Enable loose file support\" at the bottom of this window.)",
                "",
                "    - As a managed mod (recommended):",
                "      Make a folder named  load, put your TIM folder (etc.) inside it, and",
                "      zip it up. Drag the .zip onto this window to add it to the Mod",
                "      Manager. Now you can enable/disable it and set load order — when two",
                "      mods touch the same file, the one higher in the list wins.",
                "",
                "EDITING CHARACTER MODELS (advanced)",
                "",
                "    \"Model → OBJ…\" writes a model out as a .obj you can open in Blender,",
                "    along with a .mtl and a .ilmmeta.json. Keep that .ilmmeta.json — the",
                "    import step needs it.",
                "",
                "    Each 'o' object in the .obj is ONE rigid animated body part. Move and",
                "    reshape its vertices as much as you like, but do NOT rename, add or",
                "    remove objects: that list IS the rig, and changing it breaks every",
                "    animation the character has.",
                "",
                "    \"OBJ → Model…\" folds the edited .obj back into a new .ILM. It asks for",
                "    three things: your .obj, the ORIGINAL .ILM it came from, and where to",
                "    save — the .ilmmeta.json is picked up automatically from beside the",
                "    .obj. Bones, draw order and palette rows are copied from the original.",
                "",
                "    Install the result like any loose file, under its ORIGINAL name — e.g.",
                "    gamedata\\load\\CHARA\\DOB.ILM",
                "",
                "    ADDING geometry (bigger than the original model)",
                "",
                "    You are not limited to reshaping what is there. Add vertices and faces",
                "    inside an existing 'o' block — subdivide it, extrude it — and the import",
                "    rebuilds the model at its new size. It notices by itself: when the .obj",
                "    carries more geometry than it exported, it asks you to confirm, then",
                "    prints a per-part budget table. You never tick anything, and an .obj you",
                "    only reshaped still rebuilds exactly as it always did.",
                "",
                "    The limits, and they are hard — a part addresses its own vertices,",
                "    normals and faces with SINGLE BYTES:",
                "      - vertex slots reach 255 across the whole model (255 is reserved),",
                "      - normal slots reach 256,",
                "      - a model has at most 56 body parts,",
                "      - and each part's own vertex / normal / face counts are byte-sized.",
                "    Go over and the import REFUSES, names the part and the limit, and writes",
                "    nothing. Decimate that part, or move some detail onto a neighbour.",
                "",
                "    A grown model needs both of these to actually show up in game:",
                "",
                "      1. \"Enable loose file support\" ticked (bottom of this window). A model",
                "         larger than the disc's is read ONLY through the loose-file path.",
                "      2. Installed under its ORIGINAL name — gamedata\\load\\CHARA\\DOB.ILM.",
                "         A file called DOB_new.ILM is never looked up.",
                "",
                "    Texture packs do not get in the way — a pack's .png for the same",
                "    character overrides its TEXTURE only, never the loose .ILM.",
                "",
                "    REPLACING a part outright (your own mesh, not theirs)",
                "",
                "    You can also throw the original geometry away and model your own. When",
                "    the .obj no longer has the same vertices and faces the original had —",
                "    faces deleted, a part remodelled from scratch, a mesh retopologised —",
                "    the import offers to REBUILD the model instead, and asks first. The",
                "    original .ILM then supplies only the rig: bone bindings, draw order,",
                "    materials and palette rows. Every vertex, face, normal and UV is yours.",
                "",
                "    You never pick this. It is offered only when reshaping and adding have",
                "    both already been ruled out, so an .obj either of those can handle is",
                "    still handled that way — a rebuild is never the quiet answer to a small",
                "    edit.",
                "",
                "    What your .obj has to look like:",
                "",
                "      - Split your mesh into objects named EXACTLY like the originals, in",
                "        the same order, with the same number of objects. The leading TWO",
                "        DIGITS of the name ARE the bone index — 06LHAND is bone 6. Rename",
                "        one and that part silently binds to bone 0.",
                "      - Every face must be a triangle or a quad, and must be unwrapped:",
                "        each corner carries a texel.",
                "      - A face may only use a material the part already had. The material",
                "        name encodes the CLUT palette row, and there is nowhere to invent",
                "        a new one.",
                "",
                "    Joints — this is the part that bites. The format has no skinning and no",
                "    weights. A seam only stays shut because the later-drawn part reads the",
                "    earlier part's vertex. So either:",
                "",
                "      - model the parts OVERLAPPING at each joint, the way the originals",
                "        are (that is why hands and heads interpenetrate in Blender), or",
                "      - snap the seam vertices of the two parts to EXACTLY the same place.",
                "        The rebuild welds coincident vertices (within 0.01 units) onto the",
                "        earlier-drawn part automatically.",
                "",
                "    Snap them exactly rather than by eye. The radius is deliberately tiny:",
                "    a seam that misses stays open, which you can SEE and fix, while a weld",
                "    that reaches too far binds geometry to the wrong bone and only shows",
                "    itself once that limb moves. The confirmation reports both — how close",
                "    the nearest unwelded pair came, and any vertex that changed bone. Read",
                "    them; they are the whole diagnosis.",
                "",
                "    Install a rebuilt model exactly like a grown one: loose file support on,",
                "    and under its ORIGINAL name in gamedata\\load\\<FOLDER>\\.",
                "",
                "    Rebuild the animations? No — you do not have to. The rig is untouched:",
                "    new vertices belong to the part you put them in and follow its bone, so",
                "    every existing animation keeps working. That is also why the one rule",
                "    from above still stands — never rename, add or remove an 'o' OBJECT.",
                "",
                "    Preview before you install: \"View Model…\" opens the rebuilt .ILM.",
            };

            ShowTextDialog("Loose-File Mods — Help", lines, false);
        }

        /// <summary>Scrollable read-only text modal — the shape the Help dialog has always had,
        /// shared so the grow-mode budget report and its refusals look the same. Monospace turns
        /// off word wrap and widens the window, which column-aligned tables need to stay readable.</summary>
        private void ShowTextDialog(string title, string[] lines, bool monospace)
        {
            ShowTextDialog(title, lines, monospace, null);
        }

        /// <summary>With `confirmText` the Close button becomes that action plus a Cancel, and the
        /// return value is whether the user took it. A rebuild has to be judged from the weld and
        /// bone report, which is far too long for a MessageBox, so the question is asked on the
        /// same page as the evidence rather than in a box that follows it.</summary>
        private bool ShowTextDialog(string title, string[] lines, bool monospace, string confirmText)
        {
            int w = monospace ? 720 : 540;
            int h = monospace ? 470 : 420;
            bool confirm = !string.IsNullOrEmpty(confirmText);
            bool accepted = false;
            // A column-aligned table only lines up in a fixed-pitch face. The font is declared
            // OUTSIDE the form's using so it is disposed after the form, never while a live
            // control still references it. A null resource in a using is a legal no-op.
            using (Font mono = monospace ? new Font(FontFamily.GenericMonospace, 8.25f) : null)
            using (var dlg = new Form())
            {
                dlg.Text            = title;
                dlg.ClientSize      = new Size(w, h);
                dlg.FormBorderStyle = FormBorderStyle.FixedDialog;
                dlg.StartPosition   = FormStartPosition.CenterParent;
                dlg.MaximizeBox     = false;
                dlg.MinimizeBox     = false;
                try { dlg.Icon = Properties.Resources.launchericon; } catch { }

                var box = new TextBox
                {
                    Multiline   = true,
                    ReadOnly    = true,
                    ScrollBars  = monospace ? ScrollBars.Both : ScrollBars.Vertical,
                    WordWrap    = !monospace,
                    BorderStyle = BorderStyle.FixedSingle,
                    BackColor   = SystemColors.Window,
                    Location    = new Point(12, 12),
                    Size        = new Size(w - 24, h - 64),
                    TabStop     = false,
                    Text        = string.Join("\r\n", lines)
                };
                box.Select(0, 0);
                dlg.Controls.Add(box);

                var close = new Button
                {
                    Text = confirm ? "Cancel" : "Close",
                    Location = new Point(w - 96, h - 40),
                    Size = new Size(84, 28),
                    DialogResult = confirm ? DialogResult.Cancel : DialogResult.OK
                };
                dlg.Controls.Add(close);
                dlg.CancelButton = close;
                dlg.AcceptButton = close;
                if (confirm)
                {
                    var go = new Button
                    {
                        Text = confirmText,
                        Location = new Point(w - 216, h - 40),
                        Size = new Size(112, 28),
                        DialogResult = DialogResult.OK
                    };
                    dlg.Controls.Add(go);
                    // Cancel stays the default button: this dialog exists because the safe paths
                    // already refused, so Enter must not commit a rebuild the user has not read.
                }

                if (mono != null) box.Font = mono;
                accepted = dlg.ShowDialog(this) == DialogResult.OK;
            }
            return accepted;
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
