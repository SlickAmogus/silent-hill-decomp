using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Windows.Forms;

/// <summary>
/// Separate window for editing keyboard + controller bindings. Code-generated
/// (no Designer) so the ~28 rows stay maintainable. Shares Form1's
/// ConfigManager so edits land in the same config.cfg on Save.
///
/// Keyboard boxes are click-to-rebind: focus one and press a key; the binding
/// is set to that key's SDL scancode name (no free typing, so you can't save
/// an invalid name). Controller values are dropdowns of valid SDL button names.
/// "NONE" = unbound. D-pad + analog sticks are movement and are not rebindable.
/// </summary>
public class ControlsForm : Form
{
    private readonly ConfigManager config;

    // { display label, config key } per bindable PSX button.
    private static readonly string[][] KeyboardBinds =
    {
        new[] { "Move Up",          "key_up" },
        new[] { "Move Down",        "key_down" },
        new[] { "Turn Left",        "key_left" },
        new[] { "Turn Right",       "key_right" },
        new[] { "Action / Shoot",   "key_cross" },
        new[] { "Flashlight",       "key_circle" },
        new[] { "Map",              "key_triangle" },
        new[] { "Run",              "key_square" },
        new[] { "Sidestep Left",    "key_l1" },
        new[] { "Sidestep Right",   "key_r1" },
        new[] { "View",             "key_l2" },
        new[] { "Aim",              "key_r2" },
        new[] { "Pause",            "key_start" },
        new[] { "Select",           "key_select" },
    };

    // PC-only hotkeys, shown under the PSX binds with a small gap.
    private static readonly string[][] QuickBinds =
    {
        new[] { "Quick Save",       "key_quicksave" },
        new[] { "Quick Load",       "key_quickload" },
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
        { "key_quicksave", "F6" }, { "key_quickload", "F8" },
        { "pad_cross", "a" }, { "pad_circle", "b" }, { "pad_triangle", "y" }, { "pad_square", "x" },
        { "pad_l1", "leftshoulder" }, { "pad_r1", "rightshoulder" }, { "pad_l2", "lefttrigger" }, { "pad_r2", "righttrigger" },
        { "pad_l3", "leftstick" }, { "pad_r3", "rightstick" }, { "pad_start", "start" }, { "pad_select", "back" },
    };

    // WinForms Keys -> SDL scancode name (what PsyX_LookupKeyboardMapping wants).
    private static readonly Dictionary<Keys, string> KeyToSdl = BuildKeyMap();

    private const string ListenPrompt = "Press a key...";

    private readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();
    private RadioButton debugYes;
    private RadioButton debugNo;

    private static readonly Color Back = Color.FromArgb(30, 30, 30);
    private static readonly Color PanelBack = Color.FromArgb(45, 45, 45);
    private static readonly Color Listening = Color.FromArgb(70, 70, 95);
    private static readonly Color TextColor = Color.White;

    public ControlsForm(ConfigManager configManager)
    {
        config = configManager;
        BuildUi();
        LoadValues();
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        // Don't start with a keyboard box focused (it would show "Press a
        // key..."). Move focus to the Save button; the box's Leave restores it.
        if (ActiveControl is TextBox)
            ActiveControl = AcceptButton as Button;
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
        ClientSize = new Size(640, 600);

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
            AddKeyRow(KeyboardBinds[i][0], KeyboardBinds[i][1], colKbX, rowY0 + i * rowH, labelW, inputW);

        // Quick Save / Quick Load: under the PSX binds, set off by a small gap.
        int quickY0 = rowY0 + KeyboardBinds.Length * rowH + 12;
        for (int i = 0; i < QuickBinds.Length; i++)
            AddKeyRow(QuickBinds[i][0], QuickBinds[i][1], colKbX, quickY0 + i * rowH, labelW, inputW);

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

        // Below the (taller) keyboard column.
        int debugY = quickY0 + QuickBinds.Length * rowH + 16;
        AddLabel("Allow debug controls:", colKbX, debugY, 150);
        debugYes = new RadioButton { Text = "Yes", Left = colKbX + 160, Top = debugY - 3, Width = 50, ForeColor = TextColor };
        debugNo = new RadioButton { Text = "No", Left = colKbX + 215, Top = debugY - 3, Width = 50, ForeColor = TextColor };
        Controls.Add(debugYes);
        Controls.Add(debugNo);

        Button btnReset = new Button { Text = "Reset to Defaults", Width = 130, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        btnReset.Left = 20;
        btnReset.Top = ClientSize.Height - 42;
        btnReset.Click += (s, e) => ResetDefaults();
        Controls.Add(btnReset);

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

    private void AddKeyRow(string label, string cfgKey, int x, int y, int labelW, int inputW)
    {
        AddLabel(label, x, y, labelW);

        TextBox tb = new TextBox
        {
            Left = x + labelW,
            Top = y - 3,
            Width = inputW,
            ReadOnly = true,           // click-to-rebind; no free typing
            Cursor = Cursors.Hand,
            BackColor = PanelBack,
            ForeColor = TextColor,
            BorderStyle = BorderStyle.FixedSingle,
        };
        tb.Enter += KeyBox_Enter;
        tb.Leave += KeyBox_Leave;
        tb.PreviewKeyDown += KeyBox_PreviewKeyDown; // make arrows/Tab reach KeyDown
        tb.KeyDown += KeyBox_KeyDown;
        inputs[cfgKey] = tb;
        Controls.Add(tb);
    }

    // --- Keyboard rebind capture ----------------------------------------

    private void KeyBox_Enter(object sender, EventArgs e)
    {
        TextBox tb = (TextBox)sender;
        tb.Tag = tb.Text;            // remember current value
        tb.Text = ListenPrompt;
        tb.BackColor = Listening;
    }

    private void KeyBox_Leave(object sender, EventArgs e)
    {
        TextBox tb = (TextBox)sender;
        if (tb.Text == ListenPrompt && tb.Tag != null)
            tb.Text = (string)tb.Tag; // left without pressing a key -> restore
        tb.BackColor = PanelBack;
    }

    private void KeyBox_PreviewKeyDown(object sender, PreviewKeyDownEventArgs e)
    {
        // Treat every key (incl. arrows/Tab) as input so KeyDown fires for it.
        e.IsInputKey = true;
    }

    private void KeyBox_KeyDown(object sender, KeyEventArgs e)
    {
        e.SuppressKeyPress = true;
        e.Handled = true;

        TextBox tb = (TextBox)sender;
        Keys kc = ResolveSidedModifier(e.KeyCode);
        string name;
        if (KeyToSdl.TryGetValue(kc, out name))
        {
            tb.Text = name;
            tb.Tag = name;          // so Leave keeps it
            ActiveControl = null;   // commit + stop listening
        }
        // Unmapped key: keep listening for a recognized one.
    }

    [DllImport("user32.dll")]
    private static extern short GetKeyState(int nVirtKey);

    private const int VK_RSHIFT = 0xA1;
    private const int VK_RCONTROL = 0xA3;
    private const int VK_RMENU = 0xA5;

    // WinForms KeyDown reports a side-less ShiftKey/ControlKey/Menu. Resolve the
    // actual side from the live key state so Left/Right Shift (etc.) can bind.
    private static Keys ResolveSidedModifier(Keys k)
    {
        if (k == Keys.ShiftKey)   return (GetKeyState(VK_RSHIFT)   & 0x8000) != 0 ? Keys.RShiftKey   : Keys.LShiftKey;
        if (k == Keys.ControlKey) return (GetKeyState(VK_RCONTROL) & 0x8000) != 0 ? Keys.RControlKey : Keys.LControlKey;
        if (k == Keys.Menu)       return (GetKeyState(VK_RMENU)    & 0x8000) != 0 ? Keys.RMenu       : Keys.LMenu;
        return k;
    }

    // --- Load / save ----------------------------------------------------

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

    private void ResetDefaults()
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            string def = Defaults.ContainsKey(kv.Key) ? Defaults[kv.Key] : "";

            TextBox tb = kv.Value as TextBox;
            if (tb != null)
            {
                tb.Text = def;
                tb.Tag = def;
                continue;
            }

            ComboBox cb = kv.Value as ComboBox;
            if (cb != null)
            {
                int idx = cb.Items.IndexOf(def);
                cb.SelectedIndex = idx >= 0 ? idx : 0;
            }
        }

        debugNo.Checked = true;
        debugYes.Checked = false;
    }

