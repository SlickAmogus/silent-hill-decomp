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
///
/// Experimental: a Control Style dropdown (Classic / Thirdperson Shooter, etc.;
/// list is published by the game in control_styles), a Change-Camera bind on
/// both keyboard + controller, and an "Allow mouse controls / secondary inputs"
/// toggle that reveals a second bind box per keyboard row and lets bind boxes
/// capture mouse buttons (Mouse1..Mouse5). Esc/Backspace clears a binding.
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
        new[] { "Inventory",        "key_select" },
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
        new[] { "Inventory",        "pad_select" },
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
        { "key_change_cam", "F9" }, { "pad_change_cam", "rightstick" },
        { "key_swap_shoulder", "Mouse3" },
        { "pad_cross", "a" }, { "pad_circle", "b" }, { "pad_triangle", "y" }, { "pad_square", "x" },
        { "pad_l1", "leftshoulder" }, { "pad_r1", "rightshoulder" }, { "pad_l2", "lefttrigger" }, { "pad_r2", "righttrigger" },
        { "pad_l3", "leftstick" }, { "pad_r3", "rightstick" }, { "pad_start", "start" }, { "pad_select", "back" },
        // Alternate binds: Action=Mouse1, Aim=Mouse2, Flashlight=F, Map=Tab,
        // Sidestep L/R=A/D; rest unbound.
        { "key_cross_2", "Mouse1" }, { "key_r2_2", "Mouse2" },
        { "key_circle_2", "F" }, { "key_triangle_2", "Tab" }, { "key_l1_2", "A" }, { "key_r1_2", "D" },
        { "key_up_2", "NONE" }, { "key_down_2", "NONE" }, { "key_left_2", "NONE" }, { "key_right_2", "NONE" },
        { "key_square_2", "NONE" }, { "key_l2_2", "NONE" },
        { "key_start_2", "NONE" }, { "key_select_2", "NONE" },
    };

    // WinForms Keys -> SDL scancode name (what PsyX_LookupKeyboardMapping wants).
    private static readonly Dictionary<Keys, string> KeyToSdl = BuildKeyMap();

    private const string ListenPrompt = "Press a key...";

    private readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();
    private readonly List<TextBox> secondaryBoxes = new List<TextBox>();
    private RadioButton debugYes;
    private RadioButton debugNo;

    private ComboBox cmbControlStyle;
    private readonly List<string> styleIds = new List<string>();
    private CheckBox chkInvertMouseY;
    private CheckBox chkInvertControllerY;
    private CheckBox chkTpsAimZoom;
    private CheckBox chkCrosshair;

    // The click that focuses a bind box must NOT be captured as a Mouse1 binding.
    private bool ignoreNextMouseBind;

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
        ClientSize = new Size(760, 610);

        const int colKbX = 20;
        const int colPadX = 420;
        const int headerY = 14;
        const int rowY0 = 44;
        const int rowH = 26;
        const int labelW = 120;
        const int inputW = 118;
        const int padInputW = 150;
        const int secGap = 6;

        AddHeader("Keyboard Controls", colKbX, headerY);
        AddLabel("Alternate", colKbX + labelW + inputW + secGap, headerY + 4, inputW);
        AddHeader("Controller Controls", colPadX, headerY);

        // Keyboard PSX binds — each gets a hidden secondary box (shown when the
        // mouse/secondary toggle is on).
        for (int i = 0; i < KeyboardBinds.Length; i++)
            AddKeyRow(KeyboardBinds[i][0], KeyboardBinds[i][1], colKbX, rowY0 + i * rowH, labelW, inputW, true);

        // Quick Save / Quick Load: under the PSX binds, set off by a small gap.
        int quickY0 = rowY0 + KeyboardBinds.Length * rowH + 12;
        for (int i = 0; i < QuickBinds.Length; i++)
            AddKeyRow(QuickBinds[i][0], QuickBinds[i][1], colKbX, quickY0 + i * rowH, labelW, inputW, false);

        // Change Camera (keyboard) — between Quick Load and Allow Debug.
        int changeCamY = quickY0 + QuickBinds.Length * rowH + 8;
        AddKeyRow("Change Camera", "key_change_cam", colKbX, changeCamY, labelW, inputW, false);

        // Swap Shoulder (OTS side) — defaults to middle mouse; rebindable.
        int swapShoulderY = changeCamY + rowH;
        AddKeyRow("Swap Shoulder", "key_swap_shoulder", colKbX, swapShoulderY, labelW, inputW, false);

        // Controller binds.
        for (int i = 0; i < ControllerBinds.Length; i++)
        {
            int y = rowY0 + i * rowH;
            AddLabel(ControllerBinds[i][0], colPadX, y, labelW);
            AddPadCombo(ControllerBinds[i][1], colPadX + labelW, y - 3, padInputW);
        }

        // Change Camera (controller) — below Select.
        int padChangeCamY = rowY0 + ControllerBinds.Length * rowH;
        AddLabel("Change Camera", colPadX, padChangeCamY, labelW);
        AddPadCombo("pad_change_cam", colPadX + labelW, padChangeCamY - 3, padInputW);

        // --- Experimental section (right column, gap left above for a future
        // control under Select) ---
        int expY = padChangeCamY + rowH + 16;
        AddHeader("Experimental", colPadX, expY);

        int styleY = expY + 30;
        AddLabel("Control Style", colPadX, styleY, 90);
        cmbControlStyle = new ComboBox
        {
            Left = colPadX + 90,
            Top = styleY - 3,
            Width = 180,
            DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = PanelBack,
            ForeColor = TextColor,
            FlatStyle = FlatStyle.Flat,
        };
        Controls.Add(cmbControlStyle);

        chkInvertMouseY = new CheckBox
        {
            Text = "Invert Mouse Y",
            Left = colPadX,
            Top = styleY + 30,
            Width = 160,
            ForeColor = TextColor,
        };
        chkInvertControllerY = new CheckBox
        {
            Text = "Invert Controller Y",
            Left = colPadX,
            Top = styleY + 56,
            Width = 180,
            ForeColor = TextColor,
        };
        chkTpsAimZoom = new CheckBox
        {
            Text = "TPS/OTS Zoom",
            Left = colPadX,
            Top = styleY + 82,
            Width = 180,
            ForeColor = TextColor,
        };
        chkCrosshair = new CheckBox
        {
            Text = "Crosshair (aiming, TPS/OTS)",
            Left = colPadX,
            Top = styleY + 108,
            Width = 220,
            ForeColor = TextColor,
        };
        Controls.Add(chkInvertMouseY);
        Controls.Add(chkInvertControllerY);
        Controls.Add(chkTpsAimZoom);
        Controls.Add(chkCrosshair);

        // Allow debug controls — below the (taller) keyboard column.
        int debugY = swapShoulderY + rowH + 14;
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

    private void AddPadCombo(string cfgKey, int left, int top, int width)
    {
        ComboBox cb = new ComboBox
        {
            Left = left,
            Top = top,
            Width = width,
            DropDownStyle = ComboBoxStyle.DropDownList,
            BackColor = PanelBack,
            ForeColor = TextColor,
            FlatStyle = FlatStyle.Flat,
        };
        cb.Items.AddRange(ControllerButtons);
        inputs[cfgKey] = cb;
        Controls.Add(cb);
    }

    private TextBox MakeBindBox(string cfgKey, int left, int top, int width)
    {
        TextBox tb = new TextBox
        {
            Left = left,
            Top = top,
            Width = width,
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
        tb.MouseDown += KeyBox_MouseDown;           // bind mouse buttons (when enabled)
        inputs[cfgKey] = tb;
        Controls.Add(tb);
        return tb;
    }

    private void AddKeyRow(string label, string cfgKey, int x, int y, int labelW, int inputW, bool withSecondary)
    {
        AddLabel(label, x, y, labelW);
        MakeBindBox(cfgKey, x + labelW, y - 3, inputW);

        if (withSecondary)
        {
            TextBox tb2 = MakeBindBox(cfgKey + "_2", x + labelW + inputW + 6, y - 3, inputW);
            secondaryBoxes.Add(tb2);   // alternate binds are always shown
        }
    }

    // --- Keyboard rebind capture ----------------------------------------

    private void KeyBox_Enter(object sender, EventArgs e)
    {
        TextBox tb = (TextBox)sender;
        tb.Tag = tb.Text;            // remember current value
        tb.Text = ListenPrompt;
        tb.BackColor = Listening;
        // The click that focuses a box must not also bind as Mouse1.
        ignoreNextMouseBind = true;
    }

    private void KeyBox_Leave(object sender, EventArgs e)
    {
        TextBox tb = (TextBox)sender;
        if (tb.Text == ListenPrompt && tb.Tag != null)
            tb.Text = (string)tb.Tag; // left without pressing a key -> restore
        tb.BackColor = PanelBack;
        ignoreNextMouseBind = false;
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

        // Esc / Backspace clears the binding rather than binding that key.
        if (e.KeyCode == Keys.Escape || e.KeyCode == Keys.Back)
        {
            tb.Text = "NONE";
            tb.Tag = "NONE";
            ActiveControl = null;   // commit + stop listening
            return;
        }

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

    private void KeyBox_MouseDown(object sender, MouseEventArgs e)
    {
        TextBox tb = (TextBox)sender;

        // Only bind when the box is actively listening (focused, showing the
        // prompt). This skips the focusing click whether MouseDown fires before
        // KeyBox_Enter (not listening yet) or after it (guarded below).
        if (tb.Text != ListenPrompt)
            return;

        // The click that focused the box arrives right after KeyBox_Enter armed
        // the guard — swallow it so it doesn't self-bind as Mouse1.
        if (ignoreNextMouseBind)
        {
            ignoreNextMouseBind = false;
            return;
        }

        string name = MouseButtonName(e.Button);
        if (name == null)
            return;

        tb.Text = name;
        tb.Tag = name;
        ActiveControl = null;       // commit + stop listening
    }

    private static string MouseButtonName(MouseButtons b)
    {
        switch (b)
        {
            case MouseButtons.Left:     return "Mouse1";
            case MouseButtons.Right:    return "Mouse2";
            case MouseButtons.Middle:   return "Mouse3";
            case MouseButtons.XButton1: return "Mouse4";
            case MouseButtons.XButton2: return "Mouse5";
            default:                    return null;
        }
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

    private void PopulateControlStyles()
    {
        cmbControlStyle.Items.Clear();
        styleIds.Clear();

        // The game publishes "id:Label|id:Label|..." each launch.
        string raw = config.Get("control_styles", "classic:Classic (Default)|tps:Thirdperson Shooter");
        foreach (string part in raw.Split('|'))
        {
            if (part.Trim().Length == 0) continue;
            int c = part.IndexOf(':');
            string id = (c >= 0 ? part.Substring(0, c) : part).Trim();
            string label = (c >= 0 ? part.Substring(c + 1) : part).Trim();
            if (id.Length == 0) continue;
            styleIds.Add(id);
            cmbControlStyle.Items.Add(label.Length > 0 ? label : id);
        }
        if (cmbControlStyle.Items.Count == 0)
        {
            styleIds.Add("classic");
            cmbControlStyle.Items.Add("Classic (Default)");
        }

        string cur = config.Get("control_style", "classic");
        int idx = styleIds.IndexOf(cur);
        cmbControlStyle.SelectedIndex = idx >= 0 ? idx : 0;
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

        PopulateControlStyles();

        chkInvertMouseY.Checked = config.Get("invert_mouse_y", "0") == "1";
        chkInvertControllerY.Checked = config.Get("invert_controller_y", "0") == "1";
        chkTpsAimZoom.Checked = config.Get("tps_aim_zoom", "1") == "1";
        chkCrosshair.Checked = config.Get("crosshair", "0") == "1";

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

        if (cmbControlStyle.Items.Count > 0)
            cmbControlStyle.SelectedIndex = 0;     // Classic
        chkInvertMouseY.Checked = false;
        chkInvertControllerY.Checked = false;
        chkTpsAimZoom.Checked = true;
        chkCrosshair.Checked = false;

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

        if (cmbControlStyle.SelectedIndex >= 0 && cmbControlStyle.SelectedIndex < styleIds.Count)
            config.Set("control_style", styleIds[cmbControlStyle.SelectedIndex]);
        config.Set("invert_mouse_y", chkInvertMouseY.Checked ? "1" : "0");
        config.Set("invert_controller_y", chkInvertControllerY.Checked ? "1" : "0");
        config.Set("tps_aim_zoom", chkTpsAimZoom.Checked ? "1" : "0");
        config.Set("crosshair", chkCrosshair.Checked ? "1" : "0");

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
