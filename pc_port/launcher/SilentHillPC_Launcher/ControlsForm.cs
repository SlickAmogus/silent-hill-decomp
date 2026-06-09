using System;
using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;

/// <summary>
/// Separate window for editing keyboard + controller bindings. Code-generated
/// (no Designer) so the ~28 rows stay maintainable. Shares Form1's
/// ConfigManager so edits land in the same config.cfg on Save.
///
/// Keyboard values are SDL scancode names ("C", "Z", "Space", "Up",
/// "Left Shift", "["). Controller values are SDL game-controller button names
/// ("a","b","x","y","leftshoulder","righttrigger","leftstick","start","back",
/// "guide"). "NONE" = unbound. D-pad + analog sticks are movement and are not
/// rebindable here.
/// </summary>
public class ControlsForm : Form
{
    private readonly ConfigManager config;

    // { display label, config key } per bindable PSX button.
    private static readonly string[][] KeyboardBinds =
    {
        new[] { "Move Up",          "key_up" },
        new[] { "Move Down",        "key_down" },
        new[] { "Move Left",        "key_left" },
        new[] { "Move Right",       "key_right" },
        new[] { "Action / Shoot",   "key_cross" },
        new[] { "Flashlight",       "key_circle" },
        new[] { "Map",              "key_triangle" },
        new[] { "Run",              "key_square" },
        new[] { "Sidestep Left",    "key_l1" },
        new[] { "Sidestep Right",   "key_r1" },
        new[] { "View",             "key_l2" },
        new[] { "Aim",              "key_r2" },
        new[] { "Stick Click (L3)", "key_l3" },
        new[] { "Stick Click (R3)", "key_r3" },
        new[] { "Pause",            "key_start" },
        new[] { "Select",           "key_select" },
    };

    private static readonly string[][] ControllerBinds =
    {
        new[] { "Action / Shoot",   "pad_cross" },
        new[] { "Flashlight",       "pad_circle" },
        new[] { "Map",              "pad_triangle" },
        new[] { "Run",              "pad_square" },
        new[] { "Sidestep Left",    "pad_l1" },
        new[] { "Sidestep Right",   "pad_r1" },
        new[] { "View",             "pad_l2" },
        new[] { "Aim",              "pad_r2" },
        new[] { "Stick Click (L3)", "pad_l3" },
        new[] { "Stick Click (R3)", "pad_r3" },
        new[] { "Pause",            "pad_start" },
        new[] { "Select",           "pad_select" },
    };

    // Valid controller buttons (SDL names). D-pad/sticks excluded (movement).
    private static readonly string[] ControllerButtons =
    {
        "a", "b", "x", "y",
        "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
        "leftstick", "rightstick", "start", "back", "guide", "NONE",
    };

    // Default binding for each key, so a config without the line shows the
    // engine default rather than blank. Mirrors pc_config.c.
    private static readonly Dictionary<string, string> Defaults =
        new Dictionary<string, string>
    {
        { "key_up", "Up" }, { "key_down", "Down" }, { "key_left", "Left" }, { "key_right", "Right" },
        { "key_cross", "C" }, { "key_circle", "V" }, { "key_triangle", "Z" }, { "key_square", "X" },
        { "key_l1", "A" }, { "key_r1", "D" }, { "key_l2", "Right Shift" }, { "key_r2", "Left Shift" },
        { "key_l3", "[" }, { "key_r3", "]" }, { "key_start", "Return" }, { "key_select", "Space" },
        { "pad_cross", "a" }, { "pad_circle", "b" }, { "pad_triangle", "y" }, { "pad_square", "x" },
        { "pad_l1", "leftshoulder" }, { "pad_r1", "rightshoulder" }, { "pad_l2", "lefttrigger" }, { "pad_r2", "righttrigger" },
        { "pad_l3", "leftstick" }, { "pad_r3", "rightstick" }, { "pad_start", "start" }, { "pad_select", "back" },
    };

    private readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();
    private RadioButton debugYes;
    private RadioButton debugNo;

    private static readonly Color Back = Color.FromArgb(30, 30, 30);
    private static readonly Color PanelBack = Color.FromArgb(45, 45, 45);
    private static readonly Color TextColor = Color.White;

    public ControlsForm(ConfigManager configManager)
    {
        config = configManager;
        BuildUi();
        LoadValues();
    }

