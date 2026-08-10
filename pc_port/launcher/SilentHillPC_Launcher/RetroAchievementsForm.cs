using System;
using System.Collections.Generic;
using System.Drawing;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>
    /// RetroAchievements sign-in.
    ///
    /// Exchanges the account password for a connect token via RA's login2
    /// endpoint and stores only the token in config.cfg — the password is never
    /// written to disk, never logged, and never seen by the game.
    ///
    /// Softcore only. The port ships quick save/load, debug controls, free
    /// cameras and gamemodes, so hardcore is not offered.
    /// </summary>
    public class RetroAchievementsForm : Form
    {
        private readonly ConfigManager _config;

        private CheckBox _chkEnable;
        private TextBox _txtUser;
        private TextBox _txtPass;
        private Button _btnSignIn;
        private Button _btnSignOut;
        private Label _lblStatus;
        private ComboBox _cboSfx;
        private Button _btnClose;

        private static readonly HttpClient _http = CreateHttpClient();

        private static HttpClient CreateHttpClient()
        {
            // .NET Framework 4.7.2 does not negotiate TLS 1.2 by default on every
            // host configuration; retroachievements.org requires it.
            try { ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12; }
            catch { }
            var c = new HttpClient();
            c.DefaultRequestHeaders.Add("User-Agent", "SilentHillPC-Launcher");
            c.Timeout = TimeSpan.FromSeconds(30);
            return c;
        }

        public RetroAchievementsForm(ConfigManager config)
        {
            _config = config;
            BuildUi();
            LoadFromConfig();
        }

        private void BuildUi()
        {
            Text = "RetroAchievements";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(430, 229);

            var lblIntro = new Label
            {
                Location = new Point(12, 12),
                Size = new Size(406, 46),
                Text = "Earn achievements on your real RetroAchievements account while " +
                       "playing this port. Your disc image identifies the game, so the " +
                       "official Silent Hill set is used.\r\n" +
                       "Softcore only — this port has quick save/load and debug features."
            };

            _chkEnable = new CheckBox
            {
                Location = new Point(15, 66),
                Size = new Size(220, 20),
                Text = "Enable RetroAchievements"
            };

            var lblUser = new Label { Location = new Point(15, 98), Size = new Size(70, 20), Text = "Username" };
            _txtUser = new TextBox { Location = new Point(90, 95), Size = new Size(200, 22) };

            var lblPass = new Label { Location = new Point(15, 128), Size = new Size(70, 20), Text = "Password" };
            _txtPass = new TextBox
            {
                Location = new Point(90, 125),
                Size = new Size(200, 22),
                UseSystemPasswordChar = true
            };

            _btnSignIn = new Button
            {
                Location = new Point(300, 94),
                Size = new Size(110, 25),
                Text = "Sign in"
            };
            _btnSignIn.Click += async (s, e) => await SignInAsync();

            _btnSignOut = new Button
            {
                Location = new Point(300, 124),
                Size = new Size(110, 25),
                Text = "Sign out"
            };
            _btnSignOut.Click += (s, e) => SignOut();

            _lblStatus = new Label
            {
                Location = new Point(15, 158),
                Size = new Size(400, 30),
                Text = ""
            };

            // Replaces the old "only a connect token is saved" note: the same
            // point is already made by the intro text, and this row is the only
            // free space on the page.
            var lblSfx = new Label
            {
                Location = new Point(15, 197),
                Size = new Size(105, 20),
                Text = "Achievement SFX:"
            };

            _cboSfx = new ComboBox
            {
                Location = new Point(120, 194),
                Size = new Size(120, 21),
                DropDownStyle = ComboBoxStyle.DropDownList
            };
            _cboSfx.Items.AddRange(new object[] { "Xbox", "PlayStation", "Steam" });

            _btnClose = new Button
            {
                Location = new Point(330, 192),
                Size = new Size(85, 25),
                Text = "Close",
                DialogResult = DialogResult.OK
            };
            _btnClose.Click += (s, e) => SaveEnableFlag();

            Controls.AddRange(new Control[]
            {
                lblIntro, _chkEnable, lblUser, _txtUser, lblPass, _txtPass,
                _btnSignIn, _btnSignOut, _lblStatus, lblSfx, _cboSfx, _btnClose
            });

            AcceptButton = _btnSignIn;
            CancelButton = _btnClose;
        }

        private void LoadFromConfig()
        {
            _chkEnable.Checked = _config.Get("retroachievements", "0") == "1";
            _txtUser.Text = _config.Get("ra_username", "");

            switch (_config.Get("ra_sfx", "playstation"))
            {
                case "xbox":  _cboSfx.SelectedIndex = 0; break;
                case "steam": _cboSfx.SelectedIndex = 2; break;
                default:      _cboSfx.SelectedIndex = 1; break;
            }

            bool signedIn = !string.IsNullOrEmpty(_config.Get("ra_token", ""));
            SetSignedInUi(signedIn);
        }

        private void SetSignedInUi(bool signedIn)
        {
            _btnSignOut.Enabled = signedIn;
            if (signedIn)
            {
                _lblStatus.ForeColor = Color.DarkGreen;
                _lblStatus.Text = "Signed in as " + _config.Get("ra_username", "") +
                                  ". Achievements will unlock in softcore mode.";
                _txtPass.Text = "";
            }
            else
            {
                _lblStatus.ForeColor = SystemColors.ControlText;
                _lblStatus.Text = "Not signed in.";
            }
        }

        private void SaveEnableFlag()
        {
            _config.Set("retroachievements", _chkEnable.Checked ? "1" : "0");
            _config.Set("ra_sfx", SelectedSfxKey());
            _config.Save();
        }

        // The game reads these three names; keep them in step with the
        // ra_sfx handling in pc_config.c / pc_ra_toast.c.
        private string SelectedSfxKey()
        {
            switch (_cboSfx.SelectedIndex)
            {
                case 0:  return "xbox";
                case 2:  return "steam";
                default: return "playstation";
            }
        }

        private void SignOut()
        {
            _config.Set("ra_token", "");
            _config.Set("ra_username", "");
            _config.Save();
            _txtUser.Text = "";
            _txtPass.Text = "";
            SetSignedInUi(false);
        }

        private async Task SignInAsync()
        {
            string user = _txtUser.Text.Trim();
            string pass = _txtPass.Text;

            if (user.Length == 0 || pass.Length == 0)
            {
                _lblStatus.ForeColor = Color.Firebrick;
                _lblStatus.Text = "Enter your RetroAchievements username and password.";
                return;
            }

            _btnSignIn.Enabled = false;
            _lblStatus.ForeColor = SystemColors.ControlText;
            _lblStatus.Text = "Signing in...";

            try
            {
                // login2 trades the password for a long-lived connect token; the
                // password itself is discarded as soon as this call returns.
                var form = new FormUrlEncodedContent(new[]
                {
                    new KeyValuePair<string, string>("r", "login2"),
                    new KeyValuePair<string, string>("u", user),
                    new KeyValuePair<string, string>("p", pass)
                });

                var resp = await _http.PostAsync("https://retroachievements.org/dorequest.php", form);
                string body = await resp.Content.ReadAsStringAsync();

                string token = ExtractJsonString(body, "Token");
                if (!resp.IsSuccessStatusCode || string.IsNullOrEmpty(token))
                {
                    string err = ExtractJsonString(body, "Error");
                    _lblStatus.ForeColor = Color.Firebrick;
                    _lblStatus.Text = string.IsNullOrEmpty(err)
                        ? "Sign-in failed. Check your username and password."
                        : "Sign-in failed: " + err;
                    return;
                }

                string canonical = ExtractJsonString(body, "User");
                if (string.IsNullOrEmpty(canonical))
                    canonical = user;

                _config.Set("ra_username", canonical);
                _config.Set("ra_token", token);
                _chkEnable.Checked = true;
                _config.Set("retroachievements", "1");
                _config.Save();

                _txtUser.Text = canonical;
                SetSignedInUi(true);
            }
            catch (Exception ex)
            {
                _lblStatus.ForeColor = Color.Firebrick;
                _lblStatus.Text = "Could not reach retroachievements.org: " + ex.Message;
            }
            finally
            {
                _btnSignIn.Enabled = true;
                _txtPass.Text = "";
            }
        }

        /// <summary>
        /// Minimal string-field reader for RA's flat JSON responses — avoids
        /// taking a JSON dependency for two fields.
        /// </summary>
        private static string ExtractJsonString(string json, string field)
        {
            if (string.IsNullOrEmpty(json))
                return null;

            string key = "\"" + field + "\"";
            int i = json.IndexOf(key, StringComparison.OrdinalIgnoreCase);
            if (i < 0)
                return null;

            i = json.IndexOf(':', i + key.Length);
            if (i < 0)
                return null;

            i++;
            while (i < json.Length && char.IsWhiteSpace(json[i]))
                i++;
            if (i >= json.Length || json[i] != '"')
                return null;

            i++;
            var sb = new StringBuilder();
            while (i < json.Length && json[i] != '"')
            {
                if (json[i] == '\\' && i + 1 < json.Length)
                    i++;
                sb.Append(json[i++]);
            }
            return sb.ToString();
        }
    }
}