    private void SaveValues()
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            string val;
            TextBox tb = kv.Value as TextBox;
            if (tb != null)
                val = (tb.Text == ListenPrompt) ? (tb.Tag as string ?? "") : tb.Text.Trim();
            else
                val = ((ComboBox)kv.Value).SelectedItem != null ? ((ComboBox)kv.Value).SelectedItem.ToString() : "";

            if (val.Length == 0) val = "NONE";
            config.Set(kv.Key, val);
        }

        config.Set("allow_debug_controls", debugYes.Checked ? "1" : "0");
        config.Save();
    }

    // --- WinForms Keys -> SDL scancode name -----------------------------

    private static Dictionary<Keys, string> BuildKeyMap()
    {
        var m = new Dictionary<Keys, string>();

        for (char c = 'A'; c <= 'Z'; c++)
            m[(Keys)c] = c.ToString();                 // A..Z

        for (int d = 0; d <= 9; d++)
        {
            m[Keys.D0 + d] = d.ToString();             // top-row 0..9
            m[Keys.NumPad0 + d] = "Keypad " + d;       // keypad 0..9
        }

        for (int f = 1; f <= 12; f++)
            m[Keys.F1 + (f - 1)] = "F" + f;            // F1..F12

        m[Keys.Up] = "Up"; m[Keys.Down] = "Down"; m[Keys.Left] = "Left"; m[Keys.Right] = "Right";
        m[Keys.Space] = "Space"; m[Keys.Return] = "Return"; m[Keys.Tab] = "Tab";
        m[Keys.Back] = "Backspace"; m[Keys.Escape] = "Escape";
        m[Keys.LShiftKey] = "Left Shift"; m[Keys.RShiftKey] = "Right Shift";
        m[Keys.LControlKey] = "Left Ctrl"; m[Keys.RControlKey] = "Right Ctrl";
        m[Keys.LMenu] = "Left Alt"; m[Keys.RMenu] = "Right Alt";
        m[Keys.Insert] = "Insert"; m[Keys.Delete] = "Delete";
        m[Keys.Home] = "Home"; m[Keys.End] = "End";
        m[Keys.PageUp] = "PageUp"; m[Keys.PageDown] = "PageDown";
        m[Keys.CapsLock] = "CapsLock";
        m[Keys.Multiply] = "Keypad *"; m[Keys.Add] = "Keypad +";
        m[Keys.Subtract] = "Keypad -"; m[Keys.Divide] = "Keypad /"; m[Keys.Decimal] = "Keypad .";
        m[Keys.OemOpenBrackets] = "["; m[Keys.OemCloseBrackets] = "]";
        m[Keys.OemSemicolon] = ";"; m[Keys.Oemcomma] = ","; m[Keys.OemPeriod] = ".";
        m[Keys.OemQuestion] = "/"; m[Keys.Oemtilde] = "`"; m[Keys.OemMinus] = "-";
        m[Keys.Oemplus] = "="; m[Keys.OemPipe] = "\\"; m[Keys.OemQuotes] = "'";

        return m;
    }
}
