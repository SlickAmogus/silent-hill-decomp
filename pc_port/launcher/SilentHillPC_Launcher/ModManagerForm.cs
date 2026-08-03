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
        private FfmpegStatusRow _ffmpegRow;

        public ModManagerForm(ConfigManager config, string gameRoot)
        {
            _config   = config;
            _gameRoot = gameRoot;
            _mgr      = new ModManager(gameRoot, config);

            Text            = "Mod Manager";
            ClientSize      = new Size(600, 602);
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

        /// <summary>Ask about any archive lying in the mod folders, unpack what was agreed to
        /// (cancellable), then re-index and repopulate.</summary>
        private void ExtractThenScan()
        {
            var pending = _mgr.PendingWork();
            if (pending.Count > 0)
            {
                // A fully-stored .zip unpacks by plain copy, so it goes through silently.
                // Anything that really decompresses is offered first: this used to start on
                // its own and could duplicate a folder the user had unpacked by hand.
                var todo = pending.Where(p => p.Cheap).ToList();
                var ask  = pending.Where(p => !p.Cheap).ToList();
                if (ask.Count > 0 && AskUnpack(ask)) todo.AddRange(ask);

                if (todo.Count > 0)
                    ProgressDialog.RunCancellable(this, "Unpacking archives…",
                        (r, cancelled) => _mgr.Prepare(todo, r, cancelled));
            }
            _mgr.Scan();
            Populate();
        }

        /// <summary>Confirm before unpacking archives found in the mod folders, naming them.</summary>
        private bool AskUnpack(List<ModManager.PendingItem> items)
        {
            var lines = items.Take(8).Select(p => "    " + p.Name).ToList();
            if (items.Count > lines.Count) lines.Add("    …and " + (items.Count - lines.Count) + " more");

            return MessageBox.Show(this,
                "Unpack " + items.Count + " archive(s) found in your mod folders?\n\n" +
                string.Join("\n", lines) +
                "\n\nNo = leave them as they are; tick one later to unpack it.",
                "Mod Manager", MessageBoxButtons.YesNo, MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2) == DialogResult.Yes;
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
            var btnVw = new Button { Text = "Model Viewer", Location = new Point(510, 226), Size = new Size(78, 28) };
            var btnTp = new Button { Text = "TIM → PNG…",   Location = new Point(510, 258), Size = new Size(78, 28) };
            var btnBp = new Button { Text = "Bulk → PNG…",  Location = new Point(510, 290), Size = new Size(78, 28) };
            var btnRef = new Button { Text = "Reference ▾", Location = new Point(510, 322), Size = new Size(78, 28) };
            var btnReb = new Button { Text = "Rebuild…",    Location = new Point(510, 354), Size = new Size(78, 28) };
            var btnMo = new Button { Text = "Model → OBJ…", Location = new Point(510, 386), Size = new Size(78, 28) };
            var btnOm = new Button { Text = "OBJ → Model…", Location = new Point(510, 418), Size = new Size(78, 28) };
            var btnHelp = new Button { Text = "Help…",      Location = new Point(510, 450), Size = new Size(78, 28) };
            _btnTips = new ToolTip();
            _btnTips.SetToolTip(btnEx, "Unpack a Silent Hill .bin disc image into the loose asset tree.");
            _btnTips.SetToolTip(btnTp, "Convert individual .TIM texture files to .png.");
            _btnTips.SetToolTip(btnBp, "Recursively convert every .TIM under a folder to .png in place.");
            _btnTips.SetToolTip(btnRef, "Reference: one correct image to paint over — every region shown through the palette " +
                "the game really draws it with. Works for ANY texture (characters, world, weapons, items), one at a time or " +
                "the whole disc in one pass.");
            _btnTips.SetToolTip(btnReb, "Rebuild Textures: slice your edited reference image back into the per-row " +
                "NAME.TIM.pNN.png files the game loads (gamedata/load/<FOLDER>/). No 16-colour-per-region limit — paint freely.");
            _btnTips.SetToolTip(btnMo, "Model → OBJ: write a character model out as a .obj (plus .mtl and .ilmmeta.json) you can " +
                "open in Blender. Each 'o' object is ONE rigid animated body part — reshape its vertices freely, but do NOT " +
                "rename, add or remove objects: that list is the rig, and changing it breaks the animation.");
            _btnTips.SetToolTip(btnOm, "OBJ → Model: opens the high-poly replacement dialog (browse your model + the " +
                "character to replace, tick the fixes, Help explains each). A \"Simple…\" button there switches to " +
                "reshaping an existing character (patch / grow / replace, vertex-limited).");
            _btnTips.SetToolTip(btnVw, "Model Viewer: a 3D window for .ILM characters (with .ANM animation playback), " +
                ".PLM props, .TMD items and edited .obj files — textured with their real in-game palettes. " +
                "Open models from its File menu or drag & drop them onto it.");
            _btnTips.SetToolTip(btnHelp, "How to make and install loose-file texture mods.");
            btnEx.Click += (s, e) => OnExtractBin();
            btnTp.Click += (s, e) => OnConvertTim();
            btnBp.Click += (s, e) => OnBulkPng();
            var refMenu = new ContextMenuStrip();
            refMenu.Items.Add("One texture…",  null, (s, e) => OnBuildReference());
            refMenu.Items.Add("Every texture…", null, (s, e) => OnBuildAllReferences());
            btnRef.Click += (s, e) => refMenu.Show(btnRef, new Point(0, btnRef.Height));
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

            // BC7 .dds tooling (texconv). One button, a dropdown of actions —
            // like the OBJ pair, but grouped since they share the same converter.
            var btnDds = new Button { Text = "DDS ▾", Location = new Point(510, 482), Size = new Size(78, 28) };
            var ddsMenu = new ContextMenuStrip();
            ddsMenu.Items.Add("PNG → BC7 DDS…",       null, (s, e) => OnDdsEncode());
            ddsMenu.Items.Add("DDS → PNG…",           null, (s, e) => OnDdsDecode());
            ddsMenu.Items.Add("Convert folder → BC7…", null, (s, e) => OnDdsFolder());
            btnDds.Click += (s, e) => ddsMenu.Show(btnDds, new Point(0, btnDds.Height));
            _btnTips.SetToolTip(btnDds,
                "PNG ↔ BC7 .dds (via texconv). BC7 decodes about twice as fast as .png " +
                "and keeps a real 8-bit alpha the cutout needs; a whole-texture " +
                "replacement is also 4x cheaper in VRAM.\n\n" +
                "PNG → BC7 DDS: convert one or more .png to .dds beside them.\n" +
                "DDS → PNG: decode a .dds back to .png to inspect or edit.\n" +
                "Convert folder → BC7: convert every .png under a folder (a whole pack) " +
                "to .dds; offers to remove the source .png so the game uses the .dds. A " +
                "DuckStation pack is classified first so its whole-texture entries keep " +
                "a mip chain and its region entries are written single-level.\n\n" +
                "Works for loose overrides and every kind of DuckStation pack entry; the " +
                "GPU must support BC7 (any card since ~2010).");
            Controls.Add(btnDds);

            _chkLoose = new CheckBox
            {
                Text     = "Enable loose file support (required for load-folder mods)",
                Location = new Point(12, 486),
                AutoSize = true
            };
            Controls.Add(_chkLoose);

            // FMV mods only play through ffmpeg, and a wrong/absent runtime fails
            // silently — so the requirement is stated here, next to the mods it gates.
            _ffmpegRow = new FfmpegStatusRow(_gameRoot)
            {
                Location = new Point(12, 516),
                Size     = new Size(576, 28),
            };
            Controls.Add(_ffmpegRow);

            var btnApply = new Button { Text = "Apply", Location = new Point(414, 562), Size = new Size(84, 30) };
            var btnClose = new Button { Text = "Close", Location = new Point(504, 562), Size = new Size(84, 30) };
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
            ProgressDialog.RunCancellable(this, "Importing mods…", (r, cancelled) =>
            {
                foreach (var p in others)
                {
                    if (cancelled()) return;
                    if (_mgr.Import(p, r, cancelled) == ModManager.ImportResult.Added) imported++;
                }
            });

            ExtractThenScan(); // asks before unpacking anything we just copied in

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
                ProgressDialog.RunCancellable(this, "Extracting " + Path.GetFileName(binPath) + "…",
                    (r, cancelled) => { res = BinExtractor.Extract(binPath, outDir, convertPng, deleteTim, r, cancelled); });
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

            if (res.Cancelled)
            {
                MessageBox.Show(this,
                    "Cancelled after " + res.Files + " file(s). They are complete and were left in:\n" + outDir,
                    "Extract BIN", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            ClutComposer.ComposeAllResult refRes = null;
            if (buildRefs)
            {
                try
                {
                    string tree = outDir;
                    ProgressDialog.RunCancellable(this, "Building reference composites…", (r, cancelled) =>
                    {
                        var idx = ClutComposer.EnsureIndex(tree, (i, n, m) => r(i, n, "Indexing " + m));
                        refRes = ClutComposer.ComposeAll(idx, null, (i, n, m) => r(i, n, m), cancelled);
                    });
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
                msg += "Built " + (refRes.Made + refRes.Flat) + " reference composite(s).\n";
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
                var chkRef = new CheckBox { Text = "Build reference composites (one paintable image per texture)",
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

        private bool DdsReady()
        {
            if (DdsConverter.IsAvailable()) return true;
            MessageBox.Show(this,
                "texconv.exe isn't available, so BC7 .dds conversion can't run.\n\n" +
                "It ships embedded in the launcher; if this build was made without it, put " +
                "texconv.exe (Microsoft DirectXTex) next to the launcher or on your PATH.",
                "DDS", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }

        /// <summary>DDS ▸ "PNG → BC7 DDS…": convert selected .png to .dds beside them.</summary>
        private void OnDdsEncode()
        {
            if (!DdsReady()) return;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select PNG texture(s) to convert to BC7 .dds";
                ofd.Filter = "PNG images (*.png)|*.png|All files (*.*)|*.*";
                ofd.Multiselect = true;
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;

                int ok = 0;
                var failures = new System.Collections.Generic.List<string>();
                foreach (var png in ofd.FileNames)
                {
                    string err;
                    if (DdsConverter.EncodePng(png, Path.GetDirectoryName(png), out err)) ok++;
                    else failures.Add(err);
                }

                string msg = "Encoded " + ok + " of " + ofd.FileNames.Length + " file(s) to BC7 .dds.";
                if (failures.Count > 0) msg += "\n\nFailed:\n - " + string.Join("\n - ", failures.Take(6));
                MessageBox.Show(this, msg, "PNG → BC7 DDS", MessageBoxButtons.OK,
                    failures.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
        }

        /// <summary>DDS ▸ "DDS → PNG…": decode selected .dds back to .png beside them.</summary>
        private void OnDdsDecode()
        {
            if (!DdsReady()) return;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select .dds texture(s) to decode to PNG";
                ofd.Filter = "DDS textures (*.dds)|*.dds|All files (*.*)|*.*";
                ofd.Multiselect = true;
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;

                int ok = 0;
                var failures = new System.Collections.Generic.List<string>();
                foreach (var dds in ofd.FileNames)
                {
                    string err;
                    if (DdsConverter.DecodeDds(dds, Path.GetDirectoryName(dds), out err)) ok++;
                    else failures.Add(err);
                }

                string msg = "Decoded " + ok + " of " + ofd.FileNames.Length + " file(s) to PNG.";
                if (failures.Count > 0) msg += "\n\nFailed:\n - " + string.Join("\n - ", failures.Take(6));
                MessageBox.Show(this, msg, "DDS → PNG", MessageBoxButtons.OK,
                    failures.Count > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
            }
        }

        /// <summary>DDS ▸ "Convert folder → BC7…": encode every .png under a folder to
        /// .dds beside it (a whole pack at once), optionally removing the source PNGs
        /// so the game takes the .dds path.
        ///
        /// The folder is classified BEFORE anything is written, but no longer as a
        /// guardrail: since the engine gained CPU BC7 decoding, sub-rect entries
        /// composite normally and the whole pack converts. The classification now
        /// drives the mip decision (whole-cover keeps its chain, the rest is encoded
        /// single-level) and the informational counts in the dialog.</summary>
        private void OnDdsFolder()
        {
            if (!DdsReady()) return;
            string folder;
            using (var fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Choose a folder — every .png under it is converted to BC7 .dds";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) fbd.SelectedPath = gamedata;
                if (fbd.ShowDialog(this) != DialogResult.OK) return;
                folder = fbd.SelectedPath;
            }

            DdsConverter.PackAnalysis pack = null;
            try
            {
                ProgressDialog.Run(this, "Checking the folder…",
                    r => { pack = DdsConverter.AnalyzePack(folder); });
            }
            catch { pack = null; }

            bool wholeOnly = false;

            /* Total counts pack entries whatever the extension, so an already-converted
             * folder still reports thousands of files with nothing left to encode. Say
             * that instead of running a conversion that reports "0 converted". */
            if (pack != null && pack.Total > 0 && pack.Pngs == 0)
            {
                MessageBox.Show(this,
                    "Nothing to convert — all " + pack.Total.ToString("N0") +
                    " files here are already .dds.\n\n" +
                    "To start over, delete this folder and re-extract the pack from its archive.",
                    "Convert folder → BC7", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (pack != null && pack.SubRect > 0)
            {
                string head =
                    "Texture pack: " + pack.Pngs.ToString("N0") + " .png to convert" +
                    (pack.Total > pack.Pngs
                        ? " (" + (pack.Total - pack.Pngs).ToString("N0") + " already .dds)"
                        : "") + ".\n\n" +
                    "BC7 .dds loads about twice as fast as .png and uses ~30% less disk. " +
                    "Memory use is unchanged.\n\n";

                if (pack.WholeCoverPngs.Count > 0)
                {
                    var choice = MessageBox.Show(this, head +
                        "Yes    = convert all of it (recommended)\n" +
                        "No     = convert only the " + pack.WholeCoverPngs.Count.ToString("N0") +
                        " whole-texture files\n" +
                        "Cancel = do nothing",
                        "Convert folder → BC7", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Information,
                        MessageBoxDefaultButton.Button1);
                    if (choice == DialogResult.Cancel) return;
                    wholeOnly = choice == DialogResult.No;
                }
                else
                {
                    var choice = MessageBox.Show(this, head +
                        "Convert all of it?",
                        "Convert folder → BC7", MessageBoxButtons.YesNo, MessageBoxIcon.Information,
                        MessageBoxDefaultButton.Button1);
                    if (choice != DialogResult.Yes) return;
                }
            }

            var del = MessageBox.Show(this,
                (wholeOnly
                    ? "Converting only the " + pack.WholeCoverPngs.Count.ToString("N0") +
                      " whole-texture files; the rest are left alone.\n\n"
                    : "") +
                "Delete each source .png after it converts?\n\n" +
                "No  = keep both. The game prefers the .dds, so the .png just takes space.\n" +
                "Yes = keep only the .dds. Sources go to the Recycle Bin, except on very\n" +
                "        long paths (common in packs) where deletion is permanent.",
                "Convert folder → BC7", MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question,
                MessageBoxDefaultButton.Button2);
            if (del == DialogResult.Cancel) return;
            bool deleteSource = del == DialogResult.Yes;

            int converted = 0, failed = 0;
            bool finished = true;
            string firstError = null;
            try
            {
                // The whole-cover run hands EncodeFiles an explicit source list, so
                // delete-source can only ever reach a file that was converted — a
                // skipped region .png is not in the list at all. Those entries take
                // the compressed upload path, so they need the full mip chain.
                if (wholeOnly)
                    finished = ProgressDialog.RunCancellable(this, "Encoding whole textures to BC7…",
                        (r, cancelled) => DdsConverter.EncodeFiles(pack.WholeCoverPngs, deleteSource, true, r, cancelled,
                                                      out converted, out failed, out firstError));
                else
                    finished = ProgressDialog.RunCancellable(this, "Encoding textures to BC7…",
                        (r, cancelled) => DdsConverter.EncodeFolder(folder, deleteSource,
                                                      pack != null ? pack.WholeCoverPngs : null, r, cancelled,
                                                      out converted, out failed, out firstError));
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Conversion failed:\n\n" + ex.Message,
                    "Convert folder → BC7", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (!finished)
            {
                MessageBox.Show(this,
                    "Cancelled after " + converted + " texture(s). The rest are untouched — run it again to finish.",
                    "Convert folder → BC7", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            string msg = "Encoded " + converted + " texture(s) to BC7 .dds.";
            if (wholeOnly)
                msg += "\nLeft " + pack.SubRect.ToString("N0") + " region file(s) untouched.";
            if (failed > 0) msg += "\n" + failed + " failed.";
            if (firstError != null) msg += "\n\nFirst error:\n" + firstError;
            MessageBox.Show(this, msg, "Convert folder → BC7", MessageBoxButtons.OK,
                failed > 0 ? MessageBoxIcon.Warning : MessageBoxIcon.Information);
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
                ProgressDialog.RunCancellable(this, "Converting textures…",
                    (r, cancelled) => { res = TimConverter.BulkConvert(folder, deleteOriginals, r, cancelled); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Conversion failed:\n\n" + ex.Message,
                    "Bulk → PNG", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (res != null && res.Cancelled)
            {
                MessageBox.Show(this,
                    "Cancelled after " + res.Converted + " file(s). The rest are untouched — run it again to finish.",
                    "Bulk → PNG", MessageBoxButtons.OK, MessageBoxIcon.Information);
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

        /// <summary>The extracted-disc root for an asset stored at ROOT\&lt;CLASS&gt;\NAME.EXT —
        /// the folder the whole-tree CLUT index is built over.</summary>
        private static string TreeRootFor(string assetPath)
        {
            string dir = Path.GetDirectoryName(Path.GetFullPath(assetPath));
            string parent = Path.GetDirectoryName(dir);
            return (!string.IsNullOrEmpty(parent) && Directory.Exists(parent)) ? parent : dir;
        }

        /// <summary>"Reference ▾ → One texture…": compose one texture's true in-game look
        /// (every region through the palette that draws it) into one editable PNG. Works for
        /// any texture class — the row map is unioned over every model on the disc that
        /// samples the sheet, which is what the index is for.</summary>
        private void OnBuildReference()
        {
            string target;
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select a texture (.TIM) or a model (.ILM / .PLM / .IPD)";
                ofd.Filter = "Textures and models (*.tim;*.ilm;*.plm;*.ipd)|*.tim;*.ilm;*.plm;*.ipd|All files (*.*)|*.*";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) ofd.InitialDirectory = gamedata;
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                target = ofd.FileName;
            }

            string root = TreeRootFor(target);
            List<ClutComposer.Target> targets = null;
            string err = null;
            try
            {
                ProgressDialog.Run(this, "Reading the texture…", r =>
                {
                    var idx = ClutComposer.EnsureIndex(root, (i, n, m) => r(i, n, "Indexing " + m));
                    targets = ClutComposer.ResolveTargets(target, idx, null, out err);
                });
            }
            catch (Exception ex) { err = ex.Message; }

            if (targets == null || targets.Count == 0)
            {
                MessageBox.Show(this, "Could not build the reference:\n\n" + (err ?? "unknown error"),
                    "Reference", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            if (targets.Count > 1)
            {
                BuildReferenceSet(targets, target);
                return;
            }

            var t = targets[0];
            string outPng;
            using (var sfd = new SaveFileDialog())
            {
                sfd.Title = "Save reference image";
                sfd.Filter = "PNG image (*.png)|*.png";
                sfd.InitialDirectory = Path.GetDirectoryName(target);
                sfd.FileName = Path.GetFileNameWithoutExtension(t.TimPath) + "_reference.png";
                if (sfd.ShowDialog(this) != DialogResult.OK) return;
                outPng = sfd.FileName;
            }

            ClutComposer.ComposeResult res = null;
            try
            {
                ProgressDialog.Run(this, "Building reference…", r => { res = ClutComposer.ComposeTarget(t, outPng); });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not build the reference:\n\n" + ex.Message,
                    "Reference", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string what = t.IsFlat
                ? string.Format("{0} — {1}x{2}, flat 2D texture (no model draws it, so this is its plain decode).",
                                Path.GetFileName(outPng), res.Width, res.Height)
                : string.Format("{0} — {1}x{2}, {3} palette row(s), {4:F0}% of the sheet covered, {5:F0}% shared.",
                                Path.GetFileName(outPng), res.Width, res.Height, res.Rows.Count,
                                res.CoveragePct, res.SharedPct);
            if (res.TooShared)
                what += string.Format("\n\nWARNING: {0:F0}% of it is drawn through MORE THAN ONE palette and a " +
                                      "composite can show only one. Edit the per-row {1}.pNN.png set instead.",
                                      res.SharedPct, Path.GetFileName(t.TimPath));
            if (MessageBox.Show(this, what + "\n\nPaint over it, then \"Rebuild…\".  Open it now?",
                    "Reference", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outPng); } catch { }
            }
        }

        /// <summary>A model that draws several textures composes to one reference each.</summary>
        private void BuildReferenceSet(List<ClutComposer.Target> targets, string source)
        {
            string outDir;
            using (var fbd = new FolderBrowserDialog())
            {
                fbd.Description = Path.GetFileName(source) + " uses " + targets.Count +
                                  " textures — folder for the reference images";
                fbd.SelectedPath = Path.GetDirectoryName(source);
                if (fbd.ShowDialog(this) != DialogResult.OK) return;
                outDir = fbd.SelectedPath;
            }

            int made = 0, tooShared = 0;
            var failures = new List<string>();
            try
            {
                ProgressDialog.RunCancellable(this, "Building references…", (r, cancelled) =>
                {
                    for (int i = 0; i < targets.Count; i++)
                    {
                        if (cancelled()) return; // between images: each one is written whole
                        r(i, targets.Count, Path.GetFileName(targets[i].TimPath));
                        string png = Path.Combine(outDir,
                            Path.GetFileNameWithoutExtension(targets[i].TimPath) + "_reference.png");
                        try
                        {
                            if (ClutComposer.ComposeTarget(targets[i], png).TooShared) tooShared++;
                            made++;
                        }
                        catch (Exception ex) { failures.Add(Path.GetFileName(targets[i].TimPath) + ": " + ex.Message); }
                    }
                });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not build the references:\n\n" + ex.Message,
                    "Reference", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            string msg = made + " reference image(s) written to:\n" + outDir;
            if (tooShared > 0)
                msg += string.Format("\n{0} of them are over {1:F0}% shared — for those, edit the per-row " +
                                     "pNN.png set, not the composite.", tooShared, ClutComposer.SharedWarnPct);
            if (failures.Count > 0) msg += "\n\nFailed:\n - " + string.Join("\n - ", failures.Take(6));
            if (MessageBox.Show(this, msg + "\n\nOpen the folder?", "Reference",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outDir); } catch { }
            }
        }

        /// <summary>"Reference ▾ → Every texture…": compose every texture on the disc in one
        /// pass, into a sibling folder mirroring the tree.</summary>
        private void OnBuildAllReferences()
        {
            string tree;
            using (var fbd = new FolderBrowserDialog())
            {
                fbd.Description = "Extracted disc folder (the one holding BG, CHARA, TIM…)";
                string gamedata = Path.Combine(_gameRoot, "gamedata");
                if (Directory.Exists(gamedata)) fbd.SelectedPath = gamedata;
                if (fbd.ShowDialog(this) != DialogResult.OK) return;
                tree = fbd.SelectedPath;
            }
            string outDir = tree.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar) + "_reference";

            ClutComposer.ComposeAllResult res = null;
            try
            {
                ProgressDialog.RunCancellable(this, "Building reference images…", (r, cancelled) =>
                {
                    var idx = ClutComposer.EnsureIndex(tree, (i, n, m) => r(i, n, "Indexing " + m));
                    res = ClutComposer.ComposeAll(idx, outDir, (i, n, m) => r(i, n, m), cancelled);
                });
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not build the references:\n\n" + ex.Message,
                    "Reference", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (res == null) return;

            string msg = string.Format("{0} reference image(s){1} in:\n{2}",
                res.Made + res.Flat, res.Cancelled ? ", cancelled early" : "", outDir);
            if (res.Failed > 0) msg += "\n" + res.Failed + " texture(s) could not be composed.";
            if (res.TooShared > 0)
                msg += string.Format("\n{0} sheet(s) are over {1:F0}% shared — for those, edit the per-row " +
                                     "pNN.png set, not the composite.", res.TooShared, ClutComposer.SharedWarnPct);
            if (MessageBox.Show(this, msg + "\n\nOpen the folder?", "Reference",
                    MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outDir); } catch { }
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

            string source = GuessSourceFor(edited);
            using (var ofd = new OpenFileDialog())
            {
                ofd.Title = "Select the texture (.TIM) or model this image came from";
                ofd.Filter = "Textures and models (*.tim;*.ilm;*.plm;*.ipd)|*.tim;*.ilm;*.plm;*.ipd|All files (*.*)|*.*";
                if (source != null) { ofd.InitialDirectory = Path.GetDirectoryName(source); ofd.FileName = Path.GetFileName(source); }
                else ofd.InitialDirectory = Path.GetDirectoryName(edited);
                if (ofd.ShowDialog(this) != DialogResult.OK) return;
                source = ofd.FileName;
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

            string src = source;
            ClutComposer.SplitResult res = null;
            try
            {
                ProgressDialog.Run(this, "Rebuilding textures…", r =>
                {
                    var idx = ClutComposer.EnsureIndex(TreeRootFor(src), (i, n, m) => r(i, n, "Indexing " + m));
                    res = ClutComposer.Split(edited, src, idx, null, outDir);
                });
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
                "\n\nPalette rows used: " + string.Join(", ", res.RowsUsed) +
                "\n\nDrop these into gamedata/load/<FOLDER>/ (e.g. CHARA) and tick " +
                "\"Enable loose file support\".";
            if (res.RowsDropped.Count > 0)
                msg += "\n\nRow(s) " + string.Join(", ", res.RowsDropped) + " are past the game's 16-row " +
                       "limit and were skipped — it could never load them.";
            if (MessageBox.Show(this, msg + "\n\nOpen the output folder?",
                    "Rebuild Textures", MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes)
            {
                try { System.Diagnostics.Process.Start(outDir); } catch { }
            }
        }

        /// <summary>"Model → OBJ…" button — shared flow in ConverterActions so the
        /// Model Viewer's menu drives the exact same implementation.</summary>
        private void OnExportModel()
        {
            ConverterActions.ExportModel(this, _gameRoot);
        }

        /// <summary>"OBJ → Model…" button: the high-poly replacement dialog (its
        /// "Simple…" button drops into the edit-existing flow). Shared flow.</summary>
        private void OnImportModel()
        {
            ConverterActions.HighPolyImport(this, _gameRoot);
        }

        /// <summary>"Model Viewer" button: show the reusable viewer window, empty —
        /// models open from its File menu or by drag &amp; drop.</summary>
        private void OnViewModel()
        {
            ConverterActions.OpenModelViewer(this, _gameRoot);
        }

        /// <summary>Best-effort guess of the texture or model a reference PNG came from
        /// (NAME_reference.png -> NAME.TIM / .ILM / .PLM / .IPD beside it). The .TIM wins:
        /// it is the target that gets the whole-corpus palette-row union.</summary>
        private static string GuessSourceFor(string png)
        {
            string dir = Path.GetDirectoryName(png) ?? ".";
            string stem = Path.GetFileNameWithoutExtension(png);
            if (stem.EndsWith("_reference", StringComparison.OrdinalIgnoreCase))
                stem = stem.Substring(0, stem.Length - "_reference".Length);
            foreach (var ext in new[] { ".TIM", ".tim", ".ILM", ".ilm", ".PLM", ".plm", ".IPD", ".ipd" })
            {
                string c = Path.Combine(dir, stem + ext);
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
                "    THE EASY WAY: don't edit those palette rows by hand. \"Reference ▾\"",
                "    builds ONE image showing every region through the palette the game",
                "    really draws it with — that is what the texture looks like in game.",
                "    It works for every texture on the disc, not just characters: the",
                "    world (.IPD), weapons and items (.PLM) too, and \"Every texture…\"",
                "    does the whole disc in one pass. Paint over that image (at native",
                "    size or any upscale of it), then \"Rebuild…\" slices it back into the",
                "    NAME.TIM.pNN.png set below. No 16-colour limit — paint freely.",
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
                "    The snap-and-weld option is for THIS rebuild only. The HIGH-POLY path",
                "    has NO welding at all: snapped vertices stay separate copies and the",
                "    joint opens as soon as a bone bends, so a high-poly model must OVERLAP",
                "    at every joint.",
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
            ConverterActions.ShowTextDialog(this, title, lines, monospace);
        }

        private void OnApply(object sender, EventArgs e)
        {
            CommitOrderAndState();
            try
            {
                ModManager.ApplyResult r = null;
                // Cancel here aborts an archive being unpacked (the only slow part), not the
                // apply itself: the pack is left off and everything else still commits, so the
                // config on disk always matches the list the user is looking at.
                ProgressDialog.RunCancellable(this, "Applying mods…",
                    (rep, cancelled) => { r = _mgr.Apply(_chkLoose.Checked, rep, cancelled); });
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
