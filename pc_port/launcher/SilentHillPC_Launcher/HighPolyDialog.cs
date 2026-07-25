using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

namespace SilentHillPC_Launcher
{
    /// <summary>The OBJ -> Model "high-poly replacement" dialog: browse the rigged model + the
    /// original character it replaces, tick which automatic fixes to apply, Help explains each.
    /// Replaces the old MessageBox cascade + the wall-of-text tooltip.</summary>
    public class HighPolyDialog : Form
    {
        private readonly TextBox _obj = new TextBox();
        private readonly TextBox _ilm = new TextBox();
        private readonly CheckBox _autoTex = new CheckBox();
        private readonly CheckBox _winding = new CheckBox();
        private readonly CheckBox _mirror = new CheckBox();
        private readonly CheckBox _seams = new CheckBox();
        private readonly Button _ok = new Button();
        private static string s_lastIlm;   // remembered donor for the session

        public string ObjPath { get { return _obj.Text; } }
        public string IlmPath { get { return _ilm.Text; } }
        public bool AutoTexture { get { return _autoTex.Checked; } }
        public bool DoWinding { get { return _winding.Checked; } }
        public bool DoMirror { get { return _mirror.Checked; } }
        public bool DoSeams { get { return _seams.Checked; } }
        /// <summary>True when the user clicked "Simple replacement…" — the caller should run the
        /// edit-existing flow instead of the high-poly build (the browse fields may be empty).</summary>
        public bool Simple { get; private set; }

        public HighPolyDialog(string gameRoot)
        {
            Text = "High-poly model replacement";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(500, 336);

            var lblObj = new Label { Text = "Your rigged model (.obj):", Location = new Point(14, 14), AutoSize = true };
            _obj.SetBounds(16, 34, 378, 22); _obj.ReadOnly = true;
            var brObj = new Button { Text = "Browse…", Location = new Point(402, 32), Size = new Size(82, 26) };
            brObj.Click += delegate { BrowseObj(); };

            var lblIlm = new Label { Text = "Original character to replace (.ILM):", Location = new Point(14, 66), AutoSize = true };
            _ilm.SetBounds(16, 86, 378, 22); _ilm.ReadOnly = true;
            if (s_lastIlm != null && File.Exists(s_lastIlm)) _ilm.Text = s_lastIlm;
            var brIlm = new Button { Text = "Browse…", Location = new Point(402, 84), Size = new Size(82, 26) };
            brIlm.Click += delegate { BrowseIlm(gameRoot); };

            var grp = new GroupBox { Text = "Automatic fixes", Location = new Point(16, 120), Size = new Size(468, 150) };
            _autoTex.Text = "Auto-texture — pack the model's textures into one sheet and fix the UVs";
            _autoTex.SetBounds(14, 24, 448, 22); _autoTex.Checked = true;
            _winding.Text = "Fix winding — face the geometry outward (stops holes). Safe to leave on.";
            _winding.SetBounds(14, 52, 448, 22); _winding.Checked = true;
            _mirror.Text = "Fix left/right (mirror) — only if the arms/legs came out swapped";
            _mirror.SetBounds(14, 80, 448, 22);
            _seams.Text = "Close seams (rough) — auto-overlap joints; by hand in Blender is cleaner";
            _seams.SetBounds(14, 108, 448, 22);
            grp.Controls.AddRange(new Control[] { _autoTex, _winding, _mirror, _seams });

            var help = new Button { Text = "Help", Location = new Point(16, 296), Size = new Size(82, 28) };
            help.Click += delegate { ShowHelp(); };
            var simpleBtn = new Button { Text = "Simple replacement…", Location = new Point(104, 296), Size = new Size(150, 28) };
            simpleBtn.Click += delegate { Simple = true; DialogResult = DialogResult.OK; };
            _ok.Text = "OK"; _ok.SetBounds(316, 296, 82, 28); _ok.Enabled = false; _ok.DialogResult = DialogResult.OK;
            _ok.Click += delegate { s_lastIlm = _ilm.Text; };
            var cancel = new Button { Text = "Cancel", Location = new Point(402, 296), Size = new Size(82, 28), DialogResult = DialogResult.Cancel };

            _obj.TextChanged += delegate { UpdateOk(); };
            _ilm.TextChanged += delegate { UpdateOk(); };
            UpdateOk();

            Controls.AddRange(new Control[] { lblObj, _obj, brObj, lblIlm, _ilm, brIlm, grp, help, simpleBtn, _ok, cancel });
            AcceptButton = _ok; CancelButton = cancel;
        }

        private void UpdateOk() { _ok.Enabled = File.Exists(_obj.Text) && File.Exists(_ilm.Text); }

        private void BrowseObj()
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = "Select your rigged model (.obj)";
                d.Filter = "Wavefront OBJ (*.obj)|*.obj|All files (*.*)|*.*";
                if (_obj.Text.Length > 0) d.InitialDirectory = Path.GetDirectoryName(_obj.Text);
                if (d.ShowDialog(this) == DialogResult.OK) _obj.Text = d.FileName;
            }
        }

        private void BrowseIlm(string gameRoot)
        {
            using (var d = new OpenFileDialog())
            {
                d.Title = "Select the original character (.ILM) to replace";
                d.Filter = "Model files (*.ilm)|*.ilm|All files (*.*)|*.*";
                if (_ilm.Text.Length > 0) d.InitialDirectory = Path.GetDirectoryName(_ilm.Text);
                else if (!string.IsNullOrEmpty(gameRoot))
                {
                    string g = Path.Combine(gameRoot, "gamedata");
                    if (Directory.Exists(g)) d.InitialDirectory = g;
                }
                if (d.ShowDialog(this) == DialogResult.OK) _ilm.Text = d.FileName;
            }
        }

        private void ShowHelp()
        {
            MessageBox.Show(this,
                "HIGH-POLY MODEL REPLACEMENT\n\n" +
                "Swaps a whole new mesh (e.g. a ripped character) onto the game's skeleton at full " +
                "detail — no vertex-count limit.\n\n" +
                "• Your model (.obj): a model you cut and named to the 23 parts in Blender. Export the " +
                "character you're replacing with Model → OBJ first to get the reference skeleton to rig " +
                "against, and keep your model's .mtl beside the .obj.\n\n" +
                "• Original character (.ILM): which character to replace (e.g. HERO.ILM from gamedata) — " +
                "it supplies the skeleton, materials and rest pose.\n\n" +
                "AUTO-TEXTURE: packs your textures (from the .mtl) into one sheet, fixes the UVs, and sets " +
                "up materials. Turn off only if your OBJ is already a single sheet using the game's " +
                "material names.\n\n" +
                "FIX WINDING: makes faces point outward so the model doesn't show through itself. Safe on.\n\n" +
                "FIX LEFT/RIGHT: mirrors the model — use ONLY if the limbs came out swapped (you'll get a " +
                "warning). It also flips the texture left-right, so leave it off unless needed.\n\n" +
                "CLOSE SEAMS (rough): auto-overlaps the joints to hide gaps, but it's approximate — dragging " +
                "the boundary vertices together in Blender is cleaner. Off by default.\n\n" +
                "SIMPLE REPLACEMENT…: switches to the basic editor for RESHAPING an existing character " +
                "(patch / grow / replace within the vertex limit), instead of swapping in a new high-poly mesh.\n\n" +
                "Output: a new .ILM and a .TIM.png sheet. Drop BOTH into gamedata\\load\\CHARA\\ under the " +
                "ORIGINAL names and set allow_loose_files = 1.",
                "Help — High-poly replacement", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
    }
}
