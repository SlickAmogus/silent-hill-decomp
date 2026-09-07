using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Media;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Voices (XA) tool: every voice line on the disc, playable and exportable, with
    /// replacements dropped into gamedata/load/XA/xa_NNNN.wav where the game's XA
    /// player picks them up instead of the disc stream (see xa_player.c). A line can
    /// be re-recorded on the spot from the microphone. One reusable window, like the
    /// Audio (VAB) tool.
    ///
    /// Lines are found the way the game finds them: the XA file's start sector comes
    /// from the disc's own file table, the line's sector and authored length from
    /// g_XaItemData (XaTable.cs, generated), and the (file, channel) pair that picks
    /// this line out of the interleaved stream from the first sector's subheader.
    /// </summary>
    internal sealed class XaToolForm : Form
    {
        private static XaToolForm s_open;

        private readonly string   _gameRoot;
        private string            _binPath;
        private int[]             _xaSectors;        // index 1..9, disc-absolute
        private readonly ListView _list  = new ListView();
        private readonly Label    _info  = new Label();
        private readonly Button   _btnPlay   = new Button();
        private readonly Button   _btnOrig   = new Button();
        private readonly Button   _btnStop   = new Button();
        private readonly Button   _btnExport = new Button();
        private readonly Button   _btnImport = new Button();
        private readonly Button   _btnRecord = new Button();
        private readonly Button   _btnRemove = new Button();
        private readonly Button   _btnFolder = new Button();
        private readonly Button   _btnLast   = new Button();
        private SoundPlayer       _player;
        private bool              _recording;
        private int               _recIdx = -1;
        private bool              _textMode;
        private ToolStripComboBox _mode;

        [DllImport("winmm.dll", CharSet = CharSet.Auto)]
        private static extern int mciSendString(string command, StringBuilder ret, int retLen, IntPtr hwnd);

        public static void ShowTool(IWin32Window owner, string gameRoot)
        {
            XaToolForm f = s_open;
            if (f == null || f.IsDisposed)
            {
                f = new XaToolForm(gameRoot);
                s_open = f;
                f.FormClosed += (s, e) => { if (s_open == f) s_open = null; };
                f.Show(owner);
            }
            else
            {
                if (f.WindowState == FormWindowState.Minimized) f.WindowState = FormWindowState.Normal;
                f.BringToFront();
                f.Activate();
            }
        }

        private XaToolForm(string gameRoot)
        {
            _gameRoot = gameRoot;
            Text = "Voices";
            ClientSize = new Size(900, 480);
            StartPosition = FormStartPosition.CenterParent;
            MinimumSize = new Size(640, 380);

            var menu = new MenuStrip();
            var file = new ToolStripMenuItem("&File");
            file.DropDownItems.Add("&Refresh", null, (s, e) => Populate());
            file.DropDownItems.Add("Open &load\\XA folder", null, (s, e) => OpenFolder());
            file.DropDownItems.Add(new ToolStripSeparator());
            file.DropDownItems.Add("E&xit", null, (s, e) => Close());
            var help = new ToolStripMenuItem("&Help");
            help.DropDownItems.Add("About voice mods…", null, (s, e) => ShowHelp());
            menu.Items.Add(file);
            menu.Items.Add(help);
            _mode = new ToolStripComboBox();
            _mode.DropDownStyle = ComboBoxStyle.DropDownList;
            _mode.Items.Add("Voice lines (disc)");
            _mode.Items.Add("Text boxes (unvoiced)");
            _mode.SelectedIndex = 0;
            _mode.Width = 170;
            _mode.Alignment = ToolStripItemAlignment.Right;
            _mode.SelectedIndexChanged += (s, e) => { StopPlayback(); _textMode = _mode.SelectedIndex == 1; Populate(); };
            menu.Items.Add(_mode);
            MainMenuStrip = menu;
            Controls.Add(menu);

            _list.View = View.Details;
            _list.FullRowSelect = true;
            _list.MultiSelect = false;
            _list.HideSelection = false;
            _list.GridLines = true;
            _list.Location = new Point(12, 30);
            _list.Size = new Size(876, 320);
            _list.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom;
            _list.SelectedIndexChanged += (s, e) => UpdateButtons();
            _list.DoubleClick += (s, e) => PlaySelected(false);
            Controls.Add(_list);

            _info.Location = new Point(12, 358);
            _info.Size = new Size(876, 34);
            _info.Anchor = AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            Controls.Add(_info);

            int y = 400;
            SetupButton(_btnPlay,   "Play",              new Point(12,  y), (s, e) => PlaySelected(false));
            SetupButton(_btnOrig,   "Play original",     new Point(100, y), (s, e) => PlaySelected(true));
            SetupButton(_btnStop,   "Stop",              new Point(210, y), (s, e) => StopPlayback());
            SetupButton(_btnExport, "Export WAV…",       new Point(298, y), (s, e) => ExportSelected());
            SetupButton(_btnLast,   "Last played in game", new Point(408, y), (s, e) => SelectLastPlayed());
            SetupButton(_btnImport, "Replace with file…", new Point(12,  y + 30), (s, e) => ImportSelected());
            SetupButton(_btnRecord, "● Record",          new Point(140, y + 30), (s, e) => ToggleRecord());
            SetupButton(_btnRemove, "Remove replacement", new Point(228, y + 30), (s, e) => RemoveSelected());
            SetupButton(_btnFolder, "Open folder",       new Point(368, y + 30), (s, e) => OpenFolder());
            _btnImport.Width = 120; _btnRemove.Width = 132; _btnLast.Width = 140; _btnOrig.Width = 102;

            ResolveDisc();
            Populate();
        }

        private void SetupButton(Button b, string text, Point at, EventHandler onClick)
        {
            b.Text = text;
            b.Location = at;
            b.Size = new Size(80, 26);
            b.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            b.Click += onClick;
            Controls.Add(b);
        }

        // ---- disc + paths ------------------------------------------------------

        private string GameDataDir { get { return Path.Combine(_gameRoot, "gamedata"); } }
        private string OverrideDir { get { return Path.Combine(GameDataDir, Path.Combine("load", "XA")); } }
        private string OverridePath(int idx)
        {
            if (_textMode) return Path.Combine(OverrideDir, "msg_" + MsgTable.Items[idx].Key.Replace('.', '_') + ".wav");
            return Path.Combine(OverrideDir, "xa_" + idx.ToString("D4") + ".wav");
        }
        private string LineLabel(int idx) { return _textMode ? MsgTable.Items[idx].Key : idx.ToString(); }

        /// <summary>The image the game plays from: the config's disc_image if it names a
        /// file in gamedata, else the first .bin there.</summary>
        private void ResolveDisc()
        {
            _binPath = null;
            _xaSectors = null;
            try
            {
                if (!Directory.Exists(GameDataDir)) return;
                var bins = Directory.GetFiles(GameDataDir, "*.bin");
                if (bins.Length == 0) return;
                string want = null;
                string cfg = Path.Combine(_gameRoot, "config.cfg");
                if (File.Exists(cfg))
                {
                    foreach (var line in File.ReadAllLines(cfg))
                    {
                        var t = line.Trim();
                        if (t.StartsWith("disc_image=", StringComparison.OrdinalIgnoreCase))
                            want = t.Substring("disc_image=".Length).Trim();
                    }
                }
                string pick = bins[0];
                if (!string.IsNullOrEmpty(want))
                {
                    foreach (var b in bins)
                    {
                        string n = Path.GetFileName(b);
                        if (string.Equals(n, want, StringComparison.OrdinalIgnoreCase) ||
                            n.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0 ||
                            want.IndexOf(n, StringComparison.OrdinalIgnoreCase) >= 0)
                        { pick = b; break; }
                    }
                }
                _binPath = pick;
                string err;
                _xaSectors = BinExtractor.ReadXaFileSectors(_binPath, out err);
                if (_xaSectors == null) _info.Text = "Disc " + Path.GetFileName(_binPath) + ": " + err;
            }
            catch (Exception ex)
            {
                _info.Text = "Disc lookup failed: " + ex.Message;
            }
        }

        // ---- list ---------------------------------------------------------------

        private void SetupColumns()
        {
            _list.Columns.Clear();
            if (_textMode)
            {
                _list.Columns.Add("Key", 110, HorizontalAlignment.Left);
                _list.Columns.Add("Text", 420, HorizontalAlignment.Left);
                _list.Columns.Add("Replacement", 190, HorizontalAlignment.Left);
                return;
            }
            _list.Columns.Add("#", 50, HorizontalAlignment.Right);
            _list.Columns.Add("Length", 60, HorizontalAlignment.Right);
            _list.Columns.Add("Subtitle", 380, HorizontalAlignment.Left);
            _list.Columns.Add("Key", 100, HorizontalAlignment.Left);
            _list.Columns.Add("Format", 96, HorizontalAlignment.Left);
            _list.Columns.Add("Replacement", 170, HorizontalAlignment.Left);
        }

        private void PopulateText()
        {
            int replaced = 0;
            for (int i = 0; i < MsgTable.Items.Length; i++)
            {
                var it = MsgTable.Items[i];
                string ov = File.Exists(OverridePath(i)) ? Path.GetFileName(OverridePath(i)) : "";
                if (ov.Length > 0) replaced++;
                var row = new ListViewItem(it.Key);
                row.SubItems.Add(it.Text);
                row.SubItems.Add(ov);
                row.Tag = i;
                if (ov.Length > 0) row.ForeColor = Color.DarkGreen;
                _list.Items.Add(row);
            }
            _info.Text = string.Format("{0} text-box messages (USA script), {1} with a voice file. Files: gamedata\\load\\XA\\msg_<KEY>.wav. Lines the game already voices keep their disc voice.",
                MsgTable.Items.Length, replaced);
        }

        private void Populate()
        {
            _list.BeginUpdate();
            _list.Items.Clear();
            SetupColumns();
            if (_textMode)
            {
                PopulateText();
                _list.EndUpdate();
                UpdateButtons();
                return;
            }
            if (_binPath == null || _xaSectors == null)
            {
                _list.EndUpdate();
                if (_binPath == null) _info.Text = "No disc image (.bin) found in gamedata.";
                UpdateButtons();
                return;
            }
            int shown = 0, replaced = 0;
            try
            {
                using (var f = new FileStream(_binPath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    var sector = new byte[2336];
                    for (int i = 0; i < XaTable.Items.Length; i++)
                    {
                        var it = XaTable.Items[i];
                        if (it.File < 1 || it.File > 9 || _xaSectors[it.File] == 0) continue;
                        string fmt = "?";
                        if (ReadSector(f, _xaSectors[it.File], it.Sector, sector))
                        {
                            bool stereo = (sector[3] & 1) != 0;
                            int rate = ((sector[3] >> 2) & 3) == 0 ? 37800 : 18900;
                            fmt = rate + " Hz " + (stereo ? "stereo" : "mono");
                        }
                        string ov = File.Exists(OverridePath(i)) ? Path.GetFileName(OverridePath(i)) : "";
                        if (ov.Length > 0) replaced++;
                        string key = XaSubtitles.Keys[i] ?? "";
                        var row = new ListViewItem(i.ToString());
                        row.SubItems.Add(FormatSeconds(it.Frames / 60.0));
                        row.SubItems.Add(SubtitleText(key));
                        row.SubItems.Add(key);
                        row.SubItems.Add(fmt);
                        row.SubItems.Add(ov);
                        row.Tag = i;
                        if (ov.Length > 0) row.ForeColor = Color.DarkGreen;
                        _list.Items.Add(row);
                        shown++;
                    }
                }
                _info.Text = string.Format("Disc: {0} — {1} voice lines, {2} replaced. Replacements live in gamedata\\load\\XA (a load mod's load\\XA folder deploys there).",
                    Path.GetFileName(_binPath), shown, replaced);
            }
            catch (Exception ex)
            {
                _info.Text = "Could not read the disc: " + ex.Message;
            }
            _list.EndUpdate();
            UpdateButtons();
        }

        private static string FormatSeconds(double s)
        {
            int m = (int)(s / 60);
            return string.Format("{0}:{1:00.0}", m, s - m * 60);
        }

        private static Dictionary<string, string> s_msgText;

        /// <summary>The script text for a message key (MsgTable), "" when unknown.</summary>
        private static string SubtitleText(string key)
        {
            if (key.Length == 0) return "";
            if (s_msgText == null)
            {
                s_msgText = new Dictionary<string, string>();
                foreach (var it in MsgTable.Items) s_msgText[it.Key] = it.Text;
            }
            string t;
            return s_msgText.TryGetValue(key, out t) ? t : "";
        }

        private int SelectedIdx()
        {
            if (_list.SelectedItems.Count == 0) return -1;
            return (int)_list.SelectedItems[0].Tag;
        }

        private void UpdateButtons()
        {
            int idx = SelectedIdx();
            bool have = idx >= 0 && (_textMode || _xaSectors != null);
            bool ov = have && File.Exists(OverridePath(idx));
            _btnPlay.Enabled = have && !_recording;
            _btnOrig.Enabled = have && ov && !_recording && !_textMode;
            _btnExport.Enabled = have && !_recording && !_textMode;
            _btnImport.Enabled = have && !_recording;
            _btnRecord.Enabled = have;
            _btnRemove.Enabled = ov && !_recording;
            _btnLast.Enabled = (_textMode || _xaSectors != null) && !_recording;
            _mode.Enabled = !_recording;
        }

        private void RefreshRow(int idx)
        {
            foreach (ListViewItem row in _list.Items)
            {
                if ((int)row.Tag != idx) continue;
                bool ov = File.Exists(OverridePath(idx));
                row.SubItems[row.SubItems.Count - 1].Text = ov ? Path.GetFileName(OverridePath(idx)) : "";
                row.ForeColor = ov ? Color.DarkGreen : SystemColors.WindowText;
                break;
            }
            UpdateButtons();
        }

        // ---- XA decode ------------------------------------------------------------

        private static bool ReadSector(FileStream f, int baseSector, int sectorIndex, byte[] dst)
        {
            long off = (long)(baseSector + sectorIndex) * 2352 + 16;
            if (off + 2336 > f.Length) return false;
            f.Seek(off, SeekOrigin.Begin);
            int got = 0;
            while (got < 2336)
            {
                int n = f.Read(dst, got, 2336 - got);
                if (n <= 0) return false;
                got += n;
            }
            return true;
        }

        private static readonly short[] FilterPos = { 0, 60, 115, 98, 122, 0, 0, 0 };
        private static readonly short[] FilterNeg = { 0, 0, -52, -55, -60, 0, 0, 0 };

        private static short ClampS16(int v) { return (short)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

        /// <summary>28 samples of one sub-block of a 128-byte sound group (XA-ADPCM,
        /// 4-bit), the same arithmetic as the game's decoder.</summary>
        private static void DecodeSubblock(byte[] sec, int group, int sb, int[] prev, short[] outSamples)
        {
            int headers = group + 4;
            int words   = group + 16;
            int shift     = sec[headers + sb] & 0xF;
            int filterIdx = (sec[headers + sb] >> 4) & 0x7;
            int fpos = FilterPos[filterIdx], fneg = FilterNeg[filterIdx];
            int byteIdx = sb >> 1;
            int nibShift = (sb & 1) != 0 ? 4 : 0;
            for (int w = 0; w < 28; w++)
            {
                int nibble = (sec[words + w * 4 + byteIdx] >> nibShift) & 0xF;
                int sample = ((short)(nibble << 12)) >> shift;
                sample += (prev[0] * fpos + prev[1] * fneg + 32) >> 6;
                prev[1] = prev[0];
                prev[0] = sample;
                outSamples[w] = ClampS16(sample);
            }
        }

        private static int DecodeSector(byte[] sec, bool stereo, int[] histL, int[] histR, short[] pcm, int at)
        {
            var a = new short[28];
            var b = new short[28];
            int n = 0;
            for (int g = 0; g < 18; g++)
            {
                int group = 8 + g * 128;
                if (stereo)
                {
                    for (int p = 0; p < 4; p++)
                    {
                        DecodeSubblock(sec, group, 2 * p, histL, a);
                        DecodeSubblock(sec, group, 2 * p + 1, histR, b);
                        for (int s = 0; s < 28; s++) { pcm[at + n++] = a[s]; pcm[at + n++] = b[s]; }
                    }
                }
                else
                {
                    for (int sb = 0; sb < 8; sb++)
                    {
                        DecodeSubblock(sec, group, sb, histL, a);
                        for (int s = 0; s < 28; s++) pcm[at + n++] = a[s];
                    }
                }
            }
            return n;
        }

        /// <summary>Decode voice line idx from the disc into a WAV image.</summary>
        private byte[] DecodeLine(int idx, out string error)
        {
            error = null;
            var it = XaTable.Items[idx];
            if (_xaSectors == null || it.File < 1 || it.File > 9 || _xaSectors[it.File] == 0) { error = "Line has no XA file."; return null; }
            using (var f = new FileStream(_binPath, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var sec = new byte[2336];
                int baseSector = _xaSectors[it.File];
                if (!ReadSector(f, baseSector, it.Sector, sec)) { error = "First sector unreadable."; return null; }
                byte file = sec[0], channel = sec[1];
                bool stereo = (sec[3] & 1) != 0;
                int rate = ((sec[3] >> 2) & 3) == 0 ? 37800 : 18900;
                if (((sec[3] >> 4) & 1) != 0) { error = "8-bit XA is not supported."; return null; }
                int samplesPerSector = stereo ? 2016 : 4032;
                int wanted = (int)(((long)(rate / 60) * it.Frames + samplesPerSector - 1) / samplesPerSector);
                var pcm = new short[wanted * 4032 + 4032];
                var histL = new int[2]; var histR = new int[2];
                int total = 0, matched = 0, cur = it.Sector, scanCap = wanted * 32;
                while (matched < wanted && scanCap-- > 0)
                {
                    if (!ReadSector(f, baseSector, cur, sec)) break;
                    cur++;
                    if (sec[0] != file || sec[1] != channel) continue;
                    total += DecodeSector(sec, stereo, histL, histR, pcm, total);
                    matched++;
                }
                if (matched == 0) { error = "No sectors for this line's channel."; return null; }
                return BuildWav(pcm, total, rate, stereo ? 2 : 1);
            }
        }

        private static byte[] BuildWav(short[] pcm, int count, int rate, int channels)
        {
            int dataBytes = count * 2;
            using (var ms = new MemoryStream(44 + dataBytes))
            using (var w = new BinaryWriter(ms))
            {
                w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write(36 + dataBytes);
                w.Write(Encoding.ASCII.GetBytes("WAVE"));
                w.Write(Encoding.ASCII.GetBytes("fmt ")); w.Write(16);
                w.Write((short)1); w.Write((short)channels); w.Write(rate);
                w.Write(rate * channels * 2); w.Write((short)(channels * 2)); w.Write((short)16);
                w.Write(Encoding.ASCII.GetBytes("data")); w.Write(dataBytes);
                for (int i = 0; i < count; i++) w.Write(pcm[i]);
                w.Flush();
                return ms.ToArray();
            }
        }

        // ---- actions ---------------------------------------------------------------

        private void PlaySelected(bool original)
        {
            int idx = SelectedIdx();
            if (idx < 0) return;
            StopPlayback();
            try
            {
                byte[] wav;
                string ov = OverridePath(idx);
                if (_textMode && !File.Exists(ov))
                {
                    _info.Text = "No voice file for " + LineLabel(idx) + " yet: press Record, or Replace with a file.";
                    return;
                }
                if (!original && File.Exists(ov))
                {
                    wav = File.ReadAllBytes(ov);
                    _info.Text = "Playing replacement " + Path.GetFileName(ov) + ".";
                }
                else
                {
                    string err;
                    wav = DecodeLine(idx, out err);
                    if (wav == null) { _info.Text = "Line " + idx + ": " + err; return; }
                    _info.Text = "Playing original line " + idx + ".";
                }
                _player = new SoundPlayer(new MemoryStream(wav));
                _player.Play();
            }
            catch (Exception ex)
            {
                _info.Text = "Playback failed: " + ex.Message;
            }
        }

        private void StopPlayback()
        {
            if (_player != null)
            {
                try { _player.Stop(); } catch { }
                _player.Dispose();
                _player = null;
            }
        }

        private void ExportSelected()
        {
            int idx = SelectedIdx();
            if (idx < 0) return;
            string err;
            byte[] wav = DecodeLine(idx, out err);
            if (wav == null) { _info.Text = "Line " + idx + ": " + err; return; }
            using (var d = new SaveFileDialog())
            {
                d.Title = "Export voice line";
                d.Filter = "WAV audio (*.wav)|*.wav";
                d.FileName = "xa_" + idx.ToString("D4") + ".wav";
                if (d.ShowDialog(this) != DialogResult.OK) return;
                File.WriteAllBytes(d.FileName, wav);
                _info.Text = "Exported line " + idx + " to " + d.FileName + ".";
            }
        }

        /// <summary>Copy any audio file in as the line's replacement. A 16-bit PCM WAV
        /// is taken as is; other WAV depths are converted; other formats go through
        /// ffmpeg.exe when one is beside the game or on PATH.</summary>
        private void ImportSelected()
        {
            int idx = SelectedIdx();
            if (idx < 0) return;
            using (var d = new OpenFileDialog())
            {
                d.Title = "Replace voice line " + LineLabel(idx);
                d.Filter = "WAV audio (*.wav)|*.wav|All audio (*.wav;*.mp3;*.ogg;*.flac;*.m4a)|*.wav;*.mp3;*.ogg;*.flac;*.m4a|All files (*.*)|*.*";
                if (d.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    byte[] wav = LoadAsPcm16Wav(d.FileName);
                    if (wav == null) { _info.Text = "Could not read " + Path.GetFileName(d.FileName) + " as audio (WAV needs PCM; other formats need ffmpeg.exe)."; return; }
                    Directory.CreateDirectory(OverrideDir);
                    File.WriteAllBytes(OverridePath(idx), wav);
                    RefreshRow(idx);
                    _info.Text = "Line " + LineLabel(idx) + " now plays " + Path.GetFileName(d.FileName) + " (saved as " + Path.GetFileName(OverridePath(idx)) + ").";
                }
                catch (Exception ex)
                {
                    _info.Text = "Replace failed: " + ex.Message;
                }
            }
        }

        private static string FindFfmpeg(string gameRoot)
        {
            string local = Path.Combine(gameRoot, "ffmpeg.exe");
            if (File.Exists(local)) return local;
            try
            {
                foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';'))
                {
                    if (dir.Trim().Length == 0) continue;
                    string p = Path.Combine(dir.Trim(), "ffmpeg.exe");
                    if (File.Exists(p)) return p;
                }
            }
            catch { }
            return null;
        }

        private byte[] LoadAsPcm16Wav(string path)
        {
            if (path.EndsWith(".wav", StringComparison.OrdinalIgnoreCase))
            {
                byte[] wav = NormalizeWav(File.ReadAllBytes(path));
                if (wav != null) return ResampleWav(wav, XaRate);
            }
            string ff = FindFfmpeg(_gameRoot);
            if (ff == null) return null;
            string tmp = Path.Combine(Path.GetTempPath(), "sh1_xa_" + Guid.NewGuid().ToString("N") + ".wav");
            try
            {
                var psi = new ProcessStartInfo(ff, "-y -i \"" + path + "\" -vn -acodec pcm_s16le -ar " + XaRate + " \"" + tmp + "\"")
                {
                    UseShellExecute = false, CreateNoWindow = true
                };
                using (var p = Process.Start(psi)) { p.WaitForExit(120000); }
                if (!File.Exists(tmp)) return null;
                byte[] wav = NormalizeWav(File.ReadAllBytes(tmp));
                return wav == null ? null : ResampleWav(wav, XaRate);
            }
            finally
            {
                try { if (File.Exists(tmp)) File.Delete(tmp); } catch { }
            }
        }

        /// <summary>The disc's XA sample rate. Files saved at it need no conversion in
        /// the game (whose software mixer takes only the disc rates and otherwise
        /// resamples at load).</summary>
        private const int XaRate = 37800;

        /// <summary>A canonical 16-bit PCM WAV (BuildWav layout) brought to the given
        /// rate by linear interpolation; returned as is when already there.</summary>
        private static byte[] ResampleWav(byte[] wav, int rate)
        {
            int channels = BitConverter.ToInt16(wav, 22);
            int inRate = BitConverter.ToInt32(wav, 24);
            int dataLen = BitConverter.ToInt32(wav, 40);
            if (inRate == rate || inRate <= 0 || channels < 1) return wav;
            int inFrames = dataLen / (channels * 2);
            long outFrames = (long)inFrames * rate / inRate;
            var pcm = new short[outFrames * channels];
            for (long i = 0; i < outFrames; i++)
            {
                long srcPos = i * inRate;
                int idx = (int)(srcPos / rate);
                int frac = (int)(srcPos % rate);
                for (int c = 0; c < channels; c++)
                {
                    int a = BitConverter.ToInt16(wav, 44 + (idx * channels + c) * 2);
                    int b = idx + 1 < inFrames ? BitConverter.ToInt16(wav, 44 + ((idx + 1) * channels + c) * 2) : a;
                    pcm[i * channels + c] = (short)(a + (int)((long)(b - a) * frac / rate));
                }
            }
            return BuildWav(pcm, pcm.Length, rate, channels);
        }

        /// <summary>A WAV re-emitted as canonical 16-bit PCM (8/24/32-bit integer and
        /// 32-bit float converted), or null if it is not PCM.</summary>
        private static byte[] NormalizeWav(byte[] d)
        {
            if (d.Length < 44 || Encoding.ASCII.GetString(d, 0, 4) != "RIFF" || Encoding.ASCII.GetString(d, 8, 4) != "WAVE") return null;
            int fmtTag = 0, channels = 0, rate = 0, bits = 0, dataOff = -1, dataLen = 0;
            for (int off = 12; off + 8 <= d.Length; )
            {
                string id = Encoding.ASCII.GetString(d, off, 4);
                int len = BitConverter.ToInt32(d, off + 4);
                if (len < 0 || off + 8 + len > d.Length) len = d.Length - off - 8;
                if (id == "fmt " && len >= 16)
                {
                    fmtTag = BitConverter.ToUInt16(d, off + 8);
                    channels = BitConverter.ToUInt16(d, off + 10);
                    rate = BitConverter.ToInt32(d, off + 12);
                    bits = BitConverter.ToUInt16(d, off + 22);
                    if (fmtTag == 0xFFFE && len >= 26) fmtTag = BitConverter.ToUInt16(d, off + 8 + 24);
                }
                else if (id == "data") { dataOff = off + 8; dataLen = len; }
                off += 8 + len + (len & 1);
            }
            if (dataOff < 0 || (channels != 1 && channels != 2) || rate <= 0) return null;
            bool isFloat = fmtTag == 3;
            if (!(fmtTag == 1 || isFloat)) return null;
            int frameIn = channels * (bits / 8);
            if (frameIn == 0) return null;
            int frames = dataLen / frameIn;
            var pcm = new short[frames * channels];
            int p = dataOff;
            for (int i = 0; i < frames * channels; i++, p += bits / 8)
            {
                int v;
                if (isFloat && bits == 32) v = (int)Math.Round(BitConverter.ToSingle(d, p) * 32767f);
                else if (bits == 16) v = BitConverter.ToInt16(d, p);
                else if (bits == 8) v = (d[p] - 128) << 8;
                else if (bits == 24) v = (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)) << 8 >> 16;
                else if (bits == 32) v = BitConverter.ToInt32(d, p) >> 16;
                else return null;
                pcm[i] = ClampS16(v);
            }
            return BuildWav(pcm, pcm.Length, rate, channels);
        }

        private void RemoveSelected()
        {
            int idx = SelectedIdx();
            if (idx < 0) return;
            string ov = OverridePath(idx);
            if (!File.Exists(ov)) return;
            if (MessageBox.Show(this, "Delete " + Path.GetFileName(ov) + "? " +
                    (_textMode ? "The text box goes back to being silent." : "The line goes back to the disc's voice."),
                    "Voices", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            StopPlayback();
            File.Delete(ov);
            RefreshRow(idx);
            _info.Text = _textMode ? LineLabel(idx) + " is silent again." : "Line " + idx + " plays the original again.";
        }

        // ---- recording (winmm MCI: no dependencies, saves a PCM WAV) ---------------

        private static string Mci(string cmd)
        {
            var ret = new StringBuilder(256);
            int err = mciSendString(cmd, ret, ret.Capacity, IntPtr.Zero);
            if (err != 0) throw new InvalidOperationException("MCI error " + err + " for: " + cmd);
            return ret.ToString();
        }

        private void ToggleRecord()
        {
            if (!_recording)
            {
                int idx = SelectedIdx();
                if (idx < 0) return;
                StopPlayback();
                try
                {
                    Mci("open new type waveaudio alias sh1xarec");
                    try
                    {
                        // The disc's own rate, so the game plays the take as is.
                        Mci("set sh1xarec time format ms bitspersample 16 channels 1 samplespersec " + XaRate + " bytespersec " + (XaRate * 2) + " alignment 2");
                    }
                    catch (InvalidOperationException)
                    {
                        Mci("set sh1xarec time format ms bitspersample 16 channels 1 samplespersec 44100 bytespersec 88200 alignment 2");
                    }
                    Mci("record sh1xarec");
                }
                catch (Exception ex)
                {
                    try { Mci("close sh1xarec"); } catch { }
                    _info.Text = "Could not start recording (is a microphone connected?): " + ex.Message;
                    return;
                }
                _recording = true;
                _recIdx = idx;
                _btnRecord.Text = "■ Stop and save";
                _info.Text = "Recording line " + LineLabel(idx) + "… speak, then press Stop and save.";
                UpdateButtons();
                return;
            }

            try
            {
                Mci("stop sh1xarec");
                Directory.CreateDirectory(OverrideDir);
                string target = OverridePath(_recIdx);
                Mci("save sh1xarec \"" + target + "\"");
                Mci("close sh1xarec");
                _info.Text = "Saved the take as " + Path.GetFileName(target) + ". The game plays it for line " + LineLabel(_recIdx) + " now.";
                RefreshRow(_recIdx);
                _list.Focus();
                PlaySelectedIdx(_recIdx);
            }
            catch (Exception ex)
            {
                try { Mci("close sh1xarec"); } catch { }
                _info.Text = "Saving the recording failed: " + ex.Message;
            }
            _recording = false;
            _recIdx = -1;
            _btnRecord.Text = "● Record";
            UpdateButtons();
        }

        private void PlaySelectedIdx(int idx)
        {
            foreach (ListViewItem row in _list.Items)
            {
                if ((int)row.Tag == idx) { row.Selected = true; row.EnsureVisible(); break; }
            }
            PlaySelected(false);
        }

        // ---- helpers ------------------------------------------------------------------

        /// <summary>Select the line the game last played, from the newest log: the XA
        /// player logs "[XA] Play xaIdx=N" for every line and pc_msg_voice.c logs
        /// "[MSGBOX] KEY" for every unvoiced text box, so a modder can play a scene,
        /// come here, and find the line without knowing its number.</summary>
        private void SelectLastPlayed()
        {
            try
            {
                var logs = new List<string>(Directory.GetFiles(_gameRoot, "SilentHill*.log"));
                if (logs.Count == 0) { _info.Text = "No SilentHill log found beside the game."; return; }
                logs.Sort((a, b) => File.GetLastWriteTimeUtc(b).CompareTo(File.GetLastWriteTimeUtc(a)));
                string text;
                using (var f = new FileStream(logs[0], FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    long take = Math.Min(f.Length, 4L * 1024 * 1024);
                    f.Seek(f.Length - take, SeekOrigin.Begin);
                    var buf = new byte[take];
                    int got = 0; while (got < take) { int n = f.Read(buf, got, (int)(take - got)); if (n <= 0) break; got += n; }
                    text = Encoding.ASCII.GetString(buf, 0, got);
                }
                int idx = -1;
                string label;
                if (_textMode)
                {
                    var ms = Regex.Matches(text, @"\[MSGBOX\] (\S+)");
                    if (ms.Count == 0) { _info.Text = "The newest log has no text boxes (the game logs each unvoiced box it opens)."; return; }
                    label = ms[ms.Count - 1].Groups[1].Value;
                    for (int i = 0; i < MsgTable.Items.Length; i++)
                        if (MsgTable.Items[i].Key == label) { idx = i; break; }
                }
                else
                {
                    var ms = Regex.Matches(text, @"\[XA\] (?:Play|override) (?:xaIdx=|xa_)(\d+)");
                    if (ms.Count == 0) { _info.Text = "The newest log has no voice line plays."; return; }
                    idx = int.Parse(ms[ms.Count - 1].Groups[1].Value);
                    label = idx.ToString();
                }
                foreach (ListViewItem row in _list.Items)
                {
                    if ((int)row.Tag == idx) { row.Selected = true; row.EnsureVisible(); _info.Text = "Line " + label + " was the last one the game showed (" + Path.GetFileName(logs[0]) + ")."; return; }
                }
                _info.Text = "Line " + label + " was last shown but is not listed.";
            }
            catch (Exception ex)
            {
                _info.Text = "Log read failed: " + ex.Message;
            }
        }

        private void OpenFolder()
        {
            try
            {
                Directory.CreateDirectory(OverrideDir);
                Process.Start("explorer.exe", "\"" + OverrideDir + "\"");
            }
            catch { }
        }

        private void ShowHelp()
        {
            MessageBox.Show(this,
                "Every voice line the game streams from the disc is listed by its number. Play one to hear it, " +
                "Export it as WAV to edit elsewhere, then Replace it with any audio file, or press Record, say the " +
                "line, and Stop and save.\n\n" +
                "A replacement is a WAV in gamedata\\load\\XA named xa_NNNN.wav. Mono or stereo. Record and Replace " +
                "save it at 37800 Hz, the disc's own rate; a file at any other rate is converted by the game when it " +
                "plays. The scene keeps its authored timing: a shorter take does not rush it, and a longer take is " +
                "not cut off.\n\n" +
                "Loose file support must be on (the Mod Manager turns it on when a load mod is applied). To ship a " +
                "fan dub as a mod, put the files in a load\\XA folder inside the mod; Apply deploys them here.\n\n" +
                "Not sure which number a line is? Play the scene in the game, then press \"Last played in game\".\n\n" +
                "Text boxes: switch the list (top right) to \"Text boxes (unvoiced)\" to give a voice to any map " +
                "message the game shows silently: memos, examined objects, doors, item prompts. Record or Replace " +
                "works the same; the file is gamedata\\load\\XA\\msg_<KEY>.wav and plays when that box opens, " +
                "staying on through the box's pages unless a later page has its own file. Read the box in the game, " +
                "then press \"Last played in game\" to find its key. Keys follow the USA script; boxes the game " +
                "already voices ignore these files.",
                "Voice mods", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            StopPlayback();
            if (_recording)
            {
                try { Mci("stop sh1xarec"); Mci("close sh1xarec"); } catch { }
                _recording = false;
            }
            base.OnFormClosing(e);
        }
    }
}
