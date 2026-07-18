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
        new[] { "Reload",           "key_reload" },
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

    // Valid controller buttons (SDL names). Sticks excluded (movement/look). The
    // D-pad is included so it can be bound to actions once "Disable D-pad for
    // movement" (Experimental) frees it from walking.
    private static readonly string[] ControllerButtons =
    {
        "a", "b", "x", "y",
        "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger",
        "leftstick", "rightstick", "start", "back", "guide",
        "dpup", "dpdown", "dpleft", "dpright", "NONE",
    };

    // Default binding for each key, so a config without the line shows the
    // engine default rather than blank. Mirrors pc_config.c.
    private static readonly Dictionary<string, string> Defaults =
        new Dictionary<string, string>
    {
        { "key_up", "Up" }, { "key_down", "Down" }, { "key_left", "Left" }, { "key_right", "Right" },
        { "key_cross", "C" }, { "key_circle", "V" }, { "key_triangle", "Z" }, { "key_square", "X" },
        { "key_l1", "A" }, { "key_r1", "D" }, { "key_l2", "Right Shift" }, { "key_r2", "Left Shift" },
        { "key_l3", "NONE" }, { "key_r3", "NONE" }, { "key_start", "Return" }, { "key_select", "Space" },
        { "key_quicksave", "F6" }, { "key_quickload", "F8" },
        { "key_change_cam", "F9" }, { "pad_change_cam", "rightstick" },
        { "key_swap_shoulder", "Mouse3" }, { "key_console", "`" },
        { "key_gfx_cycle", "\\" }, { "key_gfx_prev", "[" }, { "key_gfx_next", "]" },
        { "key_exit_game", "Escape" },
        { "key_reload", "R" }, { "key_reload_2", "NONE" },
        { "key_cycle_weapons", "NONE" }, { "key_quick_heal", "NONE" },
        { "pad_reload", "NONE" }, { "pad_cycle_weapons", "NONE" }, { "pad_quick_heal", "NONE" },
        { "key_quick_turn", "NONE" }, { "pad_quick_turn", "NONE" },
        { "key_rear_look", "NONE" }, { "pad_rear_look", "NONE" },
        { "pad_cross", "a" }, { "pad_circle", "b" }, { "pad_triangle", "y" }, { "pad_square", "x" },
        { "pad_l1", "leftshoulder" }, { "pad_r1", "rightshoulder" }, { "pad_l2", "lefttrigger" }, { "pad_r2", "righttrigger" },
        { "pad_l3", "leftstick" }, { "pad_r3", "rightstick" }, { "pad_start", "start" }, { "pad_select", "back" },
        // Classic per-action alternates default to NONE (unbound) — handled by the
        // NONE fallback, so none are seeded here.
    };

    // Modern (altcam) scheme defaults — applied while an alternate camera (TPS/OTS)
    // is active. Per-action keys not listed default to NONE (unbound).
    private static readonly Dictionary<string, string> AltDefaults =
        new Dictionary<string, string>
    {
        { "key_up", "W" }, { "key_down", "S" }, { "key_left", "Left" }, { "key_right", "Right" },
        { "key_cross", "Mouse1" }, { "key_circle", "F" }, { "key_triangle", "Tab" }, { "key_square", "Left Shift" },
        { "key_l1", "A" }, { "key_r1", "D" }, { "key_l2", "NONE" }, { "key_r2", "Mouse2" },
        { "key_start", "Return" }, { "key_select", "Space" },
        { "key_cross_2", "E" },
        { "pad_cross", "righttrigger" }, { "pad_circle", "b" }, { "pad_triangle", "y" }, { "pad_square", "leftshoulder" },
        { "pad_l1", "NONE" }, { "pad_r1", "NONE" }, { "pad_l2", "NONE" }, { "pad_r2", "lefttrigger" },
        { "pad_start", "start" }, { "pad_select", "back" },
        { "pad_cross_2", "a" },
        // PC-only actions (alt-cam scheme) — default to the same as classic until rebound.
        { "key_change_cam", "F9" }, { "pad_change_cam", "rightstick" },
        { "key_reload", "R" }, { "key_reload_2", "NONE" },
        { "pad_reload", "NONE" },
        { "key_cycle_weapons", "NONE" }, { "pad_cycle_weapons", "NONE" },
        { "key_quick_heal", "NONE" }, { "pad_quick_heal", "NONE" },
        { "key_quick_turn", "NONE" }, { "pad_quick_turn", "NONE" },
        { "key_rear_look", "NONE" }, { "pad_rear_look", "NONE" },
    };

    // Per-scheme bind keys (saved twice: classic as-is, altcam with an "_altcam"
    // suffix). Anything in `inputs` NOT in this set is a scheme-independent global.
    private static readonly HashSet<string> SchemeKeys = BuildSchemeKeys();

    private static HashSet<string> BuildSchemeKeys()
    {
        var s = new HashSet<string>();
        foreach (var b in KeyboardBinds)   { s.Add(b[1]); s.Add(b[1] + "_2"); }
        foreach (var b in ControllerBinds) { s.Add(b[1]); s.Add(b[1] + "_2"); }
        // PC-only actions are per-scheme too (classic vs alt-cam), riding the same
        // _altcam save/load path. key_reload / key_reload_2 come from the KeyboardBinds
        // loop above (Reload is a standard keyboard row); these standalone actions have
        // no secondary (_2) slot.
        foreach (var k in new[] { "key_change_cam", "pad_change_cam", "pad_reload",
                                  "key_cycle_weapons", "pad_cycle_weapons",
                                  "key_quick_heal", "pad_quick_heal",
                                  "key_quick_turn", "pad_quick_turn",
                                  "key_rear_look", "pad_rear_look" })
            s.Add(k);
        return s;
    }

    // WinForms Keys -> SDL scancode name (what PsyX_LookupKeyboardMapping wants).
    private static readonly Dictionary<Keys, string> KeyToSdl = BuildKeyMap();

    private const string ListenPrompt = "Press a key...";

    private readonly Dictionary<string, Control> inputs = new Dictionary<string, Control>();
    private readonly List<TextBox> secondaryBoxes = new List<TextBox>();
    // Rear Look rows (label + input, keyboard + controller): disabled unless the
    // alt-cam scheme is selected, since Rear Look only works in TPS/OTS.
    private readonly List<Control> rearLookControls = new List<Control>();
    private RadioButton debugYes;
    private RadioButton debugNo;

    private ComboBox cmbControlStyle;
    private readonly List<string> styleIds = new List<string>();
    private CheckBox chkInvertMouseY;
    private CheckBox chkInvertControllerY;
    private CheckBox chkTpsOtsAim;
    private CheckBox chkCrosshair;
    private CheckBox chkImmersiveFps;
    private CheckBox chkAimAssist;
    private CheckBox chk2dControls;
    private CheckBox chkButtonSprint;
    private CheckBox chkTpsCameraCollision;
    private CheckBox chkDisableDpad;
    private NumericUpDown numMouseSens;
    private NumericUpDown numControllerSens;
    private NumericUpDown numFpsFov;
    private NumericUpDown numTpsFov;
    private NumericUpDown numTpsAimZoom;
    private TrackBar trkMouseSens;
    private TrackBar trkControllerSens;
    private TrackBar trkFpsFov;
    private TrackBar trkTpsFov;
    private TrackBar trkTpsAimZoom;
    private bool syncingSens;   /* guards the numeric <-> slider mirroring */
    private CheckBox chkAltCamControls;
    private ToolTip  tips;

    // Two control schemes (0 = classic / default camera, 1 = altcam / any
    // alternate-modern camera). The UI shows ONE scheme's per-action binds at a
    // time; the "Alt. Cam Controls" checkbox switches which. Globals (change-cam,
    // quick save/load, swap-shoulder) are not schemed.
    private int activeScheme = 0;
    private readonly Dictionary<string, string>[] schemeValues =
        { new Dictionary<string, string>(), new Dictionary<string, string>() };

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
        /* Height fits the sensitivity column, which is now the tallest: its last
         * slider (TPS/OTS Aim Zoom) bottoms out at styleY + 346 = 722. The bottom
         * button row is placed from ClientSize.Height, so it follows automatically. */
        ClientSize = new Size(860, 868);

        tips = new ToolTip { AutoPopDelay = 20000, InitialDelay = 350, ReshowDelay = 80, ShowAlways = true };

        const int colKbX = 20;
        const int colPadX = 420;
        const int headerY = 14;
        const int rowY0 = 44;
        const int rowH = 26;
        const int labelW = 120;
        const int inputW = 118;
        const int padInputW = 80;   /* ~half width to fit the controller alternate column */
        const int secGap = 6;

        AddHeader("Keyboard Controls", colKbX, headerY);
        AddLabel("Alternate", colKbX + labelW + inputW + secGap, headerY + 4, inputW);
        AddHeader("Controller Controls", colPadX, headerY);
        AddLabel("Alternate", colPadX + labelW + padInputW + secGap, headerY + 4, padInputW);

        // "Alt. Cam Controls" toggle (top-right): swaps the keyboard + controller
        // binds shown between the default-camera scheme and the alternate-camera scheme.
        chkAltCamControls = new CheckBox
        {
            Text = "Alt. Cam Controls",
            Left = ClientSize.Width - 135,
            Top = headerY,
            Width = 150,
            ForeColor = TextColor,
        };
        chkAltCamControls.CheckedChanged += AltCamControls_CheckedChanged;
        Controls.Add(chkAltCamControls);

        // "?" sits directly under the "Alt. Cam Controls" text.
        Button btnAltCamHelp = new Button
        {
            Text = "?",
            Left = ClientSize.Width - 45,
            Top = headerY + 24,
            Width = 26,
            Height = 22,
            BackColor = PanelBack,
            ForeColor = TextColor,
            FlatStyle = FlatStyle.Flat,
        };
        btnAltCamHelp.Click += (s, e) => MessageBox.Show(this,
            "Leave the box unchecked to set classic controls, check it to set controls for modern TPS/OTS modes. " +
            "Each control style also has alternates so that you can use more than one button for the same action.",
            "Alternate Camera Controls", MessageBoxButtons.OK, MessageBoxIcon.Information);
        Controls.Add(btnAltCamHelp);

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

        // Console toggle key — keyboard-only, not per-camera, no alternate. Rebindable
        // for layouts that can't reach tilde.
        int consoleY = swapShoulderY + rowH;
        AddKeyRow("Console", "key_console", colKbX, consoleY, labelW, inputW, false);
        tips.SetToolTip(inputs["key_console"],
            "Developer console toggle (needs Allow debug controls = Yes). Hold to show/hide it; tap while open to type a command.");

        // Graphics-effect tuning keys (keyboard-only). Cycle picks which enabled
        // effect (flashlight / post-process / tonemap) is tuned; Prev/Next lower
        // and raise its intensity live. Defaults \ / [ / ].
        int gfxCycleY = consoleY + rowH + 8;
        AddKeyRow("Gfx: Cycle Effect", "key_gfx_cycle", colKbX, gfxCycleY, labelW, inputW, false);
        AddKeyRow("Gfx: Adjust Down",  "key_gfx_prev",  colKbX, gfxCycleY + rowH,     labelW, inputW, false);
        AddKeyRow("Gfx: Adjust Up",    "key_gfx_next",  colKbX, gfxCycleY + rowH * 2, labelW, inputW, false);
        tips.SetToolTip(inputs["key_gfx_cycle"],
            "Cycles which enabled graphics effect (per-pixel flashlight, post-process, tone mapping) the Adjust keys tune.");
        tips.SetToolTip(inputs["key_gfx_prev"], "Lowers the selected graphics effect's intensity (hold to repeat).");
        tips.SetToolTip(inputs["key_gfx_next"], "Raises the selected graphics effect's intensity (hold to repeat).");

        // Exit Game — quits at the title/main menu, warm-reboots to the title
        // otherwise. Default Esc; unbind with Del/Backspace/Esc like any other bind.
        int exitGameY = gfxCycleY + rowH * 3;
        AddKeyRow("Exit Game", "key_exit_game", colKbX, exitGameY, labelW, inputW, false);
        tips.SetToolTip(inputs["key_exit_game"],
            "Quits to desktop at the title/main menu; warm-reboots to the title during gameplay or a cutscene. Unbind to disable.");

        // PC-only action binds (keyboard). Reload is a standard row under Inventory
        // above; Cycle Weapons / Quick Heal / Quick Turn / Rear Look are here. Rear
        // Look works only in TPS/OTS, so its row is disabled unless the alt-cam scheme
        // is selected (top-right "Alt. Cam Controls").
        int cycleHealY = exitGameY + rowH + 8;
        AddKeyRow("Cycle Weapons", "key_cycle_weapons", colKbX, cycleHealY, labelW, inputW, false);
        AddKeyRow("Quick Heal", "key_quick_heal", colKbX, cycleHealY + rowH, labelW, inputW, false);
        AddKeyRow("Quick Turn", "key_quick_turn", colKbX, cycleHealY + rowH * 2, labelW, inputW, false);
        Label lblRearLookKb = AddLabel("Rear Look", colKbX, cycleHealY + rowH * 3, labelW);
        TextBox boxRearLookKb = MakeBindBox("key_rear_look", colKbX + labelW, cycleHealY + rowH * 3 - 3, inputW);
        rearLookControls.Add(lblRearLookKb);
        rearLookControls.Add(boxRearLookKb);
        tips.SetToolTip(inputs["key_cycle_weapons"],
            "Cycles the equipped weapon through the weapons you own, weakest to strongest.");
        tips.SetToolTip(inputs["key_quick_heal"],
            "Uses the most sensible healing item you are carrying (a stronger one when badly hurt, a drink otherwise).");
        tips.SetToolTip(inputs["key_quick_turn"],
            "Quick 180 turn — Harry spins to face the opposite direction (animated, not a snap).");
        tips.SetToolTip(boxRearLookKb,
            "Hold to swing the camera behind Harry (Thirdperson / Over-the-Shoulder only). Bind it with the alt-cam scheme selected.");

        // Controller binds — primary + an alternate (second button) per action.
        for (int i = 0; i < ControllerBinds.Length; i++)
        {
            int y = rowY0 + i * rowH;
            AddLabel(ControllerBinds[i][0], colPadX, y, labelW);
            AddPadCombo(ControllerBinds[i][1], colPadX + labelW, y - 3, padInputW);
            AddPadCombo(ControllerBinds[i][1] + "_2", colPadX + labelW + padInputW + secGap, y - 3, padInputW);
        }

        // Change Camera (controller) — below Select.
        int padChangeCamY = rowY0 + ControllerBinds.Length * rowH;
        AddLabel("Change Camera", colPadX, padChangeCamY, labelW);
        AddPadCombo("pad_change_cam", colPadX + labelW, padChangeCamY - 3, padInputW);

        // PC-only action binds (controller) — Reload / Cycle Weapons / Quick Heal /
        // Quick Turn / Rear Look, in the room freed below by shifting Experimental
        // down. Rear Look works only in TPS/OTS -> disabled unless alt-cam selected.
        int padReloadY = padChangeCamY + rowH;
        int padCycleY  = padReloadY + rowH;
        int padHealY   = padCycleY + rowH;
        int padQtY     = padHealY + rowH;
        int padRlY     = padQtY + rowH;
        AddLabel("Reload", colPadX, padReloadY, labelW);
        AddPadCombo("pad_reload", colPadX + labelW, padReloadY - 3, padInputW);
        AddLabel("Cycle Weapons", colPadX, padCycleY, labelW);
        AddPadCombo("pad_cycle_weapons", colPadX + labelW, padCycleY - 3, padInputW);
        AddLabel("Quick Heal", colPadX, padHealY, labelW);
        AddPadCombo("pad_quick_heal", colPadX + labelW, padHealY - 3, padInputW);
        AddLabel("Quick Turn", colPadX, padQtY, labelW);
        AddPadCombo("pad_quick_turn", colPadX + labelW, padQtY - 3, padInputW);
        Label lblRearLookPad = AddLabel("Rear Look", colPadX, padRlY, labelW);
        AddPadCombo("pad_rear_look", colPadX + labelW, padRlY - 3, padInputW);
        rearLookControls.Add(lblRearLookPad);
        rearLookControls.Add(inputs["pad_rear_look"]);
        foreach (Control _rl in rearLookControls) _rl.Enabled = chkAltCamControls.Checked;
        tips.SetToolTip(inputs["pad_reload"],
            "Reload the equipped firearm on the controller (same action as the keyboard Reload).");
        tips.SetToolTip(inputs["pad_cycle_weapons"],
            "Cycles the equipped weapon through the weapons you own, weakest to strongest.");
        tips.SetToolTip(inputs["pad_quick_heal"],
            "Uses the most sensible healing item you are carrying (a stronger one when badly hurt).");
        tips.SetToolTip(inputs["pad_quick_turn"],
            "Quick 180 turn — Harry spins to face the opposite direction (animated, not a snap).");
        tips.SetToolTip(inputs["pad_rear_look"],
            "Hold to swing the camera behind Harry (Thirdperson / Over-the-Shoulder only). Bind it with the alt-cam scheme selected.");

        // --- Experimental section (right column) ---
        // The whole left column (header, Control Style, all checkboxes) is shifted
        // down to clear the new controller binds added above (Reload / Cycle Weapons
        // / Quick Heal). The sensitivity sliders keep the original styleY so they do
        // NOT move; only the checkbox column (chkY) shifts.
        int styleY = padChangeCamY + rowH + 16 + 30;   // sensitivity column (unshifted)
        int chkY = styleY + 5 * rowH;                  // Experimental left column (shifted below the 5 controller PC rows)
        AddHeader("Experimental", colPadX, chkY - 30);
        AddLabel("Control Style", colPadX, chkY, 90);
        cmbControlStyle = new ComboBox
        {
            Left = colPadX + 90,
            Top = chkY - 3,
            Width = 90,             // half width — its old 180 overlapped the sensitivity column
            DropDownWidth = 180,    // keep the open list readable

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
            Top = chkY + 30,
            Width = 160,
            ForeColor = TextColor,
        };
        chkInvertControllerY = new CheckBox
        {
            Text = "Invert Controller Y",
            Left = colPadX,
            Top = chkY + 56,
            Width = 180,
            ForeColor = TextColor,
        };
        chkTpsOtsAim = new CheckBox
        {
            Text = "OTS aiming in Thirdperson",
            Left = colPadX,
            Top = chkY + 82,
            Width = 220,
            ForeColor = TextColor,
        };
        chkCrosshair = new CheckBox
        {
            Text = "Crosshair (aiming, TPS/OTS)",
            Left = colPadX,
            Top = chkY + 108,
            Width = 220,
            ForeColor = TextColor,
        };
        chkImmersiveFps = new CheckBox
        {
            Text = "Immersive FPS head tracking",
            Left = colPadX,
            Top = chkY + 134,
            Width = 240,
            ForeColor = TextColor,
        };
        Controls.Add(chkInvertMouseY);
        Controls.Add(chkInvertControllerY);
        Controls.Add(chkTpsOtsAim);
        Controls.Add(chkCrosshair);
        Controls.Add(chkImmersiveFps);
        tips.SetToolTip(chkImmersiveFps,
            "First-person only: the view follows Harry's animated head (idle sway / lean) with your mouse look layered " +
            "on top, easing in after you stand still for a moment. Off = the view is driven purely by the mouse.");

        chk2dControls = new CheckBox
        {
            Text = "2D Controls (screen-relative)",
            Left = colPadX,
            Top = chkY + 160,
            Width = 240,
            ForeColor = TextColor,
        };
        Controls.Add(chk2dControls);
        tips.SetToolTip(chk2dControls,
            "Screen-relative movement for every camera except First Person (Classic fixed cameras + Thirdperson / " +
            "Over-the-Shoulder): the stick / movement keys push Harry relative to the screen and he turns to face the " +
            "direction you press, instead of tank controls. Off = the original per-camera controls.");

        // --- Sensitivity column (second column, right of the checkbox stack):
        // label + value box on one row, a slider line underneath (mirrors the
        // numeric box both ways).
        int sensX = colPadX + 235;
        int sensW = 190;

        AddLabel("Mouse Sensitivity", sensX, styleY + 30, 125);
        numMouseSens = new NumericUpDown
        {
            Left = sensX + sensW - 60,
            Top = styleY + 27,
            Width = 60,
            DecimalPlaces = 1,
            Increment = 0.1m,
            Minimum = 0.1m,
            Maximum = 4.0m,
            BackColor = PanelBack,
            ForeColor = TextColor,
        };
        Controls.Add(numMouseSens);
        trkMouseSens = MakeSensSlider(sensX, styleY + 52, sensW);
        WireSensPair(numMouseSens, trkMouseSens);

        AddLabel("Controller Sensitivity", sensX, styleY + 96, 125);
        numControllerSens = new NumericUpDown
        {
            Left = sensX + sensW - 60,
            Top = styleY + 93,
            Width = 60,
            DecimalPlaces = 1,
            Increment = 0.1m,
            Minimum = 0.1m,
            Maximum = 4.0m,
            BackColor = PanelBack,
            ForeColor = TextColor,
        };
        Controls.Add(numControllerSens);
        trkControllerSens = MakeSensSlider(sensX, styleY + 118, sensW);
        WireSensPair(numControllerSens, trkControllerSens);

        AddLabel("First-person FOV", sensX, styleY + 162, 125);
        numFpsFov = new NumericUpDown
        {
            Left = sensX + sensW - 60,
            Top = styleY + 159,
            Width = 60,
            DecimalPlaces = 1,
            Increment = 1m,
            Minimum = 55m,
            Maximum = 110m,
            BackColor = PanelBack,
            ForeColor = TextColor,
        };
        Controls.Add(numFpsFov);
        trkFpsFov = MakeSensSlider(sensX, styleY + 184, sensW);
        trkFpsFov.Minimum = 55;
        trkFpsFov.Maximum = 110;
        WirePair(numFpsFov, trkFpsFov, 1m);
        tips.SetToolTip(numFpsFov,
            "Horizontal field of view (degrees, 4:3 basis) used ONLY while playing in First-person mode — menus, " +
            "cutscenes, and the other cameras keep the game's original projection. Default 71.1 = the game's " +
            "original FOV; 90 = standard FPS feel.");

        AddLabel("Thirdperson FOV", sensX, styleY + 228, 125);
        numTpsFov = new NumericUpDown
        {
            Left = sensX + sensW - 60,
            Top = styleY + 225,
            Width = 60,
            DecimalPlaces = 1,
            Increment = 1m,
            Minimum = 55m,
            Maximum = 110m,
            BackColor = PanelBack,
            ForeColor = TextColor,
        };
        Controls.Add(numTpsFov);
        trkTpsFov = MakeSensSlider(sensX, styleY + 250, sensW);
        trkTpsFov.Minimum = 55;
        trkTpsFov.Maximum = 110;
        WirePair(numTpsFov, trkTpsFov, 1m);
        tips.SetToolTip(numTpsFov,
            "Horizontal field of view (degrees, 4:3 basis) used ONLY while playing in Thirdperson or " +
            "Over-the-Shoulder. The Classic fixed cameras always keep the game's original projection. " +
            "Default 71.1 = the game's own FOV, so leaving it alone changes nothing.");

        AddLabel("TPS/OTS Aim Zoom", sensX, styleY + 294, 125);
        numTpsAimZoom = new NumericUpDown
        {
            Left = sensX + sensW - 60,
            Top = styleY + 291,
            Width = 60,
            DecimalPlaces = 0,
            Increment = 5m,
            Minimum = 0m,
            Maximum = 200m,
            BackColor = PanelBack,
            ForeColor = TextColor,
        };
        Controls.Add(numTpsAimZoom);
        trkTpsAimZoom = MakeSensSlider(sensX, styleY + 316, sensW);
        trkTpsAimZoom.Minimum = 0;
        trkTpsAimZoom.Maximum = 200;
        WirePair(numTpsAimZoom, trkTpsAimZoom, 1m);
        tips.SetToolTip(numTpsAimZoom,
            "How far the Thirdperson / Over-the-Shoulder camera pulls in behind Harry while you aim, as a percentage " +
            "of the zoom range. 100 (default) = the original zoom, 200 = a deeper 2x zoom, 0 = no zoom at all (this " +
            "replaces the old TPS/OTS Aim Zoom checkbox).");

        tips.SetToolTip(numMouseSens,
            "Mouse look-speed multiplier for the Thirdperson / Over-the-Shoulder / First-person cameras (1.0 = default).");
        tips.SetToolTip(numControllerSens,
            "Right-stick look-speed multiplier for the Thirdperson / Over-the-Shoulder / First-person cameras (1.0 = default).");

        chkAimAssist = new CheckBox
        {
            Text = "Aim Assist (TPS/OTS)",
            Left = colPadX,
            Top = chkY + 186,
            Width = 200,
            ForeColor = TextColor,
        };
        Controls.Add(chkAimAssist);

        chkButtonSprint = new CheckBox
        {
            Text = "Always use button based sprinting",
            Left = colPadX,
            Top = chkY + 212,
            Width = 260,
            ForeColor = TextColor,
        };
        Controls.Add(chkButtonSprint);
        tips.SetToolTip(chkButtonSprint,
            "Walk by default and only sprint while the bound Run control is held — like the classic control " +
            "style. Applies to the alternate cameras (Thirdperson / Over-the-Shoulder / First-person) and to " +
            "2D Controls under any camera. Off = pushing the stick most of the way also sprints.");

        chkTpsCameraCollision = new CheckBox
        {
            Text = "Allow thirdperson camera collision",
            Left = colPadX,
            Top = chkY + 238,
            /* Kept clear of the sensitivity column (Left = colPadX + 235): this row
             * now sits beside the Thirdperson FOV slider. */
            Width = 230,
            ForeColor = TextColor,
        };
        Controls.Add(chkTpsCameraCollision);
        tips.SetToolTip(chkTpsCameraCollision,
            "Thirdperson / Over-the-Shoulder cameras only: when a wall would come between the camera and Harry, " +
            "pull the camera in so it stays on his side of it (on = the default). Off = the camera holds its full " +
            "orbit distance and is allowed to pass through geometry.");

        chkDisableDpad = new CheckBox
        {
            Text = "Disable D-pad for movement",
            Left = colPadX,
            Top = chkY + 264,
            Width = 210,
            ForeColor = TextColor,
        };
        Controls.Add(chkDisableDpad);
        tips.SetToolTip(chkDisableDpad,
            "Stops the controller D-pad from walking / turning Harry, freeing it to be bound to actions — pick " +
            "dpup / dpdown / dpleft / dpright for Reload, Cycle Weapons, Quick Heal, or any controller bind above. " +
            "Keyboard arrow keys are unaffected. Off = the D-pad moves Harry as usual.");

        tips.SetToolTip(chkAimAssist,
            "Thirdperson / Over-the-Shoulder free-aim only (NOT first person): when the reticle is over an enemy " +
            "(mouse) or near one (controller), the shot is redirected onto the enemy's body so it connects instead of " +
            "grazing the narrow hitbox. Mouse = light 'hit anywhere on the body'; controller = stronger auto-aim. " +
            "Off = the bullet goes exactly where the reticle points.");

        tips.SetToolTip(cmbControlStyle,
            "Classic = original fixed cameras. Thirdperson Shooter = mouse / right-stick follow camera behind Harry. " +
            "Over the Shoulder = the same, offset to one side (middle mouse swaps sides).");
        tips.SetToolTip(chkTpsOtsAim,
            "Thirdperson only: raising the gun eases the camera over Harry's shoulder, so you aim with the same framing " +
            "as Over-the-Shoulder mode, then eases back to the centred camera when you lower it. The shoulder-swap bind " +
            "works while aiming too. Off = Thirdperson keeps its centred camera while aiming.");
        tips.SetToolTip(chkCrosshair,
            "Draws a small crosshair at the center of the screen while you're aiming in Thirdperson / Over-the-Shoulder mode.");

        // Allow debug controls — on the bottom button row, to the right of Reset
        // to Defaults (clear of the keyboard column AND the Reset button).
        int debugY = ClientSize.Height - 37;
        int debugX = 165;
        AddLabel("Allow debug controls:", debugX, debugY, 140);
        debugYes = new RadioButton { Text = "Yes", Left = debugX + 145, Top = debugY - 3, Width = 50, ForeColor = TextColor };
        debugNo = new RadioButton { Text = "No", Left = debugX + 200, Top = debugY - 3, Width = 50, ForeColor = TextColor };
        Controls.Add(debugYes);
        Controls.Add(debugNo);

        // Small hint, past the radio buttons: bind boxes also clear on Del (not
        // just Esc/Backspace) — easy to miss since it's not shown on the boxes.
        Controls.Add(new Label
        {
            Text = "Press Del to unbind",
            Left = debugX + 260,
            Top = debugY,
            AutoSize = true,
            ForeColor = Color.Gray,
        });

        Button btnReset = new Button { Text = "Reset to Defaults", Width = 130, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        btnReset.Left = 20;
        btnReset.Top = ClientSize.Height - 42;
        btnReset.Click += (s, e) => ResetDefaults();
        Controls.Add(btnReset);

        Button btnSave = new Button { Text = "Save", Width = 90, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        Button btnCancel = new Button { Text = "Cancel", Width = 90, Height = 30, BackColor = PanelBack, ForeColor = TextColor, FlatStyle = FlatStyle.Flat };
        btnSave.Left = ClientSize.Width - 220;
        btnSave.Top = ClientSize.Height - 42;
        btnCancel.Left = ClientSize.Width - 120;
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

    private Label AddLabel(string text, int x, int y, int w)
    {
        Label l = new Label { Text = text, Left = x, Top = y, Width = w, ForeColor = TextColor };
        Controls.Add(l);
        return l;
    }

    private void AddPadCombo(string cfgKey, int left, int top, int width)
    {
        ComboBox cb = new ComboBox
        {
            Left = left,
            Top = top,
            Width = width,
            DropDownWidth = 150,   /* box is narrow; keep the open list readable */
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
        tb.MouseWheel += KeyBox_MouseWheel;         // bind mouse scroll up/down
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

        // Esc / Backspace / Del clears the binding rather than binding that key.
        if (e.KeyCode == Keys.Escape || e.KeyCode == Keys.Back || e.KeyCode == Keys.Delete)
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

    // Bind the mouse wheel: scroll up/down while a box is listening captures
    // "MouseWheelUp"/"MouseWheelDown" (the game maps these to a tap).
    private void KeyBox_MouseWheel(object sender, MouseEventArgs e)
    {
        TextBox tb = (TextBox)sender;
        if (tb.Text != ListenPrompt || e.Delta == 0)
            return;

        string name = e.Delta > 0 ? "MouseWheelUp" : "MouseWheelDown";
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

    private void AddStyle(string id, string label)
    {
        if (string.IsNullOrEmpty(id) || styleIds.Contains(id)) return;
        styleIds.Add(id);
        cmbControlStyle.Items.Add(string.IsNullOrEmpty(label) ? id : label);
    }

    private void PopulateControlStyles()
    {
        cmbControlStyle.Items.Clear();
        styleIds.Clear();

        // Built-in styles this launcher knows — always offered, even if the game
        // hasn't republished control_styles yet (a stale / pre-OTS config.cfg).
        AddStyle("classic", "Classic (Default)");
        AddStyle("tps",     "Thirdperson Shooter");
        AddStyle("ots",     "Over the Shoulder");

        // Union in whatever the game published (forward-compat with future modes).
        string raw = config.Get("control_styles", "");
        foreach (string part in raw.Split('|'))
        {
            if (part.Trim().Length == 0) continue;
            int c = part.IndexOf(':');
            string id    = (c >= 0 ? part.Substring(0, c) : part).Trim();
            string label = (c >= 0 ? part.Substring(c + 1) : part).Trim();
            AddStyle(id, label);
        }

        string cur = config.Get("control_style", "classic");
        int idx = styleIds.IndexOf(cur);
        cmbControlStyle.SelectedIndex = idx >= 0 ? idx : 0;
    }

    // --- Scheme helpers -------------------------------------------------

    // Show one scheme's per-action binds in the (shared) UI controls.
    private void PopulateUiFromScheme(int scheme)
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            if (!SchemeKeys.Contains(kv.Key)) continue;
            string val = schemeValues[scheme].ContainsKey(kv.Key) ? schemeValues[scheme][kv.Key] : "NONE";
            SetControlValue(kv.Value, val, val);
        }
    }

    // Capture the UI's current per-action binds back into a scheme's value dict.
    private void FlushUiToScheme(int scheme)
    {
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            if (!SchemeKeys.Contains(kv.Key)) continue;
            schemeValues[scheme][kv.Key] = GetControlValue(kv.Value);
        }
    }

    private void AltCamControls_CheckedChanged(object sender, EventArgs e)
    {
        // Rear Look only works in TPS/OTS, so it is only editable in the alt-cam scheme.
        foreach (Control c in rearLookControls) c.Enabled = chkAltCamControls.Checked;

        int next = chkAltCamControls.Checked ? 1 : 0;
        if (next == activeScheme) return;
        FlushUiToScheme(activeScheme);   // keep edits to the scheme we're leaving
        activeScheme = next;
        PopulateUiFromScheme(activeScheme);
    }

    private static void SetControlValue(Control c, string val, string fallback)
    {
        TextBox tb = c as TextBox;
        if (tb != null) { tb.Text = val; tb.Tag = val; return; }
        ComboBox cb = c as ComboBox;
        if (cb != null)
        {
            int idx = cb.Items.IndexOf(val);
            if (idx < 0) idx = cb.Items.IndexOf(fallback);
            cb.SelectedIndex = idx >= 0 ? idx : 0;
        }
    }

    private static string GetControlValue(Control c)
    {
        TextBox tb = c as TextBox;
        if (tb != null)
        {
            string v = (tb.Text == ListenPrompt) ? (tb.Tag as string ?? "") : tb.Text.Trim();
            return v.Length == 0 ? "NONE" : v;
        }
        ComboBox cb = c as ComboBox;
        if (cb != null)
            return cb.SelectedItem != null ? cb.SelectedItem.ToString() : "NONE";
        return "NONE";
    }

    private void LoadValues()
    {
        // Per-scheme binds: classic from the base key, altcam from base + "_altcam".
        foreach (string key in SchemeKeys)
        {
            string defC = Defaults.ContainsKey(key) ? Defaults[key] : "NONE";
            schemeValues[0][key] = config.Get(key, defC);
            string defA = AltDefaults.ContainsKey(key) ? AltDefaults[key] : "NONE";
            schemeValues[1][key] = config.Get(key + "_altcam", defA);
        }

        // Global (scheme-independent) inputs load straight from config.
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            if (SchemeKeys.Contains(kv.Key)) continue;
            string def = Defaults.ContainsKey(kv.Key) ? Defaults[kv.Key] : "";
            SetControlValue(kv.Value, config.Get(kv.Key, def), def);
        }

        activeScheme = 0;
        chkAltCamControls.Checked = false;
        PopulateUiFromScheme(0);

        PopulateControlStyles();

        chkInvertMouseY.Checked = config.Get("invert_mouse_y", "0") == "1";
        chkInvertControllerY.Checked = config.Get("invert_controller_y", "0") == "1";
        chkTpsOtsAim.Checked = config.Get("tps_ots_aim", "1") == "1";
        chkCrosshair.Checked = config.Get("crosshair", "0") == "1";
        chkImmersiveFps.Checked = config.Get("immersive_fps_head_tracking", "0") == "1";
        chk2dControls.Checked = config.Get("control_2d", "0") == "1";
        chkAimAssist.Checked = config.Get("aim_assist", "1") == "1";
        chkButtonSprint.Checked = config.Get("altcam_button_sprint", "0") == "1";
        chkTpsCameraCollision.Checked = config.Get("tps_camera_collision", "1") == "1";
        chkDisableDpad.Checked = config.Get("disable_dpad_movement", "0") == "1";
        numMouseSens.Value = ClampSens(config.Get("mouse_sensitivity", "1.0"));
        numControllerSens.Value = ClampSens(config.Get("controller_sensitivity", "1.0"));
        numFpsFov.Value = ClampFov(config.Get("fps_fov", "71.1"));
        numTpsFov.Value = ClampFov(config.Get("tps_fov", "71.1"));
        // Migration: the aim zoom used to be the on/off "tps_aim_zoom" key. If the
        // slider key isn't there yet, land on the position that matches whatever the
        // old checkbox said, so an existing setting isn't silently lost. The old "on"
        // full zoom is the 100% default; 200% is a new, deeper 2x zoom. Existing
        // configs with tps_aim_zoom_amount=100 keep their original feel unchanged.
        numTpsAimZoom.Value = ClampAimZoom(
            config.Get("tps_aim_zoom_amount", config.Get("tps_aim_zoom", "1") == "1" ? "100" : "0"));

        bool dbg = config.Get("allow_debug_controls", "0") == "1";
        debugYes.Checked = dbg;
        debugNo.Checked = !dbg;
    }

    private void ResetDefaults()
    {
        // Reset BOTH schemes to their defaults, then re-show the active one.
        foreach (string key in SchemeKeys)
        {
            schemeValues[0][key] = Defaults.ContainsKey(key) ? Defaults[key] : "NONE";
            schemeValues[1][key] = AltDefaults.ContainsKey(key) ? AltDefaults[key] : "NONE";
        }
        PopulateUiFromScheme(activeScheme);

        // Global (scheme-independent) inputs.
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            if (SchemeKeys.Contains(kv.Key)) continue;
            string def = Defaults.ContainsKey(kv.Key) ? Defaults[kv.Key] : "";
            SetControlValue(kv.Value, def, def);
        }

        if (cmbControlStyle.Items.Count > 0)
            cmbControlStyle.SelectedIndex = 0;     // Classic
        chkInvertMouseY.Checked = false;
        chkInvertControllerY.Checked = false;
        chkTpsOtsAim.Checked = true;
        chkCrosshair.Checked = false;
        chkImmersiveFps.Checked = false;
        chk2dControls.Checked = false;
        chkAimAssist.Checked = true;
        chkButtonSprint.Checked = false;
        chkTpsCameraCollision.Checked = true;
        chkDisableDpad.Checked = false;
        numMouseSens.Value = 1.0m;
        numControllerSens.Value = 1.0m;
        numFpsFov.Value = 71.1m;
        numTpsFov.Value = 71.1m;
        numTpsAimZoom.Value = 100m;

        debugNo.Checked = true;
        debugYes.Checked = false;
    }

    // A slider line for a sensitivity value: 0.1..4.0 mapped to 1..40 ticks.
    private TrackBar MakeSensSlider(int x, int y, int width)
    {
        var trk = new TrackBar
        {
            Left = x,
            Top = y,
            Width = width,
            Height = 30,
            AutoSize = false,
            Minimum = 1,
            Maximum = 40,
            TickStyle = TickStyle.None,
            SmallChange = 1,
            LargeChange = 5,
            BackColor = Back,
        };
        Controls.Add(trk);
        return trk;
    }

    // Mirror a NumericUpDown and its slider both ways. tickValue = the numeric
    // value of one slider tick (0.1 for the sensitivities, 1 for FOV).
    private void WirePair(NumericUpDown num, TrackBar trk, decimal tickValue)
    {
        num.ValueChanged += (s, e) =>
        {
            if (syncingSens) return;
            syncingSens = true;
            int v = (int)Math.Round(num.Value / tickValue);
            trk.Value = Math.Max(trk.Minimum, Math.Min(trk.Maximum, v));
            syncingSens = false;
        };
        trk.ValueChanged += (s, e) =>
        {
            if (syncingSens) return;
            syncingSens = true;
            num.Value = trk.Value * tickValue;
            syncingSens = false;
        };
        // Seed the slider from the box's initial value.
        trk.Value = Math.Max(trk.Minimum, Math.Min(trk.Maximum, (int)Math.Round(num.Value / tickValue)));
    }

    private void WireSensPair(NumericUpDown num, TrackBar trk)
    {
        WirePair(num, trk, 0.1m);
    }

    // Parse a camera FOV from config, clamped to [55, 110] degrees
    // (one decimal kept so the 71.1 native default round-trips).
    private static decimal ClampFov(string s)
    {
        double v;
        if (!double.TryParse(s, System.Globalization.NumberStyles.Float,
                             System.Globalization.CultureInfo.InvariantCulture, out v))
            v = 71.1;
        if (v < 55.0) v = 55.0;
        if (v > 110.0) v = 110.0;
        return (decimal)(Math.Round(v * 10.0) / 10.0);
    }

    // Parse a 0-100 percentage from config (the TPS/OTS aim zoom amount).
    private static decimal ClampAimZoom(string s)
    {
        double v;
        if (!double.TryParse(s, System.Globalization.NumberStyles.Float,
                             System.Globalization.CultureInfo.InvariantCulture, out v))
            v = 100.0;
        if (v < 0.0) v = 0.0;
        if (v > 200.0) v = 200.0;
        return (decimal)Math.Round(v);
    }

    // Parse a sensitivity string from config, clamp to the launcher's [0.1, 4.0]
    // range, and return it as a NumericUpDown-safe decimal (invariant culture so a
    // "1.5" written by the game is read the same on comma-decimal locales).
    private static decimal ClampSens(string s)
    {
        double v;
        if (!double.TryParse(s, System.Globalization.NumberStyles.Float,
                             System.Globalization.CultureInfo.InvariantCulture, out v))
            v = 1.0;
        if (v < 0.1) v = 0.1;
        if (v > 4.0) v = 4.0;
        return (decimal)v;
    }

    private void SaveValues()
    {
        // Capture the scheme currently shown in the UI before writing both out.
        FlushUiToScheme(activeScheme);

        // Per-scheme binds: classic to the base key, altcam to base + "_altcam".
        foreach (string key in SchemeKeys)
        {
            string vC = schemeValues[0].ContainsKey(key) ? schemeValues[0][key] : "NONE";
            if (vC.Length == 0) vC = "NONE";
            config.Set(key, vC);

            string vA = schemeValues[1].ContainsKey(key) ? schemeValues[1][key] : "NONE";
            if (vA.Length == 0) vA = "NONE";
            config.Set(key + "_altcam", vA);
        }

        // Global (scheme-independent) inputs.
        foreach (KeyValuePair<string, Control> kv in inputs)
        {
            if (SchemeKeys.Contains(kv.Key)) continue;
            config.Set(kv.Key, GetControlValue(kv.Value));
        }

        if (cmbControlStyle.SelectedIndex >= 0 && cmbControlStyle.SelectedIndex < styleIds.Count)
            config.Set("control_style", styleIds[cmbControlStyle.SelectedIndex]);
        config.Set("invert_mouse_y", chkInvertMouseY.Checked ? "1" : "0");
        config.Set("invert_controller_y", chkInvertControllerY.Checked ? "1" : "0");
        config.Set("tps_ots_aim", chkTpsOtsAim.Checked ? "1" : "0");
        config.Set("crosshair", chkCrosshair.Checked ? "1" : "0");
        config.Set("immersive_fps_head_tracking", chkImmersiveFps.Checked ? "1" : "0");
        config.Set("control_2d", chk2dControls.Checked ? "1" : "0");
        config.Set("aim_assist", chkAimAssist.Checked ? "1" : "0");
        config.Set("altcam_button_sprint", chkButtonSprint.Checked ? "1" : "0");
        config.Set("tps_camera_collision", chkTpsCameraCollision.Checked ? "1" : "0");
        config.Set("disable_dpad_movement", chkDisableDpad.Checked ? "1" : "0");
        config.Set("mouse_sensitivity",
            ((double)numMouseSens.Value).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
        config.Set("controller_sensitivity",
            ((double)numControllerSens.Value).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture));
        config.Set("fps_fov",
            ((double)numFpsFov.Value).ToString("0.#", System.Globalization.CultureInfo.InvariantCulture));
        config.Set("tps_fov",
            ((double)numTpsFov.Value).ToString("0.#", System.Globalization.CultureInfo.InvariantCulture));
        config.Set("tps_aim_zoom_amount",
            ((double)numTpsAimZoom.Value).ToString("0", System.Globalization.CultureInfo.InvariantCulture));

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
