using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// Silent Hill Online settings, and a way to run the master server.
    ///
    /// Writes the online_* keys in config.cfg. Everything is inert in the game
    /// while "Play online" is unchecked: no socket is opened and no thread
    /// starts, so an unchecked box really is the offline port.
    ///
    /// Test connection speaks the real protocol (a HELLO datagram, waiting for
    /// a WELCOME), rather than probing the port: a UDP port that answers
    /// nothing is indistinguishable from a closed one, and "the port is open"
    /// would not have told the user whether the server on it is a version they
    /// can join. The reply carries the server name and message of the day, so a
    /// successful test shows what they are about to connect to.
    /// </summary>
    public class OnlineForm : Form
    {
        private readonly ConfigManager _config;

        private CheckBox _chkEnable;
        private TextBox _txtServer;
        private NumericUpDown _numPort;
        private TextBox _txtName;
        private TextBox _txtPassword;
        private CheckBox _chkGhosts;
        private CheckBox _chkMemos;
        private CheckBox _chkDeaths;
        private CheckBox _chkEvents;
        private ComboBox _cboStyle;
        private TrackBar _trkRange;
        private Label _lblRange;
        private Label _lblStatus;
        private Button _btnTest;
        private Button _btnHost;
        private Button _btnClose;

        private Process _server;

        // Must match sh_net_proto.h. Only the handshake is duplicated here, and
        // only so the test button can tell a live server from an open port.
        private const int ProtoVer = 1;
        private const int HdrSize = 12;
        private const int MsgHello = 0x01;
        private const int MsgWelcome = 0x02;
        private const int MsgReject = 0x06;
        private const int NameMax = 24;
        private const int BuildMax = 24;
        private const int ServerMax = 32;
        private const int MotdMax = 128;
        private const int RejectMax = 96;

        public OnlineForm(ConfigManager config)
        {
            _config = config;
            BuildUi();
            LoadFromConfig();
        }

        private static Label Lbl(string text, int x, int y, int w = 120)
        {
            return new Label { Location = new Point(x, y + 3), Size = new Size(w, 15), Text = text };
        }

        private void BuildUi()
        {
            Text = "Silent Hill Online";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(560, 432);

            var intro = new Label
            {
                Location = new Point(12, 10),
                Size = new Size(536, 44),
                Text = "Other people, in your Silent Hill. You still play alone — nothing " +
                       "anyone else does can touch your game. What you get is their " +
                       "outlines moving through the fog, the messages they leave, and " +
                       "the places they died."
            };

            _chkEnable = new CheckBox
            {
                Location = new Point(15, 60),
                Size = new Size(240, 20),
                Text = "Play online"
            };

            var grpServer = new GroupBox
            {
                Location = new Point(12, 86),
                Size = new Size(536, 116),
                Text = "Master server"
            };

            _txtServer = new TextBox { Location = new Point(96, 24), Size = new Size(250, 22) };
            _numPort = new NumericUpDown
            {
                Location = new Point(410, 24),
                Size = new Size(72, 22),
                Minimum = 1,
                Maximum = 65535,
                Value = 27888
            };
            _txtName = new TextBox { Location = new Point(96, 52), Size = new Size(180, 22), MaxLength = 23 };
            _txtPassword = new TextBox
            {
                Location = new Point(410, 52),
                Size = new Size(110, 22),
                UseSystemPasswordChar = true
            };

            _btnTest = new Button { Location = new Point(96, 82), Size = new Size(110, 24), Text = "Test connection" };
            _btnTest.Click += BtnTest_Click;

            _btnHost = new Button { Location = new Point(212, 82), Size = new Size(130, 24), Text = "Host a server here" };
            _btnHost.Click += BtnHost_Click;

            _lblStatus = new Label
            {
                Location = new Point(350, 86),
                Size = new Size(178, 20),
                Text = ""
            };

            grpServer.Controls.AddRange(new Control[]
            {
                Lbl("Address", 12, 24, 80), _txtServer,
                Lbl("Port", 372, 24, 34), _numPort,
                Lbl("Your name", 12, 52, 80), _txtName,
                Lbl("Password", 348, 52, 58), _txtPassword,
                _btnTest, _btnHost, _lblStatus
            });

            var grpWhat = new GroupBox
            {
                Location = new Point(12, 210),
                Size = new Size(536, 150),
                Text = "What you see"
            };

            _chkGhosts = new CheckBox { Location = new Point(14, 24), Size = new Size(240, 20), Text = "Other players, as outlines" };
            _chkMemos = new CheckBox { Location = new Point(14, 48), Size = new Size(240, 20), Text = "Messages people leave" };
            _chkDeaths = new CheckBox { Location = new Point(14, 72), Size = new Size(240, 20), Text = "Markers where people died" };
            _chkEvents = new CheckBox { Location = new Point(14, 96), Size = new Size(240, 20), Text = "Join / leave / death feed" };

            _cboStyle = new ComboBox
            {
                Location = new Point(348, 22),
                Size = new Size(174, 22),
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            _cboStyle.Items.AddRange(new object[] { "Outline only", "Floor ring only", "Outline and ring" });

            _trkRange = new TrackBar
            {
                Location = new Point(340, 62),
                Size = new Size(190, 40),
                Minimum = 5,
                Maximum = 120,
                TickFrequency = 15,
                Value = 40
            };
            _trkRange.ValueChanged += (s, e) => UpdateRangeLabel();
            _lblRange = new Label { Location = new Point(348, 104), Size = new Size(180, 18), Text = "" };

            grpWhat.Controls.AddRange(new Control[]
            {
                _chkGhosts, _chkMemos, _chkDeaths, _chkEvents,
                Lbl("How ghosts draw", 258, 22, 90), _cboStyle,
                Lbl("Visible within", 258, 66, 80), _trkRange, _lblRange
            });

            var keys = new Label
            {
                Location = new Point(14, 368),
                Size = new Size(360, 18),
                Text = "In game:  F11 who is online     M leave a message"
            };

            _btnClose = new Button
            {
                Location = new Point(452, 396),
                Size = new Size(96, 26),
                Text = "Save",
                DialogResult = DialogResult.OK
            };
            _btnClose.Click += (s, e) => SaveToConfig();

            var tip = new ToolTip { AutoPopDelay = 30000, InitialDelay = 400, ReshowDelay = 100 };
            tip.SetToolTip(_txtServer,
                "Hostname or IP of the machine running sh_master. 127.0.0.1 is this machine.");
            tip.SetToolTip(_btnHost,
                "Starts online_server\\sh_master.exe in a console window on this machine. " +
                "Others need UDP " + _numPort.Value + " forwarded to reach it from outside your network.");
            tip.SetToolTip(_btnTest,
                "Sends a real join request and waits for the reply, so a pass means the " +
                "server is running AND speaks a version you can join.");
            tip.SetToolTip(_trkRange,
                "Metres past which another player is not drawn. Keeping it modest is a " +
                "design choice, not a performance one: a ghost visible across the whole " +
                "map gives away rooms you have not reached yet.");
            tip.SetToolTip(_chkDeaths,
                "Your own deaths are reported too, so other players see where you fell.");

            Controls.AddRange(new Control[] { intro, _chkEnable, grpServer, grpWhat, keys, _btnClose });
            AcceptButton = _btnClose;
        }

        private void UpdateRangeLabel()
        {
            _lblRange.Text = _trkRange.Value + " metres";
        }

        private static int GetInt(ConfigManager c, string key, int def)
        {
            int v;
            return int.TryParse(c.Get(key, def.ToString()), out v) ? v : def;
        }

        private void LoadFromConfig()
        {
            _chkEnable.Checked = GetInt(_config, "online_enabled", 0) != 0;
            _txtServer.Text = _config.Get("online_server", "127.0.0.1");
            _numPort.Value = Math.Min(65535, Math.Max(1, GetInt(_config, "online_port", 27888)));
            _txtName.Text = _config.Get("online_name", "Wanderer");
            _txtPassword.Text = _config.Get("online_password", "");
            _chkGhosts.Checked = GetInt(_config, "online_ghosts", 1) != 0;
            _chkMemos.Checked = GetInt(_config, "online_memos", 1) != 0;
            _chkDeaths.Checked = GetInt(_config, "online_deaths", 1) != 0;
            _chkEvents.Checked = GetInt(_config, "online_events", 1) != 0;
            _cboStyle.SelectedIndex = Math.Min(2, Math.Max(0, GetInt(_config, "online_ghost_style", 2)));
            _trkRange.Value = Math.Min(_trkRange.Maximum,
                                       Math.Max(_trkRange.Minimum, GetInt(_config, "online_ghost_range", 40)));
            UpdateRangeLabel();
        }

        private void SaveToConfig()
        {
            // Sanitised the same way the client sanitises it, so what the
            // launcher shows and what other players see are the same string.
            var name = new StringBuilder();
            foreach (char ch in _txtName.Text)
            {
                if (name.Length >= NameMax - 1) break;
                name.Append(ch >= 32 && ch <= 126 ? ch : '?');
            }
            if (name.ToString().Trim().Length == 0) name = new StringBuilder("Wanderer");

            _config.Set("online_enabled", _chkEnable.Checked ? "1" : "0");
            _config.Set("online_server", _txtServer.Text.Trim());
            _config.Set("online_port", ((int)_numPort.Value).ToString());
            _config.Set("online_name", name.ToString());
            _config.Set("online_password", _txtPassword.Text);
            _config.Set("online_ghosts", _chkGhosts.Checked ? "1" : "0");
            _config.Set("online_memos", _chkMemos.Checked ? "1" : "0");
            _config.Set("online_deaths", _chkDeaths.Checked ? "1" : "0");
            _config.Set("online_events", _chkEvents.Checked ? "1" : "0");
            _config.Set("online_ghost_style", _cboStyle.SelectedIndex.ToString());
            _config.Set("online_ghost_range", _trkRange.Value.ToString());
            _config.Save();
        }

        // ---------------------------------------------------------------
        // Handshake
        // ---------------------------------------------------------------

        private static void PutU8(byte[] b, ref int o, int v) { b[o++] = (byte)v; }

        private static void PutU16(byte[] b, ref int o, int v)
        {
            b[o++] = (byte)(v & 0xFF);
            b[o++] = (byte)((v >> 8) & 0xFF);
        }

        private static void PutU32(byte[] b, ref int o, uint v)
        {
            b[o++] = (byte)(v & 0xFF);
            b[o++] = (byte)((v >> 8) & 0xFF);
            b[o++] = (byte)((v >> 16) & 0xFF);
            b[o++] = (byte)((v >> 24) & 0xFF);
        }

        private static void PutStr(byte[] b, ref int o, string s, int cap)
        {
            for (int i = 0; i < cap; i++)
                b[o + i] = (byte)(i < s.Length && s[i] >= 32 && s[i] <= 126 ? s[i] : 0);
            b[o + cap - 1] = 0;
            o += cap;
        }

        private static string GetStr(byte[] b, ref int o, int cap)
        {
            int n = 0;
            while (n < cap && o + n < b.Length && b[o + n] != 0) n++;
            string s = Encoding.ASCII.GetString(b, o, n);
            o += cap;
            return s;
        }

        /// <summary>FNV-1a, matching ShnHashPassword in sh_net_proto.h.</summary>
        private static uint HashPassword(string s)
        {
            if (string.IsNullOrEmpty(s)) return 0;
            uint h = 2166136261u;
            foreach (char c in s)
            {
                h ^= (byte)c;
                h *= 16777619u;
            }
            return h == 0 ? 1u : h;
        }

        private void BtnTest_Click(object sender, EventArgs e)
        {
            _lblStatus.ForeColor = SystemColors.ControlText;
            _lblStatus.Text = "Contacting...";
            _btnTest.Enabled = false;
            Application.DoEvents();
            try
            {
                string message;
                bool ok = TryHandshake(_txtServer.Text.Trim(), (int)_numPort.Value,
                                       _txtName.Text, _txtPassword.Text, out message);
                _lblStatus.ForeColor = ok ? Color.FromArgb(0, 110, 0) : Color.FromArgb(150, 0, 0);
                _lblStatus.Text = message;
                if (ok)
                {
                    MessageBox.Show(this, message, "Silent Hill Online",
                                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                }
            }
            finally
            {
                _btnTest.Enabled = true;
            }
        }

        private bool TryHandshake(string host, int port, string name, string password, out string message)
        {
            message = "";
            if (string.IsNullOrEmpty(host))
            {
                message = "Enter a server address first.";
                return false;
            }

            try
            {
                using (var sock = new UdpClient())
                {
                    sock.Client.ReceiveTimeout = 2500;

                    var buf = new byte[HdrSize + 128];
                    int o = HdrSize;
                    PutU16(buf, ref o, ProtoVer);
                    PutU8(buf, ref o, 0);
                    PutU8(buf, ref o, 0);
                    PutU32(buf, ref o, 0);
                    PutStr(buf, ref o, string.IsNullOrEmpty(name) ? "Wanderer" : name, NameMax);
                    PutStr(buf, ref o, "launcher", BuildMax);
                    PutU32(buf, ref o, HashPassword(password));

                    int h = 0;
                    PutU8(buf, ref h, 'S'); PutU8(buf, ref h, 'H');
                    PutU8(buf, ref h, 'O'); PutU8(buf, ref h, '1');
                    PutU8(buf, ref h, ProtoVer);
                    PutU8(buf, ref h, MsgHello);
                    PutU16(buf, ref h, o - HdrSize);
                    PutU32(buf, ref h, 0);

                    sock.Send(buf, o, host, port);

                    // Two tries: the first datagram of a session commonly meets a
                    // NAT table that has not opened yet.
                    for (int attempt = 0; attempt < 2; attempt++)
                    {
                        try
                        {
                            IPEndPoint from = null;
                            byte[] reply = sock.Receive(ref from);
                            if (reply.Length < HdrSize) continue;
                            if (reply[0] != 'S' || reply[1] != 'H' || reply[2] != 'O' || reply[3] != '1')
                                continue;
                            if (reply[4] != ProtoVer)
                            {
                                message = "That server speaks protocol v" + reply[4] +
                                          "; this build speaks v" + ProtoVer + ".";
                                return false;
                            }

                            int type = reply[5];
                            int p = HdrSize;
                            if (type == MsgWelcome)
                            {
                                p += 4 + 4 + 2 + 2 + 2 + 2;
                                string srv = GetStr(reply, ref p, ServerMax);
                                string motd = GetStr(reply, ref p, MotdMax);
                                message = "Connected to \"" + srv + "\"" +
                                          (motd.Length > 0 ? " — " + motd : "");
                                return true;
                            }
                            if (type == MsgReject)
                            {
                                p += 4;
                                string why = GetStr(reply, ref p, RejectMax);
                                message = "Refused: " + (why.Length > 0 ? why : "no reason given");
                                return false;
                            }
                        }
                        catch (SocketException)
                        {
                            if (attempt == 0) sock.Send(buf, o, host, port);
                        }
                    }
                    message = "No reply from " + host + ":" + port +
                              ". Is sh_master running, and is UDP " + port + " reachable?";
                    return false;
                }
            }
            catch (Exception ex)
            {
                message = ex.Message;
                return false;
            }
        }

        // ---------------------------------------------------------------
        // Hosting
        // ---------------------------------------------------------------

        private void BtnHost_Click(object sender, EventArgs e)
        {
            if (_server != null && !_server.HasExited)
            {
                MessageBox.Show(this, "A server started from here is already running.",
                                "Silent Hill Online", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            string baseDir = AppDomain.CurrentDomain.BaseDirectory;
            string exe = Path.Combine(baseDir, "online_server\\sh_master.exe");
            if (!File.Exists(exe))
                exe = Path.Combine(baseDir, "sh_master.exe");
            if (!File.Exists(exe))
            {
                MessageBox.Show(this,
                    "sh_master.exe was not found next to the launcher or in online_server\\.\r\n\r\n" +
                    "Build it with:\r\n" +
                    "    cmake -S online_server -B online_server/build\r\n" +
                    "    cmake --build online_server/build",
                    "Silent Hill Online", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            try
            {
                var args = "-p " + (int)_numPort.Value;
                if (_txtPassword.Text.Length > 0)
                    args += " -w \"" + _txtPassword.Text.Replace("\"", "") + "\"";
                _server = Process.Start(new ProcessStartInfo
                {
                    FileName = exe,
                    Arguments = args,
                    WorkingDirectory = Path.GetDirectoryName(exe),
                    UseShellExecute = true
                });
                // Hosting locally means connecting locally; save the user the
                // step they would otherwise forget.
                if (_txtServer.Text.Trim().Length == 0)
                    _txtServer.Text = "127.0.0.1";
                _lblStatus.ForeColor = SystemColors.ControlText;
                _lblStatus.Text = "Server started on UDP " + (int)_numPort.Value + ".";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "Silent Hill Online",
                                MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }
    }
}
