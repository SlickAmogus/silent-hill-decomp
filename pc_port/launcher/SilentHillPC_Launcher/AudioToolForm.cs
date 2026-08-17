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
        private readonly Dictionary<int, List<SfxMatch>> _matches = new Dictionary<int, List<SfxMatch>>();

        public static void ShowTool(IWin32Window owner, string startDir)
        {
            AudioToolForm f = s_open;
            if (f == null || f.IsDisposed)
            {
                f = new AudioToolForm();
                s_open = f;
                f.FormClosed += (s, e) => { if (s_open == f) s_open = null; };
                f._lastDir = startDir;
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
            Text = "Audio";
            ClientSize = new Size(760, 460);
            StartPosition = FormStartPosition.CenterParent;
            MinimumSize = new Size(620, 360);
            AllowDrop = true;

            var menu = new MenuStrip();
            var file = new ToolStripMenuItem("&File");
            file.DropDownItems.Add("&Open sound bank…", null, (s, e) => PickAndOpen());
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("E&xit", null, (s, e) => Close());
            var help = new ToolStripMenuItem("&Help");
            help.DropDownItems.Add("About sound banks…", null, (s, e) => ShowHelp());
            menu.Items.Add(file);
            menu.Items.Add(help);
            MainMenuStrip = menu;
            Controls.Add(menu);

            _list.View = View.Details;
            _list.FullRowSelect = true;
            _list.MultiSelect = true;
            _list.HideSelection = false;
            _list.GridLines = true;
            _list.Location = new Point(12, 30);
            _list.Size = new Size(736, 300);
            _list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;
            _list.Columns.Add("#", 44, HorizontalAlignment.Right);
            _list.Columns.Add("Bytes", 74, HorizontalAlignment.Right);
            _list.Columns.Add("Samples", 78, HorizontalAlignment.Right);
            _list.Columns.Add("Length", 70, HorizontalAlignment.Right);
            _list.Columns.Add("Loops", 52, HorizontalAlignment.Center);
            _list.Columns.Add("In-game rate", 88, HorizontalAlignment.Right);
            _list.Columns.Add("Sound ids", 240, HorizontalAlignment.Left);
            _list.Columns.Add("Programs", 76, HorizontalAlignment.Left);
            _list.Columns.Add("Centre note", 82, HorizontalAlignment.Right);
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.DoubleClick += (s, e) => PlaySelected();
            Controls.Add(_list);

            _info.Location = new Point(12, 338);
            _info.Size = new Size(736, 34);
            _info.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            _info.Text = "No bank loaded — File > Open, or drag a .VAB onto this window.";
            Controls.Add(_info);

            int y = 380;
            SetupButton(_btnPlay, "Play", new Point(12, y), (s, e) => PlaySelected());
            SetupButton(_btnStop, "Stop", new Point(100, y), (s, e) => StopPlayback());
            SetupButton(_btnWav, "Export WAV…", new Point(188, y), (s, e) => ExportSelected(false));
            SetupButton(_btnVag, "Export raw VAG…", new Point(300, y), (s, e) => ExportSelected(true));
            SetupButton(_btnAll, "Export all…", new Point(430, y), (s, e) => ExportAll());
            SetupButton(_btnRep, "Replace…", new Point(12, y + 30), (s, e) => ReplaceSelected());
            SetupButton(_btnRevert, "Revert", new Point(100, y + 30), (s, e) => RevertSelected());
            SetupButton(_btnSave, "Save bank as…", new Point(188, y + 30), (s, e) => SaveBank());

            var sl = new Label
            {
                Text = "Bank slot:",
                Location = new Point(548, y - 24),
                Size = new Size(78, 20),
                Anchor = AnchorStyles.Bottom | AnchorStyles.Left
            };
            Controls.Add(sl);
            _slot.DropDownStyle = ComboBoxStyle.DropDownList;
            _slot.Location = new Point(628, y - 28);
            _slot.Size = new Size(88, 22);
            _slot.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            _slot.Items.AddRange(new object[] { "Any", "base", "weapon", "ambient", "music" });
            _slot.SelectedIndex = 0;
            _slot.SelectedIndexChanged += (s, e) => { if (_vab != null) Open(_vab.Path); };
            Controls.Add(_slot);

            var rl = new Label
            {
                Text = "Preview rate:",
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
            _rate.SelectedIndex = 0;
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
            get { return _slot.SelectedIndex <= 0 ? -1 : _slot.SelectedIndex - 1; }
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
                d.Title = "Open a sound bank";
                d.Filter = "PSX sound banks (*.vab)|*.vab|All files (*.*)|*.*";
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
                MessageBox.Show(this, err, "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
            Text = "Audio — " + Path.GetFileName(path);

            _matches.Clear();
            _list.BeginUpdate();
            _list.Items.Clear();
            int identified = 0;
            foreach (VabVag vag in v.Vags)
            {
                var progs = new List<int>();
                int centre = -1;
                foreach (VabTone t in vag.Tones)
                {
                    if (!progs.Contains(t.Program)) progs.Add(t.Program);
                    if (centre < 0) centre = t.CenterNote;
                }
                progs.Sort();

                List<SfxMatch> hits = v.MatchesFor(vag.Index, SlotFilter);
                _matches[vag.Index] = hits;
                if (hits.Count > 0) identified++;

                // Duration follows the rate it actually plays at, so a sound pitched
                // down reads as the longer sound the player hears.
                double rate = RateFor(vag);
                int samples = vag.BlockCount * 28;
                double secs = samples / rate;

                var names = new List<string>();
                var rates = new List<string>();
                foreach (SfxMatch m in hits)
                {
                    // The slot is part of the identity when the filter is off: the same
                    // program/note pair exists in banks loaded into different slots.
                    string label = SlotFilter >= 0
                        ? m.Label
                        : m.Label + " (" + SfxMatch.SlotName(m.Row.Slot) + ")";
                    if (!names.Contains(label)) names.Add(label);
                    string r = Math.Round(m.RateHz).ToString("N0");
                    if (!rates.Contains(r)) rates.Add(r);
                }

                var it = new ListViewItem(vag.Index.ToString());
                it.SubItems.Add(vag.Length.ToString("N0"));
                it.SubItems.Add(samples.ToString("N0"));
                it.SubItems.Add(secs.ToString("0.00") + "s");
                it.SubItems.Add(vag.Loops ? "yes" : "");
                it.SubItems.Add(rates.Count == 0 ? "-" :
                    (rates.Count == 1 ? rates[0] + " Hz" : string.Join(" / ", rates.ToArray()) + " Hz"));
                it.SubItems.Add(names.Count == 0 ? "" : string.Join(", ", names.ToArray()));
                it.SubItems.Add(progs.Count == 0 ? "(unused)" : string.Join(", ", progs.ConvertAll(x => x.ToString()).ToArray()));
                it.SubItems.Add(centre < 0 ? "-" : centre.ToString());
                it.Tag = vag;
                if (progs.Count == 0) it.ForeColor = SystemColors.GrayText;
                _list.Items.Add(it);
            }
            _list.EndUpdate();

            _info.Text = string.Format(
                "{0} samples, {1} programs, {2} tones — bank id {3}, {4:N0} bytes. " +
                "{5} identified from the game's sound table; the rest are in the bank but no " +
                "sound id in this slot reaches them.",
                v.VagCount, v.ProgramCount, v.Tones.Count, v.VabId, v.DeclaredSize, identified);

            string twin = LoadedTwinBank(path);
            if (twin != null)
            {
                _info.Text = "The game loads " + twin + " instead of this bank — same sounds, and "
                           + "this one is never requested. The port accepts either name, so an "
                           + "export from here still works; name it " + twin + "_005.wav to be "
                           + "explicit.\r\n" + _info.Text;
                _info.ForeColor = Color.FromArgb(255, 170, 90);
            }
            else
            {
                _info.ForeColor = ForeColor;
            }

            if (_list.Items.Count > 0) _list.Items[0].Selected = true;
            MarkPending();
        }

        /* Seven SND banks exist on the disc but are absent from the sound system's
         * own table (g_AudioData[].fileOffset_8), which is what identifies a bank
         * when it loads. Nothing ever requests them, so a replacement aimed at one
         * cannot fire however it is named or resampled — and this tool used to hand
         * out an export name for them like any other, which is how MAP000_005.wav
         * came to be a reasonable-looking file that did nothing.
         *
         * Derived by pairing every SND/*.VAB in filetable.c.USA.inc against that
         * table: 83 of 90 are reachable, these are not. Names, not sectors, so it
         * holds for every region. */
        private static readonly string[] MapOnlyBanks =
        {
            "MAP000", "MAP100", "MAP101", "MAP102", "MAP103", "MAP502", "MAP604",
        };

        /// <summary>The MEP twin the game loads in place of this bank, or null.</summary>
        private static string LoadedTwinBank(string path)
        {
            if (string.IsNullOrEmpty(path)) return null;
            string stem = Path.GetFileNameWithoutExtension(path);
            foreach (string b in MapOnlyBanks)
                if (string.Equals(stem, b, StringComparison.OrdinalIgnoreCase))
                    return "MEP" + b.Substring(3);
            return null;
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
                d.Title = "Replace sample " + vag.Index;
                d.Filter = "Audio (*.wav;*.vag)|*.wav;*.vag|WAV audio (*.wav)|*.wav|Raw PSX ADPCM (*.vag)|*.vag";
                if (!string.IsNullOrEmpty(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                byte[] body;
                string note;

                if (d.FileName.EndsWith(".vag", StringComparison.OrdinalIgnoreCase))
                {
                    try { body = File.ReadAllBytes(d.FileName); }
                    catch (Exception ex)
                    {
                        MessageBox.Show(this, ex.Message, "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                    if ((body.Length & 0x0F) != 0)
                    {
                        MessageBox.Show(this,
                            "That .vag is " + body.Length + " bytes, which is not a whole number of " +
                            "16-byte ADPCM blocks. It is probably not raw PSX ADPCM — a .vag with a " +
                            "48-byte header needs that header stripped first.",
                            "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                    note = "raw ADPCM, " + body.Length.ToString("N0") + " bytes";
                }
                else
                {
                    int srcRate;
                    string err;
                    short[] pcm = VagEncoder.ReadWav(d.FileName, out srcRate, out err);
                    if (pcm == null)
                    {
                        MessageBox.Show(this, err, "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }

                    int target = (int)Math.Round(RateFor(vag));
                    short[] resampled = VagEncoder.Resample(pcm, srcRate, target);

                    var opt = new VagEncoder.Options { Loop = vag.Loops };
                    body = VagEncoder.Encode(resampled, opt);

                    note = string.Format("{0} Hz WAV resampled to {1} Hz, {2:N0} bytes{3}",
                        srcRate, target, body.Length, vag.Loops ? ", looping" : "");
                }

                if (body.Length > VabFile.MaxVagBytes)
                {
                    MessageBox.Show(this,
                        "That would be " + body.Length.ToString("N0") + " bytes. A single sample " +
                        "cannot exceed " + VabFile.MaxVagBytes.ToString("N0") + " — the bank stores " +
                        "each length as length/8 in 16 bits.",
                        "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                _pending[vag.Index] = body;
                _lastDir = Path.GetDirectoryName(d.FileName);
                _info.Text = "Sample " + vag.Index + " staged: " + note +
                             " (was " + vag.Length.ToString("N0") + "). Save bank as… to write it out.";
                MarkPending();
            }
        }

        private void RevertSelected()
        {
            foreach (ListViewItem it in _list.SelectedItems)
                _pending.Remove(((VabVag)it.Tag).Index);
            _info.Text = _pending.Count == 0
                ? "All replacements reverted."
                : _pending.Count + " replacement(s) still staged.";
            MarkPending();
        }

        /// <summary>Bold + a marker on rows with a staged replacement, so it is obvious
        /// what will change before the bank is written.</summary>
        private void MarkPending()
        {
            foreach (ListViewItem it in _list.Items)
            {
                var vag = (VabVag)it.Tag;
                bool staged = _pending.ContainsKey(vag.Index);
                it.Font = new Font(_list.Font, staged ? FontStyle.Bold : FontStyle.Regular);
                if (staged)
                {
                    it.SubItems[1].Text = _pending[vag.Index].Length.ToString("N0") + " *";
                }
                else if (it.SubItems[1].Text.EndsWith(" *"))
                {
                    it.SubItems[1].Text = vag.Length.ToString("N0");
                }
            }
            UpdateButtons();
        }

        private void SaveBank()
        {
            if (_vab == null || _pending.Count == 0) return;

            string err;
            byte[] rebuilt = _vab.Rebuild(_pending, out err);
            if (rebuilt == null)
            {
                MessageBox.Show(this, err, "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            using (var d = new SaveFileDialog())
            {
                d.Title = "Save sound bank";
                d.Filter = "PSX sound banks (*.vab)|*.vab";
                d.FileName = Path.GetFileName(_vab.Path);
                if (!string.IsNullOrEmpty(_lastDir)) d.InitialDirectory = _lastDir;
                if (d.ShowDialog(this) != DialogResult.OK) return;

                // Overwriting the bank being read would invalidate every offset the
                // open view still points at.
                if (string.Equals(Path.GetFullPath(d.FileName), Path.GetFullPath(_vab.Path),
                        StringComparison.OrdinalIgnoreCase))
                {
                    if (MessageBox.Show(this,
                            "That is the bank currently open. Overwrite it and reload?",
                            "Audio", MessageBoxButtons.OKCancel, MessageBoxIcon.Warning) != DialogResult.OK)
                        return;
                }

                try { File.WriteAllBytes(d.FileName, rebuilt); }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Could not write:\n" + d.FileName + "\n\n" + ex.Message,
                        "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }

                int n = _pending.Count;
                _pending.Clear();
                Open(d.FileName);
                _info.Text = "Wrote " + Path.GetFileName(d.FileName) + " with " + n + " replaced sample(s).";
            }
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
                short[] pcm = _vab.Decode(vag.Index);
                if (pcm.Length == 0)
                {
                    MessageBox.Show(this, "That sample decoded to nothing — its first block is an end marker.",
                        "Audio", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                    return;
                }
                byte[] wav = VabFile.BuildWav(pcm, (int)Math.Round(RateFor(vag)));
                _player = new SoundPlayer(new MemoryStream(wav));
                _player.Play();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Could not play that sample:\n\n" + ex.Message,
                    "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
                    d.Title = raw ? "Export raw ADPCM" : "Export WAV";
                    d.Filter = raw ? "Raw PSX ADPCM (*.vag)|*.vag" : "WAV audio (*.wav)|*.wav";
                    d.FileName = SoundFileName(vag, raw);
                    if (!string.IsNullOrEmpty(_lastDir)) d.InitialDirectory = _lastDir;
                    if (d.ShowDialog(this) != DialogResult.OK) return;
                    if (!WriteOne(vag, d.FileName, raw)) return;
                }
                _info.Text = "Exported 1 sample.";
                return;
            }

            string dir = PickFolder("Choose a folder for the exported samples");
            if (dir == null) return;
            int n = 0;
            foreach (ListViewItem it in _list.SelectedItems)
            {
                var vag = (VabVag)it.Tag;
                if (WriteOne(vag, Path.Combine(dir, SoundFileName(vag, raw)), raw)) n++;
            }
            _info.Text = "Exported " + n + " samples to " + dir;
        }

        private void ExportAll()
        {
            if (_vab == null) return;
            string dir = PickFolder("Choose a folder for the whole bank");
            if (dir == null) return;

            int n = 0, skipped = 0;
            foreach (VabVag vag in _vab.Vags)
            {
                if (WriteOne(vag, Path.Combine(dir, SoundFileName(vag, false)), false)) n++;
                else skipped++;
            }
            _info.Text = "Exported " + n + " samples to " + dir + (skipped > 0 ? " (" + skipped + " failed)" : "");
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
                MessageBox.Show(this, "Could not write:\n" + path + "\n\n" + ex.Message,
                    "Audio", MessageBoxButtons.OK, MessageBoxIcon.Error);
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
            };
            ConverterActions.ShowTextDialog(this, "Audio — About sound banks", lines.ToArray(), false);
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            StopPlayback();
            base.OnFormClosed(e);
        }
    }
}
