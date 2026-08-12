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
        private readonly ComboBox _rate = new ComboBox();

        private VabFile _vab;
        private SoundPlayer _player;
        private string _lastDir;

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
            _list.Columns.Add("Programs", 90, HorizontalAlignment.Left);
            _list.Columns.Add("Centre note", 82, HorizontalAlignment.Right);
            _list.Columns.Add("Used by tones", 100, HorizontalAlignment.Right);
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
            _rate.Items.AddRange(new object[] { "44100 Hz", "32000 Hz", "22050 Hz", "16000 Hz", "11025 Hz", "8000 Hz" });
            _rate.SelectedIndex = 0;
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

        private int PreviewRate
        {
            get
            {
                string s = _rate.SelectedItem as string;
                if (s == null) return VabFile.UnityRate;
                int sp = s.IndexOf(' ');
                int r;
                return int.TryParse(sp > 0 ? s.Substring(0, sp) : s, out r) ? r : VabFile.UnityRate;
            }
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

            _vab = v;
            _lastDir = Path.GetDirectoryName(path);
            Text = "Audio — " + Path.GetFileName(path);

            _list.BeginUpdate();
            _list.Items.Clear();
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

                int samples = vag.BlockCount * 28;
                double secs = samples / (double)VabFile.UnityRate;

                var it = new ListViewItem(vag.Index.ToString());
                it.SubItems.Add(vag.Length.ToString("N0"));
                it.SubItems.Add(samples.ToString("N0"));
                it.SubItems.Add(secs.ToString("0.00") + "s");
                it.SubItems.Add(vag.Loops ? "yes" : "");
                it.SubItems.Add(progs.Count == 0 ? "(unused)" : string.Join(", ", progs.ConvertAll(x => x.ToString()).ToArray()));
                it.SubItems.Add(centre < 0 ? "-" : centre.ToString());
                it.SubItems.Add(vag.Tones.Count.ToString());
                it.Tag = vag;
                if (progs.Count == 0) it.ForeColor = SystemColors.GrayText;
                _list.Items.Add(it);
            }
            _list.EndUpdate();

            _info.Text = string.Format(
                "{0} samples, {1} programs, {2} tones — bank id {3}, {4:N0} bytes. " +
                "Length and preview assume the SPU's unity rate ({5} Hz); the game re-pitches each sound per note.",
                v.VagCount, v.ProgramCount, v.Tones.Count, v.VabId, v.DeclaredSize, VabFile.UnityRate);

            if (_list.Items.Count > 0) _list.Items[0].Selected = true;
            UpdateButtons();
        }

        private void UpdateButtons()
        {
            bool any = _vab != null && _list.SelectedItems.Count > 0;
            _btnPlay.Enabled = any && _list.SelectedItems.Count == 1;
            _btnWav.Enabled = any;
            _btnVag.Enabled = any;
            _btnAll.Enabled = _vab != null;
            _btnStop.Enabled = _player != null;
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
                byte[] wav = VabFile.BuildWav(pcm, PreviewRate);
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
            return BaseName + "_" + vag.Index.ToString("000") + (raw ? ".vag" : ".wav");
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
                    File.WriteAllBytes(path, VabFile.BuildWav(pcm, PreviewRate));
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