    private void BuildUi()
    {
        Text = "Controls";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Back;
        ForeColor = TextColor;
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(640, 560);

        const int colKbX = 20;
        const int colPadX = 330;
        const int headerY = 14;
        const int rowY0 = 44;
        const int rowH = 26;
        const int labelW = 140;
        const int inputW = 150;

        AddHeader("Keyboard Controls", colKbX, headerY);
        AddHeader("Controller Controls", colPadX, headerY);

        for (int i = 0; i < KeyboardBinds.Length; i++)
        {
            int y = rowY0 + i * rowH;
            AddLabel(KeyboardBinds[i][0], colKbX, y, labelW);

            TextBox tb = new TextBox
            {
                Left = colKbX + labelW,
                Top = y - 3,
                Width = inputW,
                BackColor = PanelBack,
                ForeColor = TextColor,
                BorderStyle = BorderStyle.FixedSingle,
            };
            inputs[KeyboardBinds[i][1]] = tb;
            Controls.Add(tb);
        }

        for (int i = 0; i < ControllerBinds.Length; i++)
        {
            int y = rowY0 + i * rowH;
            AddLabel(ControllerBinds[i][0], colPadX, y, labelW);

            ComboBox cb = new ComboBox
            {
                Left = colPadX + labelW,
                Top = y - 3,
                Width = inputW,
                DropDownStyle = ComboBoxStyle.DropDownList,
                BackColor = PanelBack,
                ForeColor = TextColor,
                FlatStyle = FlatStyle.Flat,
            };
            cb.Items.AddRange(ControllerButtons);
            inputs[ControllerBinds[i][1]] = cb;
            Controls.Add(cb);
        }

        int afterRowsY = rowY0 + KeyboardBinds.Length * rowH + 18;

        AddLabel("Allow debug controls:", colKbX, afterRowsY, 150);
        debugYes = new RadioButton { Text = "Yes", Left = colKbX + 160, Top = afterRowsY - 3, Width = 50, ForeColor = TextColor };
        debugNo = new RadioButton { Text = "No", Left = colKbX + 215, Top = afterRowsY - 3, Width = 50, ForeColor = TextColor };
        Controls.Add(debugYes);
        Controls.Add(debugNo);

        Label note = new Label
        {
            Text = "Keyboard uses SDL key names (C, Z, Space, Up, \"Left Shift\", [ ). NONE = unbound. " +
                   "D-pad and analog sticks are reserved for movement.",
            Left = colKbX,
            Top = ClientSize.Height - 80,
            Width = ClientSize.Width - 40,
            Height = 30,
            ForeColor = Color.Silver,
        };
        Controls.Add(note);

        Button btnSave = new Button { Text = "Save", Width = 90, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        Button btnCancel = new Button { Text = "Cancel", Width = 90, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        btnSave.Left = ClientSize.Width - 200;
        btnSave.Top = ClientSize.Height - 42;
        btnCancel.Left = ClientSize.Width - 100;
        btnCancel.Top = ClientSize.Height - 42;
        btnSave.Click += (s, e) => { SaveValues(); DialogResult = DialogResult.OK; Close(); };
        btnCancel.Click += (s, e) => { DialogResult = DialogResult.Cancel; Close(); };
        Controls.Add(btnSave);
        Controls.Add(btnCancel);
        AcceptButton = btnSave;
        CancelButton = btnCancel;
    }

    private void AddHeader(string text, int x, int y)
    {
        Controls.Add(new Label
        {
            Text = text,
            Left = x,
            Top = y,
            AutoSize = true,
            ForeColor = TextColor,
            Font = new Font("Segoe UI", 11f, FontStyle.Bold),
        });
    }

    private void AddLabel(string text, int x, int y, int w)
    {
        Controls.Add(new Label { Text = text, Left = x, Top = y, Width = w, ForeColor = TextColor });
    }

    private void LoadValues()
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            string def = Defaults.ContainsKey(kv.Key) ? Defaults[kv.Key] : "";
            string val = config.Get(kv.Key, def);

            TextBox tb = kv.Value as TextBox;
            if (tb != null)
            {
                tb.Text = val;
                continue;
            }

            ComboBox cb = kv.Value as ComboBox;
            if (cb != null)
            {
                int idx = cb.Items.IndexOf(val);
                if (idx < 0) idx = cb.Items.IndexOf(def);
                cb.SelectedIndex = idx >= 0 ? idx : 0;
            }
        }

        bool dbg = config.Get("allow_debug_controls", "0") == "1";
        debugYes.Checked = dbg;
        debugNo.Checked = !dbg;
    }

    private void SaveValues()
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            string val;
            TextBox tb = kv.Value as TextBox;
            if (tb != null)
                val = tb.Text.Trim();
            else
                val = ((ComboBox)kv.Value).SelectedItem != null ? ((ComboBox)kv.Value).SelectedItem.ToString() : "";

            if (val.Length == 0) val = "NONE";
            config.Set(kv.Key, val);
        }

        config.Set("allow_debug_controls", debugYes.Checked ? "1" : "0");
        config.Save();
    }
}
