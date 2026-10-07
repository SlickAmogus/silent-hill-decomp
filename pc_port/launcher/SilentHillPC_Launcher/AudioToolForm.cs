/* SPDX-License-Identifier: GPL-3.0-or-later */
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Media;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>Sound-bank browser: lists the samples inside a .VAB, previews them,
    /// and exports them as .wav or raw .vag. One reusable window, like the Model
    /// Viewer — a second "Audio" click or a dropped file loads into the same one
    /// rather than stacking windows.</summary>
    internal sealed class AudioToolForm : Form
    {
        private static AudioToolForm s_open;

        private readonly ListView _list = new ListView();
        private readonly Label _info = new Label();
        private readonly Button _btnPlay = new Button();
        private readonly Button _btnStop = new Button();
        private readonly Button _btnWav = new Button();
        private readonly Button _btnAll = new Button();
        private readonly Button _btnVag = new Button();
        private readonly Button _btnRep = new Button();
        private readonly Button _btnRevert = new Button();
        private readonly Button _btnSave = new Button();
        private readonly ComboBox _rate = new ComboBox();
        private readonly ComboBox _slot = new ComboBox();

        /// <summary>Staged sample replacements, applied only when the bank is saved, so
        /// the user can audition and back out without touching the original file.</summary>
        private readonly Dictionary<int, byte[]> _pending = new Dictionary<int, byte[]>();

        private VabFile _vab;
        private SoundPlayer _player;
        private string _lastDir;
        private string _gameRoot;
        /// <summary>The pristine extract's SND/ folder, when one was found. It is the
        /// authority on which banks share a sample; edited copies elsewhere are not.</summary>
        private string _sndDir;
        private bool _askedForSndDir;
        private SndSampleIndex _index;
        private readonly Dictionary<int, List<SfxMatch>> _matches = new Dictionary<int, List<SfxMatch>>();
        private readonly Dictionary<int, List<SampleRef>> _dupes = new Dictionary<int, List<SampleRef>>();

        /// <summary>Each sample as the disc has it, by index: the clean extract's copy of
        /// the open bank when there is one, else the open bank's own bytes. Duplicates
        /// are searched with THESE, so a bank already edited in gamedata/load finds the
        /// same other copies as the pristine one — the edited bytes match nothing.</summary>
        private readonly Dictionary<int, byte[]> _originals = new Dictionary<int, byte[]>();
        /// <summary>Samples whose bytes in the open file differ from the clean extract.</summary>
        private readonly HashSet<int> _edited = new HashSet<int>();

        private const string CleanSndKey = "launcher_audio_clean_snd";

        private static string ConfigPath
        {
            get { return Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.cfg"); }
        }

        /// <summary>The clean SND/ folder the user picked earlier, if it still holds banks.</summary>
        public static string SavedCleanSndDir()
        {
            try
            {
                if (!File.Exists(ConfigPath)) return null;
                string dir = new ConfigManager(ConfigPath).Get(CleanSndKey, "").Trim();
                return HasBanks(dir) ? dir : null;
            }
            catch { return null; }
        }

        private static void SaveCleanSndDir(string dir)
        {
            try
            {
                var cfg = new ConfigManager(ConfigPath);
                cfg.EnsureLauncherSection();
                cfg.Set(CleanSndKey, dir ?? "");
                cfg.Save();
            }
            catch { }
        }

        public static void ShowTool(IWin32Window owner, string gameRoot, string startDir, string cleanSndDir)
        {
            AudioToolForm f = s_open;
            if (f == null || f.IsDisposed)
            {
                f = new AudioToolForm();
                s_open = f;
                f.FormClosed += (s, e) => { if (s_open == f) s_open = null; };
                f._lastDir = startDir;
                f._gameRoot = gameRoot;
                f._sndDir = HasBanks(cleanSndDir) ? cleanSndDir : null;
                f.Show(owner);
            }
            else
            {
                if (f.WindowState == FormWindowState.Minimized) f.WindowState = FormWindowState.Normal;
                f.BringToFront();
                f.Activate();
            }
        }

        private AudioToolForm()
        {
            Text = Loc.T("Audio");
            ClientSize = new Size(760, 460);
            StartPosition = FormStartPosition.CenterParent;
            MinimumSize = new Size(620, 360);
            AllowDrop = true;

            var menu = new MenuStrip();
            var file = new ToolStripMenuItem("&File");
            file.DropDownItems.Add("&Open sound bank…", null, (s, e) => PickAndOpen());
            file.DropDownItems.Add("&Clean SND folder…", null, (s, e) => ChooseCleanSndDir(true));
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("E&xit", null, (s, e) => Close());
            var help = new ToolStripMenuItem("&Help");
            help.DropDownItems.Add("About sound banks…", null, (s, e) => ShowHelp());
            menu.Items.Add(file);
            menu.Items.Add(help);
            MainMenuStrip = menu;
            Controls.Add(menu);
            Loc.ApplyMenu(menu.Items);

            _list.View = View.Details;
            _list.FullRowSelect = true;
            _list.MultiSelect = true;
            _list.HideSelection = false;
            _list.GridLines = true;
            _list.Location = new Point(12, 30);
            _list.Size = new Size(736, 286);
            _list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;
            _list.Columns.Add("#", 44, HorizontalAlignment.Right);
            _list.Columns.Add(Loc.T("Bytes"), 74, HorizontalAlignment.Right);
            _list.Columns.Add(Loc.T("Samples"), 78, HorizontalAlignment.Right);
            _list.Columns.Add(Loc.T("Length"), 70, HorizontalAlignment.Right);
            _list.Columns.Add(Loc.T("Loops"), 52, HorizontalAlignment.Center);
            _list.Columns.Add(Loc.T("In-game rate"), 88, HorizontalAlignment.Right);
            _list.Columns.Add(Loc.T("Sound ids"), 240, HorizontalAlignment.Left);
            _list.ShowItemToolTips = true;
            _list.Columns.Add(Loc.T("Also in"), 170, HorizontalAlignment.Left);
            _list.Columns.Add(Loc.T("Programs"), 76, HorizontalAlignment.Left);
            _list.Columns.Add(Loc.T("Centre note"), 82, HorizontalAlignment.Right);
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.DoubleClick += (s, e) => PlaySelected();
            Controls.Add(_list);

            _info.Location = new Point(12, 322);
            _info.Size = new Size(736, 50);
            _info.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            _info.Text = Loc.T("No bank loaded — File > Open, or drag a .VAB onto this window.");
            Controls.Add(_info);

            int y = 380;
            SetupButton(_btnPlay, Loc.T("Play"), new Point(12, y), (s, e) => PlaySelected());
            SetupButton(_btnStop, Loc.T("Stop"), new Point(100, y), (s, e) => StopPlayback());
            SetupButton(_btnWav, Loc.T("Export WAV…"), new Point(188, y), (s, e) => ExportSelected(false));
            SetupButton(_btnVag, Loc.T("Export raw VAG…"), new Point(300, y), (s, e) => ExportSelected(true));
            SetupButton(_btnAll, Loc.T("Export all…"), new Point(430, y), (s, e) => ExportAll());
            SetupButton(_btnRep, Loc.T("Replace…"), new Point(12, y + 30), (s, e) => ReplaceSelected());
            SetupButton(_btnRevert, Loc.T("Revert"), new Point(100, y + 30), (s, e) => RevertSelected());
            SetupButton(_btnSave, Loc.T("Save bank…"), new Point(188, y + 30), (s, e) => SaveBank());
            // Buttons are measured to their caption, so a translated row can outgrow the
            // English positions above; flow each row left to right instead.
            int rowEnd = 0;
            foreach (var row in new[] { new[] { _btnPlay, _btnStop, _btnWav, _btnVag, _btnAll },
                                        new[] { _btnRep, _btnRevert, _btnSave } })
            {
                for (int i = 1; i < row.Length; i++) row[i].Left = Math.Max(row[i].Left, row[i - 1].Right + 4);
                rowEnd = Math.Max(rowEnd, row[row.Length - 1].Right);
            }

            var sl = new Label
            {
                Text = Loc.T("Bank slot:"),
                Location = new Point(548, y - 24),
                Size = new Size(78, 20),
                Anchor = AnchorStyles.Bottom | AnchorStyles.Left
            };
            Controls.Add(sl);
            _slot.DropDownStyle = ComboBoxStyle.DropDownList;
            _slot.Location = new Point(628, y - 28);
            _slot.Size = new Size(88, 22);
            _slot.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            _slot.Items.AddRange(new object[] { "This bank's", "Any", "base", "weapon", "ambient", "music" });
            Loc.LocalizeItems(_slot);
            _slot.SelectedIndex = 0;
            _slot.SelectedIndexChanged += (s, e) => { if (_vab != null) Open(_vab.Path); };
            Controls.Add(_slot);

            var rl = new Label
            {
                Text = Loc.T("Preview rate:"),
                Location = new Point(548, y + 6),
                Size = new Size(78, 20),
                Anchor = AnchorStyles.Bottom | AnchorStyles.Left
            };
            Controls.Add(rl);
            _rate.DropDownStyle = ComboBoxStyle.DropDownList;
            _rate.Location = new Point(628, y + 2);
            _rate.Size = new Size(88, 22);
            _rate.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            _rate.Items.AddRange(new object[] { AutoRate, "44100 Hz", "32000 Hz", "22050 Hz", "16000 Hz", "11025 Hz", "8000 Hz" });
            Loc.LocalizeItems(_rate);
            _rate.SelectedIndex = 0;

            // Label + picker pairs sit to the right of the button rows; move them (and
            // widen the window) when the translated rows or labels need the room.
            int labelW = Math.Max(78, Math.Max(sl.PreferredWidth, rl.PreferredWidth));
            int pairLeft = Math.Max(548, rowEnd + 12);
            foreach (var lbl in new[] { sl, rl }) { lbl.Left = pairLeft; lbl.Width = labelW; }
            foreach (var cb in new[] { _slot, _rate }) { cb.Left = pairLeft + labelW + 2; cb.Width = Math.Max(cb.Width, 110); }
            if (_rate.Right + 12 > ClientSize.Width)
            {
                int grow = _rate.Right + 12 - ClientSize.Width;
                MinimumSize = new Size(MinimumSize.Width + grow, MinimumSize.Height);
                ClientSize = new Size(ClientSize.Width + grow, ClientSize.Height);
            }
            _rate.SelectedIndexChanged += (s, e) =>
            {
                // Durations and the rate column are both derived from it.
                if (_vab != null) Open(_vab.Path);
            };
            Controls.Add(_rate);

            DragEnter += (s, e) =>
                e.Effect = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
            DragDrop += (s, e) =>
            {
                var files = e.Data.GetData(DataFormats.FileDrop) as string[];
                if (files != null && files.Length > 0) Open(files[0]);
            };

            UpdateButtons();
        }

        private void SetupButton(Button b, string text, Point p, EventHandler onClick)
        {
            b.Text = text;
            b.Location = p;
            // Measured rather than fixed: these labels are drawn in the user's font at
            // the user's DPI, and the tool column in the Mod Manager had to learn this
            // the hard way (buttons clipped mid-word with no visual cue).
            b.Size = new Size(Math.Max(84, TextRenderer.MeasureText(text, b.Font).Width + 16), 26);
            b.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            b.Click += onClick;
            Controls.Add(b);
        }

        private const string AutoRate = "Auto — in-game";

        /// <summary>Slot to restrict sound-id matching to, or -1 for any. A bank file does
        /// not record its own slot, so this is the user's call.</summary>
        private int SlotFilter
        {
            get
            {
                // Default: the slot the sound system loads THIS bank into. A row names
                // only a slot, so without it every id aimed at the weapon or music slot
                // that happens to share a program and note shows up under an ambient
                // bank as well.
                if (_slot.SelectedIndex == 0) return _vab == null ? -1 : VabFile.SlotOfBank(BaseName);
                return _slot.SelectedIndex <= 1 ? -1 : _slot.SelectedIndex - 2;
            }
        }

        /// <summary>Fixed rate the user picked, or 0 for "use each sample's own in-game
        /// rate". Guessing a single rate for a whole bank is what the sound table lets
        /// us stop doing.</summary>
        private int ChosenRate
        {
            get
            {
                string s = _rate.SelectedItem as string;
                if (s == null || s == AutoRate) return 0;
                int sp = s.IndexOf(' ');
                int r;
                return int.TryParse(sp > 0 ? s.Substring(0, sp) : s, out r) ? r : VabFile.UnityRate;
            }
        }

        /// <summary>The rate to render a given sample at: its real in-game rate when the
        /// sound table identifies it, otherwise the SPU's unity rate. A sample used at
        /// more than one pitch has no single answer, so the lowest is used — it is the
        /// longest and least likely to sound comically fast.</summary>
        private double RateFor(VabVag vag)
        {
            int fixedRate = ChosenRate;
            if (fixedRate > 0) return fixedRate;

            List<SfxMatch> hits;
            if (_matches.TryGetValue(vag.Index, out hits) && hits.Count > 0)
            {
                double lo = double.MaxValue;
                foreach (SfxMatch m in hits) if (m.RateHz < lo) lo = m.RateHz;
                if (lo > 1.0) return lo;
            }
            return VabFile.UnityRate;
        }

        private void PickAndOpen()
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = Loc.T("Open a sound bank");
                d.Filter = Loc.T("PSX sound banks") + " (*.vab)|*.vab|" + Loc.T("All files") + " (*.*)|*.*";
                if (!string.IsNullOrEmpty(_lastDir) && Directory.Exists(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) == DialogResult.OK) Open(d.FileName);
            }
        }

        public void Open(string path)
        {
            StopPlayback();
            string err;
            VabFile v = VabFile.Load(path, out err);
            if (v == null)
            {
                MessageBox.Show(this, err, Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Reopening the SAME bank is how the rate and slot pickers refresh, so staged
            // replacements have to survive it — only a genuinely different file discards
            // them.
            bool sameFile = _vab != null &&
                            string.Equals(Path.GetFullPath(_vab.Path), Path.GetFullPath(path),
                                StringComparison.OrdinalIgnoreCase);
            if (!sameFile) _pending.Clear();

            _vab = v;
            _lastDir = Path.GetDirectoryName(path);
            Text = Loc.T("Audio") + " — " + Path.GetFileName(path);

            if (_sndDir == null && !_askedForSndDir) ChooseCleanSndDir(false);

            // The extract first, so its copy of a bank wins over an edited one that
            // happens to sit next to the file being opened.
            var indexDirs = new List<string> { _sndDir, Path.GetDirectoryName(path) };
            if (_index == null || !_index.SameDirs(indexDirs)) _index = SndSampleIndex.Build(indexDirs);
            LoadOriginals(v, path);

            SfxAnswerIndex answers = SfxAnswerIndex.For(_sndDir);

            _matches.Clear();
            _dupes.Clear();
            _list.BeginUpdate();
            _list.Items.Clear();
            int identified = 0;
            foreach (VabVag vag in v.Vags)
            {
                List<SampleRef> others = _index.Others(_originals[vag.Index], BaseName);
                _dupes[vag.Index] = others;
                var progs = new List<int>();
                int centre = -1;
                foreach (VabTone t in vag.Tones)
                {
                    if (!progs.Contains(t.Program)) progs.Add(t.Program);
                    if (centre < 0) centre = t.CenterNote;
                }
                progs.Sort();

                List<SfxMatch> hits = v.MatchesFor(vag.Index, SlotFilter);
                // Ids only this bank answers first: with an ambient bank most of the
                // list is ids shared with every other map's bank, and those say least
                // about the sample.
                if (answers != null)
                {
                    hits.Sort(delegate(SfxMatch a, SfxMatch b)
                    {
                        int c = answers.BanksAnswering(a.Row.Id).CompareTo(answers.BanksAnswering(b.Row.Id));
                        return c != 0 ? c : a.Row.Id.CompareTo(b.Row.Id);
                    });
                }
                _matches[vag.Index] = hits;
                if (hits.Count > 0) identified++;

                // Duration follows the rate it actually plays at, so a sound pitched
                // down reads as the longer sound the player hears.
                double rate = RateFor(vag);
                int samples = vag.BlockCount * 28;
                double secs = samples / rate;

                var names = new List<string>();
                var rates = new List<string>();
                int layered = 0;
                foreach (SfxMatch m in hits)
                {
                    // The slot is part of the identity when the filter is off: the same
                    // program/note pair exists in banks loaded into different slots.
                    string label = SlotFilter >= 0
                        ? m.Label
                        : m.Label + " (" + Loc.T(SfxMatch.SlotName(m.Row.Slot)) + ")";
                    if (m.Layers > 1)
                    {
                        label += " +" + (m.Layers - 1);
                        layered++;
                    }
                    if (!names.Contains(label)) names.Add(label);
                    string r = Math.Round(m.RateHz).ToString("N0");
                    if (!rates.Contains(r)) rates.Add(r);
                }

                var it = new ListViewItem(vag.Index + (_edited.Contains(vag.Index) ? " *" : ""));
                it.SubItems.Add(vag.Length.ToString("N0"));
                it.SubItems.Add(samples.ToString("N0"));
                it.SubItems.Add(secs.ToString("0.00") + "s");
                it.SubItems.Add(vag.Loops ? Loc.T("yes") : "");
                it.SubItems.Add(rates.Count == 0 ? "-" :
                    (rates.Count == 1 ? rates[0] + " Hz" : string.Join(" / ", rates.ToArray()) + " Hz"));
                it.SubItems.Add(IdSummary(names));
                it.SubItems.Add(BankSummary(others));
                it.SubItems.Add(progs.Count == 0 ? Loc.T("(unused)") : string.Join(", ", progs.ConvertAll(x => x.ToString()).ToArray()));
                it.SubItems.Add(centre < 0 ? "-" : centre.ToString());
                it.Tag = vag;
                it.ToolTipText = RowTip(vag, hits, names, layered, answers);
                if (progs.Count == 0) it.ForeColor = SystemColors.GrayText;
                _list.Items.Add(it);
            }
            _list.EndUpdate();

            int slot = SlotFilter;
            _info.Text = Loc.F(
                "{0} samples, {1} programs, {2} tones — bank id {3}, {4:N0} bytes. {5} are reached " +
                "by a sound id. An id names a SLOT, not a bank, so these are the ids that play this " +
                "sample while this bank is the loaded {6} bank; the same id plays another bank's " +
                "sample elsewhere, and replacing here changes nothing there.",
                v.VagCount, v.ProgramCount, v.Tones.Count, v.VabId, v.DeclaredSize, identified,
                slot >= 0 ? Loc.T(SfxMatch.SlotName(slot)) : "");

            if (_edited.Count > 0)
            {
                _info.Text += " " + Loc.F("* marks the {0} sample(s) this file has already changed from the disc; " +
                                          "their other copies are still found from the disc's sound.", _edited.Count);
            }
            else if (_sndDir == null)
            {
                _info.Text += " " + Loc.T("No clean SND folder is set (File > Clean SND folder…), so other copies " +
                                          "are only found for samples this file has not changed.");
            }

            string twin = LoadedTwinBank(path);
            if (twin != null)
            {
                _info.Text = Loc.F("The game loads {0} instead of this bank — same sounds, and " +
                                   "this one is never requested. The port accepts either name, so an " +
                                   "export from here still works; name it {0}_005.wav to be explicit.", twin)
                           + "\r\n" + _info.Text;
                _info.ForeColor = Color.FromArgb(255, 170, 90);
            }
            else
            {
                _info.ForeColor = ForeColor;
            }

            if (_list.Items.Count > 0) _list.Items[0].Selected = true;
            MarkPending();
        }

        /// <summary>Fill _originals and _edited for a freshly opened bank. The clean copy
        /// is matched by file name; a different sample count means it is not the same
        /// bank's layout, and then the open file stands for itself.</summary>
        private void LoadOriginals(VabFile v, string path)
        {
            _originals.Clear();
            _edited.Clear();

            VabFile clean = null;
            if (_sndDir != null)
            {
                string cand = Path.Combine(_sndDir, Path.GetFileNameWithoutExtension(path) + ".VAB");
                if (File.Exists(cand) && !SamePath(cand, path))
                {
                    string err;
                    clean = VabFile.Load(cand, out err);
                    if (clean != null && clean.VagCount != v.VagCount) clean = null;
                }
            }

            foreach (VabVag vag in v.Vags)
            {
                byte[] own = v.RawVag(vag.Index);
                byte[] orig = clean != null ? clean.RawVag(vag.Index) : own;
                _originals[vag.Index] = orig;
                if (clean != null && SndSampleIndex.Key(orig) != SndSampleIndex.Key(own)) _edited.Add(vag.Index);
            }
        }

        private static bool SamePath(string a, string b)
        {
            try { return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        }

        /// <summary>Ask for the SND folder of a clean extract. Unprompted it is asked
        /// once per window, and only because none was found; from the menu it is the
        /// way to change a folder the launcher guessed wrong.</summary>
        private void ChooseCleanSndDir(bool fromMenu)
        {
            if (!fromMenu)
            {
                _askedForSndDir = true;
                if (MessageBox.Show(this,
                        Loc.T("The Audio tool finds a sound's other copies, and marks the samples you have " +
                              "already changed, by comparing against the SND folder of a clean disc extract. " +
                              "None was found under gamedata.\n\nChoose one now?"),
                        Loc.T("Audio"), MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
                    return;
            }

            using (var d = new FolderBrowserDialog())
            {
                d.Description = _sndDir != null
                    ? Loc.F("The SND folder of a clean, unedited disc extract (now: {0})", _sndDir)
                    : Loc.T("The SND folder of a clean, unedited disc extract");
                string start = _sndDir ?? _lastDir;
                if (!string.IsNullOrEmpty(start) && Directory.Exists(start)) d.SelectedPath = start;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                string dir = d.SelectedPath;
                if (!HasBanks(dir))
                {
                    MessageBox.Show(this, Loc.T("That folder holds no .VAB sound banks."), Loc.T("Audio"),
                        MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                string parent = Path.GetFileName(Path.GetDirectoryName(dir) ?? "");
                if (string.Equals(parent, "load", StringComparison.OrdinalIgnoreCase))
                {
                    // Edited banks live here, so it cannot say what the disc's sounds are.
                    MessageBox.Show(this,
                        Loc.T("That is gamedata\\load\\SND, where your edited banks go. Choose the SND " +
                              "folder inside a disc extract instead."),
                        Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }

                _sndDir = dir;
                _index = null;
                SaveCleanSndDir(dir);
            }

            if (fromMenu && _vab != null) Open(_vab.Path);
        }

        /// <summary>The MEP twin the game loads in place of this bank, or null.</summary>
        private static string LoadedTwinBank(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;
            string stem = Path.GetFileNameWithoutExtension(path);
            return VabFile.IsMapOnlyBank(stem) ? "MEP" + stem.Substring(3) : null;
        }

        private static bool HasBanks(string dir)
        {
            if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return false;
            try { return Directory.GetFiles(dir, "*.vab").Length > 0; }
            catch { return false; }
        }

        /// <summary>Four ids and a count. A sample in an ambient bank answers to dozens
        /// of them, and a wall of Sfx_Unk numbers in a 240px column says nothing; the
        /// first few are the ones fewest other banks share.</summary>
        private static string IdSummary(List<string> names)
        {
            if (names.Count == 0) return "";
            if (names.Count <= 3) return string.Join(", ", names.ToArray());
            return string.Join(", ", names.GetRange(0, 3).ToArray()) + ", " + Loc.F("+{0} more", names.Count - 3);
        }

        private const int TipIdCap = 24;

        /// <summary>The row's ids in full, up to a readable cap, with what an id means
        /// here spelled out: how many banks answer the same id, and whether the id keys
        /// on other samples at the same time.</summary>
        private string RowTip(VabVag vag, List<SfxMatch> hits, List<string> names, int layered, SfxAnswerIndex answers)
        {
            var sb = new System.Text.StringBuilder();
            int slot = SlotFilter;
            sb.Append(Loc.F("Sample {0}", vag.Index));
            if (_edited.Contains(vag.Index)) sb.Append(" ").Append(Loc.T("(already changed from the disc)"));
            sb.AppendLine();

            if (hits.Count == 0)
            {
                sb.Append(slot >= 0
                    ? Loc.F("No sound id reaches this sample from the {0} slot.", Loc.T(SfxMatch.SlotName(slot)))
                    : Loc.T("No sound id reaches this sample."));
                return sb.ToString();
            }

            sb.Append(Loc.F("Played by {0} id(s) while this bank is loaded:", names.Count)).AppendLine();

            var seen = new List<string>();
            int shown = 0;
            foreach (SfxMatch m in hits)
            {
                string label = m.Label;
                if (seen.Contains(label)) continue;
                seen.Add(label);
                if (shown >= TipIdCap) continue;

                sb.Append("  ").Append(label);
                if (m.Layers > 1) sb.Append(" ").Append(Loc.F("(+{0} more samples at once)", m.Layers - 1));
                int banks = answers == null ? 0 : answers.BanksAnswering(m.Row.Id);
                if (banks == 1) sb.Append(" — ").Append(Loc.T("only this bank answers it"));
                else if (banks > 1) sb.Append(" — ").Append(Loc.F("shared with {0} other bank(s)", banks - 1));
                sb.AppendLine();
                shown++;
            }
            if (seen.Count > shown) sb.Append("  ").Append(Loc.F("... and {0} more", seen.Count - shown)).AppendLine();

            sb.Append(slot >= 0
                ? Loc.F("A shared id plays whichever bank is loaded in the {0} slot at the time, so replacing " +
                        "this sample changes that sound only where this bank is loaded.", Loc.T(SfxMatch.SlotName(slot)))
                : Loc.T("A shared id plays whichever bank is loaded in the same slot at the time, so replacing " +
                        "this sample changes that sound only where this bank is loaded."));
            return sb.ToString();
        }

        /// <summary>Distinct bank names, shortened past four so the column stays a
        /// glance rather than a paragraph; the full list appears when the sample is
        /// replaced and again when the bank is saved.</summary>
        private static string BankSummary(List<SampleRef> others)
        {
            if (others == null || others.Count == 0) return "";
            var banks = new List<string>();
            foreach (SampleRef r in others) if (!banks.Contains(r.Bank)) banks.Add(r.Bank);
            if (banks.Count <= 4) return string.Join(", ", banks.ToArray());
            return string.Join(", ", banks.GetRange(0, 3).ToArray()) + " " + Loc.F("+{0} more", banks.Count - 3);
        }

        private static string BankDetail(List<SampleRef> others)
        {
            var parts = new List<string>();
            foreach (SampleRef r in others) parts.Add(r.Bank + " #" + r.Index);
            return string.Join(", ", parts.ToArray());
        }

        private void UpdateButtons()
        {
            bool any = _vab != null && _list.SelectedItems.Count > 0;
            _btnPlay.Enabled = any && _list.SelectedItems.Count == 1;
            _btnWav.Enabled = any;
            _btnVag.Enabled = any;
            _btnAll.Enabled = _vab != null;
            _btnStop.Enabled = _player != null;
            _btnRep.Enabled = any && _list.SelectedItems.Count == 1;
            _btnRevert.Enabled = any && SelectedHasPending();
            _btnSave.Enabled = _vab != null && _pending.Count > 0;
        }

        private bool SelectedHasPending()
        {
            foreach (ListViewItem it in _list.SelectedItems)
                if (_pending.ContainsKey(((VabVag)it.Tag).Index)) return true;
            return false;
        }

        /// <summary>Import a replacement for the selected sample. A .vag goes in as-is;
        /// a .wav is resampled to the rate the tone will play it at and re-encoded,
        /// because the sample carries no rate of its own — dropping in 44.1 kHz audio
        /// unchanged makes it play at whatever speed the tone dictates.</summary>
        private void ReplaceSelected()
        {
            VabVag vag = Selected;
            if (_vab == null || vag == null) return;

            using (var d = new OpenFileDialog())
            {
                d.Title = Loc.F("Replace sample {0}", vag.Index);
                d.Filter = Loc.T("Audio") + " (*.wav;*.vag)|*.wav;*.vag|" + Loc.T("WAV audio") + " (*.wav)|*.wav|" +
                           Loc.T("Raw PSX ADPCM") + " (*.vag)|*.vag";
                if (!string.IsNullOrEmpty(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                byte[] body;
                string note;

                if (d.FileName.EndsWith(".vag", StringComparison.OrdinalIgnoreCase))
                {
                    try { body = File.ReadAllBytes(d.FileName); }
                    catch (Exception ex)
                    {
                        MessageBox.Show(this, ex.Message, Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                    if ((body.Length & 0x0F) != 0)
                    {
                        MessageBox.Show(this,
                            Loc.F("That .vag is {0} bytes, which is not a whole number of " +
                                  "16-byte ADPCM blocks. It is probably not raw PSX ADPCM — a .vag with a " +
                                  "48-byte header needs that header stripped first.", body.Length),
                            Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                    note = Loc.F("raw ADPCM, {0} bytes", body.Length.ToString("N0"));
                }
                else
                {
                    int srcRate;
                    string err;
                    short[] pcm = VagEncoder.ReadWav(d.FileName, out srcRate, out err);
                    if (pcm == null)
                    {
                        MessageBox.Show(this, err, Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }

                    int target = (int)Math.Round(RateFor(vag));
                    short[] resampled = VagEncoder.Resample(pcm, srcRate, target);

                    var opt = new VagEncoder.Options { Loop = vag.Loops };
                    body = VagEncoder.Encode(resampled, opt);

                    note = Loc.F("{0} Hz WAV resampled to {1} Hz, {2:N0} bytes", srcRate, target, body.Length) +
                           (vag.Loops ? ", " + Loc.T("looping") : "");
                }

                if (body.Length > VabFile.MaxVagBytes)
                {
                    MessageBox.Show(this,
                        Loc.F("That would be {0} bytes. A single sample cannot exceed {1} — the bank stores " +
                              "each length as length/8 in 16 bits.",
                              body.Length.ToString("N0"), VabFile.MaxVagBytes.ToString("N0")),
                        Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                _pending[vag.Index] = body;
                _lastDir = Path.GetDirectoryName(d.FileName);
                _info.Text = Loc.F("Sample {0} staged: {1} (was {2}). Save bank… to write it out.",
                                   vag.Index, note, vag.Length.ToString("N0"));

                List<SampleRef> others;
                if (_dupes.TryGetValue(vag.Index, out others) && others.Count > 0)
                {
                    _info.Text += " " + Loc.F("The same sound is also in {0} — saving offers to update those too.",
                                              BankDetail(others));
                }
                MarkPending();
            }
        }

        private void RevertSelected()
        {
            foreach (ListViewItem it in _list.SelectedItems)
                _pending.Remove(((VabVag)it.Tag).Index);
            _info.Text = _pending.Count == 0
                ? Loc.T("All replacements reverted.")
                : Loc.F("{0} replacement(s) still staged.", _pending.Count);
            MarkPending();
        }

        /// <summary>Bold + a marker on rows with a staged replacement, so it is obvious
        /// what will change before the bank is written.</summary>
        private void MarkPending()
        {
            foreach (ListViewItem it in _list.Items)
            {
                var vag = (VabVag)it.Tag;
                byte[] body;
                bool staged = _pending.TryGetValue(vag.Index, out body);
                it.Font = new Font(_list.Font, staged ? FontStyle.Bold : FontStyle.Regular);

                // The whole size group follows the staged body, so the row describes
                // the sound Play will actually produce.
                int bytes = staged ? body.Length : vag.Length;
                int samples = bytes / 16 * 28;
                it.SubItems[1].Text = bytes.ToString("N0");
                it.SubItems[2].Text = samples.ToString("N0");
                it.SubItems[3].Text = (samples / RateFor(vag)).ToString("0.00") + "s";
            }
            UpdateButtons();
        }

        /// <summary>The PCM a row stands for right now: the staged replacement when
        /// there is one, otherwise the bank's own sample.</summary>
        private short[] SamplesFor(VabVag vag)
        {
            byte[] body;
            if (_pending.TryGetValue(vag.Index, out body)) return VabFile.DecodeAdpcm(body, 0, body.Length);
            return _vab.Decode(vag.Index);
        }

        private string LoadSndDir
        {
            get
            {
                return string.IsNullOrEmpty(_gameRoot)
                    ? null
                    : Path.Combine(Path.Combine(Path.Combine(_gameRoot, "gamedata"), "load"), "SND");
            }
        }

        /// <summary>Save is one dialog: this bank plus every bank sharing the replaced
        /// samples, a destination folder, and a source per row. No file picker first —
        /// the destination is a folder because every bank in it is named by the game,
        /// and the source is what lets a second round of edits merge into the first.</summary>
        private void SaveBank()
        {
            if (_vab == null || _pending.Count == 0) return;

            string editedBank = BaseName;
            List<DuplicateTarget> targets = PlanTargets();

            string loadSnd = LoadSndDir;
            string folder = loadSnd ?? _lastDir ?? Path.GetDirectoryName(_vab.Path);

            using (var dlg = new DuplicateSoundsDialog(editedBank, targets, folder, loadSnd))
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
            }

            // Overwriting the bank being read would invalidate every offset the open
            // view still points at, so it is reloaded afterwards; but say so first.
            foreach (DuplicateTarget t in targets)
            {
                if (!t.Selected || t.DestPath == null) continue;
                if (string.Equals(Path.GetFullPath(t.DestPath), Path.GetFullPath(_vab.Path),
                        StringComparison.OrdinalIgnoreCase))
                {
                    if (MessageBox.Show(this,
                            Loc.T("That folder holds the bank currently open. Overwrite it and reload?"),
                            Loc.T("Audio"), MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                        return;
                    break;
                }
            }

            var written = new List<string>();
            var failed = new List<string>();
            string primaryDest = null;
            foreach (DuplicateTarget t in targets)
            {
                if (!t.Selected) continue;
                string err;
                if (t.Write(out err))
                {
                    written.Add(t.Bank);
                    if (t.IsPrimary) primaryDest = t.DestPath;
                }
                else
                {
                    failed.Add(t.Bank + ": " + err);
                }
            }

            if (failed.Count > 0)
            {
                MessageBox.Show(this,
                    Loc.T("Some banks could not be written:") + "\n\n" + string.Join("\n", failed.ToArray()),
                    Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            if (primaryDest == null) return;

            int n = _pending.Count;
            _pending.Clear();
            Open(primaryDest);
            _info.Text = written.Count > 1
                ? Loc.F("Wrote {0} with {1} replaced sample(s), and the same sounds into {2}.",
                        Path.GetFileName(primaryDest), n, string.Join(", ", written.GetRange(1, written.Count - 1).ToArray()))
                : Loc.F("Wrote {0} with {1} replaced sample(s).", Path.GetFileName(primaryDest), n);
        }

        /// <summary>The bank being edited first, then for every staged replacement the
        /// other banks holding the sample it replaces, grouped per bank so each is
        /// rewritten once. Sources and destinations are the dialog's business; only the
        /// pristine path each rewrite falls back to is decided here.</summary>
        private List<DuplicateTarget> PlanTargets()
        {
            var byBank = new Dictionary<string, DuplicateTarget>(StringComparer.OrdinalIgnoreCase);
            var others = new List<DuplicateTarget>();
            var primary = new DuplicateTarget { Bank = BaseName, IsPrimary = true, OriginalPath = _vab.Path };

            foreach (KeyValuePair<int, byte[]> kv in _pending)
            {
                if (kv.Key < 1 || kv.Key > _vab.VagCount) continue;
                byte[] original;
                if (!_originals.TryGetValue(kv.Key, out original)) original = _vab.RawVag(kv.Key);
                string key = SndSampleIndex.Key(original);

                primary.Items.Add(new DuplicateItem
                {
                    TargetIndex = kv.Key,
                    SourceIndex = kv.Key,
                    Body = kv.Value,
                    OriginalKey = key,
                });

                if (_index == null) continue;
                foreach (SampleRef r in _index.Others(original, BaseName))
                {
                    DuplicateTarget t;
                    if (!byBank.TryGetValue(r.Bank, out t))
                    {
                        t = new DuplicateTarget { Bank = r.Bank, OriginalPath = r.Path };
                        byBank[r.Bank] = t;
                        others.Add(t);
                    }
                    t.Items.Add(new DuplicateItem
                    {
                        TargetIndex = r.Index,
                        SourceIndex = kv.Key,
                        Body = kv.Value,
                        OriginalKey = key,
                    });
                }
            }

            others.Sort((a, b) => string.Compare(a.Bank, b.Bank, StringComparison.OrdinalIgnoreCase));
            var targets = new List<DuplicateTarget> { primary };
            targets.AddRange(others);
            foreach (DuplicateTarget t in targets) t.Items.Sort((a, b) => a.TargetIndex.CompareTo(b.TargetIndex));
            return targets;
        }

        private VabVag Selected
        {
            get { return _list.SelectedItems.Count > 0 ? (VabVag)_list.SelectedItems[0].Tag : null; }
        }

        private void PlaySelected()
        {
            VabVag vag = Selected;
            if (_vab == null || vag == null) return;

            StopPlayback();
            try
            {
                short[] pcm = SamplesFor(vag);
                if (pcm.Length == 0)
                {
                    MessageBox.Show(this, Loc.T("That sample decoded to nothing — its first block is an end marker."),
                        Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                byte[] wav = VabFile.BuildWav(pcm, (int)Math.Round(RateFor(vag)));
                _player = new SoundPlayer(new MemoryStream(wav));
                _player.Play();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, Loc.F("Could not play that sample:\n\n{0}", ex.Message),
                    Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            UpdateButtons();
        }

        private void StopPlayback()
        {
            if (_player == null) return;
            try { _player.Stop(); } catch { }
            _player.Dispose();
            _player = null;
            UpdateButtons();
        }

        private string BaseName { get { return Path.GetFileNameWithoutExtension(_vab.Path); } }

        private void ExportSelected(bool raw)
        {
            if (_vab == null || _list.SelectedItems.Count == 0) return;

            if (_list.SelectedItems.Count == 1)
            {
                var vag = (VabVag)_list.SelectedItems[0].Tag;
                using (var d = new SaveFileDialog())
                {
                    d.Title = Loc.T(raw ? "Export raw ADPCM" : "Export WAV");
                    d.Filter = raw ? Loc.T("Raw PSX ADPCM") + " (*.vag)|*.vag" : Loc.T("WAV audio") + " (*.wav)|*.wav";
                    d.FileName = SoundFileName(vag, raw);
                    if (!string.IsNullOrEmpty(_lastDir)) d.InitialDirectory = _lastDir;
                    if (d.ShowDialog(this) != DialogResult.OK) return;
                    if (!WriteOne(vag, d.FileName, raw)) return;
                }
                _info.Text = Loc.T("Exported 1 sample.");
                return;
            }

            string dir = PickFolder(Loc.T("Choose a folder for the exported samples"));
            if (dir == null) return;
            int n = 0;
            foreach (ListViewItem it in _list.SelectedItems)
            {
                var vag = (VabVag)it.Tag;
                if (WriteOne(vag, Path.Combine(dir, SoundFileName(vag, raw)), raw)) n++;
            }
            _info.Text = Loc.F("Exported {0} samples to {1}", n, dir);
        }

        private void ExportAll()
        {
            if (_vab == null) return;
            string dir = PickFolder(Loc.T("Choose a folder for the whole bank"));
            if (dir == null) return;

            int n = 0, skipped = 0;
            foreach (VabVag vag in _vab.Vags)
            {
                if (WriteOne(vag, Path.Combine(dir, SoundFileName(vag, false)), false)) n++;
                else skipped++;
            }
            _info.Text = Loc.F("Exported {0} samples to {1}", n, dir) + (skipped > 0 ? " " + Loc.F("({0} failed)", skipped) : "");
        }

        /// <summary>Zero-padded so a folder of exports sorts in bank order, and prefixed
        /// with the bank name so exports from several banks can share one folder.</summary>
        private string SoundFileName(VabVag vag, bool raw)
        {
            /* A dot before the number, so an exported .wav is already named the
             * way the loose-file loader looks for it (gamedata/load/SND/) and an
             * export -> edit -> drop-in round trip just works. The runtime still
             * accepts the old underscore form, so files exported before this
             * keep loading. The .vag export is not a loose-file name, but it is
             * kept consistent so both halves of the tool read the same. */
            return BaseName + "." + vag.Index.ToString("000") + (raw ? ".vag" : ".wav");
        }

        private bool WriteOne(VabVag vag, string path, bool raw)
        {
            try
            {
                if (raw)
                {
                    File.WriteAllBytes(path, _vab.RawVag(vag.Index));
                }
                else
                {
                    short[] pcm = _vab.Decode(vag.Index);
                    File.WriteAllBytes(path, VabFile.BuildWav(pcm, (int)Math.Round(RateFor(vag))));
                }
                return true;
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, Loc.F("Could not write:\n{0}\n\n{1}", path, ex.Message),
                    Loc.T("Audio"), MessageBoxButtons.OK, MessageBoxIcon.Error);
                return false;
            }
        }

        private string PickFolder(string desc)
        {
            using (var d = new FolderBrowserDialog())
            {
                d.Description = desc;
                if (!string.IsNullOrEmpty(_lastDir) && Directory.Exists(_lastDir)) d.SelectedPath = _lastDir;
                return d.ShowDialog(this) == DialogResult.OK ? d.SelectedPath : null;
            }
        }

        private void ShowHelp()
        {
            var lines = new List<string>
            {
                "Sound banks (.VAB) hold every discrete sound in the game — footsteps,",
                "weapons, monster cries, Harry's voice — as PSX ADPCM samples. The 90",
                "banks live in SND/ inside an extracted disc.",
                "",
                "A sound id routes through the bank like this:",
                "    sfxId -> bank -> program -> tone -> sample",
                "Everything above the sample is routing; the sample is what you replace.",
                "Several tones can share one sample, which is why the same sound can appear",
                "at more than one pitch. A sample listed as \"(unused)\" is in the bank but",
                "no tone references it.",
                "",
                "Sound ids: a sound id names a SLOT and a program, never a bank. The game",
                "keeps four slots filled, and their occupants change as you play: the",
                "weapon slot follows the equipped weapon, the ambient slot follows the",
                "map. So the ids listed are the ones that play this sample while THIS",
                "bank is the loaded bank for its slot. In an area that loads another",
                "bank, the same id plays that bank's sample instead, and your",
                "replacement here does not affect it. Replacing a sample never removes",
                "a sound: every id listed keeps playing, with the new audio.",
                "",
                "The list is long for a sound many ids can trigger. Hover a row to see",
                "all of them. An id shown as \"+2\" keys on two further samples at the",
                "same time, so this sample is one layer of that sound.",
                "",
                "Preview rate: the SPU plays a sample at 44100 Hz when it is triggered at",
                "the tone's own centre note, and the game shifts the pitch per sound from",
                "the note stored in its sound table. So a preview is the sample's natural",
                "rate, not necessarily what you hear in game. Drop the rate if a sound",
                "seems too fast.",
                "",
                "\"Loops\" marks samples whose ADPCM blocks carry loop flags — sustained",
                "sounds like radio static and ambience. Those flags live in the compressed",
                "data, so a WAV round trip through a naive encoder loses them.",
                "",
                "Export WAV gives you editable audio. Export raw VAG gives you the exact",
                "compressed bytes, for when you want to re-inject them untouched.",
                "",
                "\"Also in\" lists the other banks that hold a byte-identical copy of the",
                "sample. The disc pastes a monster's sounds into the ambient bank of every",
                "map it appears in (the Groaner block is in eight), and the game loads one",
                "ambient bank per map — so a replacement made in MAP200 alone plays only in",
                "the areas that load MAP200.",
                "",
                "Save bank… writes this bank and, ticked per row, every bank sharing the",
                "replaced sounds into one folder (gamedata\\load\\SND by default, where the",
                "game reads them). Each row has a Source, the file the rewrite starts from.",
                "A copy already in the folder is the source automatically, so a second",
                "round of edits merges into the first instead of overwriting it. A bank",
                "that holds your earlier edit of the same sound starts ticked; one that",
                "holds a different sound of yours stays unticked.",
                "",
                "You can open either the pristine bank or your edited copy. The tool",
                "compares the open bank with the same bank in a clean extract's SND",
                "folder (File > Clean SND folder…), marks the samples you have already",
                "changed with * in the # column, and still finds their other copies",
                "from the disc's sound. Bold rows are replacements not saved yet.",
            };
            string text = Loc.T(string.Join("\n", lines.ToArray()));
            ConverterActions.ShowTextDialog(this, Loc.T("Audio — About sound banks"), text.Split('\n'), false);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            StopPlayback();
            base.OnFormClosed(e);
        }
    }
}
