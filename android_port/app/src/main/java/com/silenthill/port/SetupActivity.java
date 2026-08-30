package com.silenthill.port;

import android.Manifest;
import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.widget.Button;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * 2026 Silent Hill Android Port - Dark Horror Launcher & Disc Setup
 */
public class SetupActivity extends Activity {

    private static final String TAG = "SilentHill";

    private static final int REQ_PICK_DISC    = 1001;
    private static final int REQ_ALL_FILES    = 1002;
    private static final int REQ_READ_STORAGE = 1003;

    private static final long MIN_DISC_BYTES = 200L * 1024 * 1024;
    private static final long MAX_DISC_BYTES = 1024L * 1024 * 1024;

    private LinearLayout rootLayout;
    private TextView     statusTitle;
    private TextView     statusSubtitle;
    private LinearLayout statusCard;
    private LinearLayout buttonContainer;
    private LinearLayout candidatesContainer;
    private LinearLayout badgeRow;
    private ProgressBar  progressBar;

    private File targetDir;
    private File installedDisc;
    private String installedSerial;
    private static File s_rejectedInstalled;

    // Palette: 2026 Silent Hill Dark Horror
    private static final int COLOR_BG_DARK       = Color.parseColor("#0A0909");
    private static final int COLOR_CARD_BG       = Color.parseColor("#E6120E0E");
    private static final int COLOR_BORDER_RUST   = Color.parseColor("#4A1815");
    private static final int COLOR_CRIMSON_DARK  = Color.parseColor("#66110B");
    private static final int COLOR_CRIMSON_LIGHT = Color.parseColor("#A82218");
    private static final int COLOR_ACCENT_GLOW   = Color.parseColor("#E74C3C");
    private static final int COLOR_TEXT_WHITE    = Color.parseColor("#F2F4F8");
    private static final int COLOR_TEXT_MUTED    = Color.parseColor("#9EA4AC");
    private static final int COLOR_TEXT_RED      = Color.parseColor("#E55039");

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        requestWindowFeature(Window.FEATURE_NO_TITLE);

        targetDir = resolveTargetDir();
        installedDisc = findInstalledDisc();

        buildHorrorUi();

        if (installedDisc != null) {
            updateDiscReadyState();
            return;
        }

        /* Lay the no-disc buttons down before anything async or fallible. Without
         * them the first run is a status line and no controls at all, which is a
         * dead end for exactly the users this screen exists for. */
        updateNoDiscState();

        if (hasStorageAccess()) {
            startScan();
        } else if (s_rejectedInstalled != null) {
            setStatus("WRONG DISC IMAGE INSTALLED",
                    s_rejectedInstalled.getName() + " is not a Silent Hill disc.\nScan again, or choose the correct .BIN / .CUE.");
        } else {
            setStatus("NO DISC IMAGE DETECTED",
                    "The game needs a PlayStation Silent Hill disc image (.BIN or .CUE).\nUse FIND MY DISC to search your storage, or CHOOSE FILE to browse.");
        }
    }

    private File getConfigFile() {
        File files = getExternalFilesDir(null);
        if (files != null) {
            return new File(files, "config.cfg");
        }
        return new File(targetDir, "config.cfg");
    }

    private String readConfigValue(String key, String defVal) {
        File cfg = getConfigFile();
        if (!cfg.isFile()) return defVal;
        try (BufferedReader br = new BufferedReader(new FileReader(cfg))) {
            String line;
            while ((line = br.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("#") || line.startsWith(";")) continue;
                String[] parts = line.split("=", 2);
                if (parts.length == 2 && parts[0].trim().equals(key)) {
                    return parts[1].trim();
                }
            }
        } catch (Exception ignored) {}
        return defVal;
    }

    /** Drops a key from config.cfg entirely. Used to clear "camera_style", which
     *  earlier builds of this launcher wrote and no version of the engine has
     *  ever read -- left in place it reads like a live setting. */
    private void removeConfigKey(String key) {
        File cfg = getConfigFile();
        if (!cfg.isFile()) return;
        List<String> lines = new ArrayList<String>();
        boolean dropped = false;
        try (BufferedReader br = new BufferedReader(new FileReader(cfg))) {
            String line;
            while ((line = br.readLine()) != null) {
                String trimmed = line.trim();
                if (!trimmed.startsWith("#") && !trimmed.startsWith(";")) {
                    String[] parts = trimmed.split("=", 2);
                    if (parts.length == 2 && parts[0].trim().equals(key)) {
                        dropped = true;
                        continue;
                    }
                }
                lines.add(line);
            }
        } catch (Exception ignored) { return; }
        if (!dropped) return;
        try (BufferedWriter bw = new BufferedWriter(new FileWriter(cfg))) {
            for (String l : lines) {
                bw.write(l);
                bw.newLine();
            }
        } catch (Exception ignored) {}
    }

    private void writeConfigValue(String key, String val) {
        File cfg = getConfigFile();
        List<String> lines = new ArrayList<String>();
        boolean found = false;
        if (cfg.isFile()) {
            try (BufferedReader br = new BufferedReader(new FileReader(cfg))) {
                String line;
                while ((line = br.readLine()) != null) {
                    String trimmed = line.trim();
                    if (!trimmed.startsWith("#") && !trimmed.startsWith(";")) {
                        String[] parts = trimmed.split("=", 2);
                        if (parts.length == 2 && parts[0].trim().equals(key)) {
                            lines.add(key + " = " + val);
                            found = true;
                            continue;
                        }
                    }
                    lines.add(line);
                }
            } catch (Exception ignored) {}
        }
        if (!found) {
            lines.add(key + " = " + val);
        }
        try (BufferedWriter bw = new BufferedWriter(new FileWriter(cfg))) {
            for (String l : lines) {
                bw.write(l);
                bw.newLine();
            }
        } catch (Exception ignored) {}
    }

    // -------------------------------------------------------- UI Construction ---

    private void buildHorrorUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackground(createVignette());

        rootLayout = new LinearLayout(this);
        rootLayout.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(24);
        rootLayout.setPadding(pad, dp(28), pad, dp(28));

        // 1. Header Banner
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.VERTICAL);
        header.setGravity(Gravity.START);

        TextView title = new TextView(this);
        title.setText("SILENT HILL");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        title.setTextColor(COLOR_TEXT_WHITE);
        title.setTypeface(Typeface.create("sans-serif-black", Typeface.BOLD));
        title.setLetterSpacing(0.08f);
        title.setShadowLayer(16, 0, 0, Color.parseColor("#B3E74C3C"));
        header.addView(title);

        TextView subtitle = new TextView(this);
        subtitle.setText("ENHANCED RECOMPILATION • 2026 ANDROID EDITION");
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        subtitle.setTextColor(COLOR_TEXT_RED);
        subtitle.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        subtitle.setLetterSpacing(0.18f);
        subtitle.setPadding(0, dp(2), 0, dp(16));
        header.addView(subtitle);

        // Divider
        View div = new View(this);
        GradientDrawable divGrad = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { COLOR_CRIMSON_LIGHT, COLOR_BORDER_RUST, Color.TRANSPARENT }
        );
        div.setBackground(divGrad);
        div.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(2)));
        header.addView(div);

        rootLayout.addView(header);

        // 2. Status & Disc Card
        statusCard = new LinearLayout(this);
        statusCard.setOrientation(LinearLayout.VERTICAL);
        statusCard.setPadding(dp(18), dp(16), dp(18), dp(16));
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        cardLp.setMargins(0, dp(20), 0, dp(16));
        statusCard.setLayoutParams(cardLp);
        statusCard.setBackground(createCardDrawable(COLOR_CARD_BG, COLOR_BORDER_RUST, dp(14)));

        statusTitle = new TextView(this);
        statusTitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        statusTitle.setTextColor(COLOR_TEXT_WHITE);
        statusTitle.setTypeface(Typeface.create("sans-serif-bold", Typeface.BOLD));
        statusCard.addView(statusTitle);

        statusSubtitle = new TextView(this);
        statusSubtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        statusSubtitle.setTextColor(COLOR_TEXT_MUTED);
        statusSubtitle.setPadding(0, dp(6), 0, 0);
        statusSubtitle.setLineSpacing(dp(3), 1.0f);
        statusCard.addView(statusSubtitle);

        badgeRow = new LinearLayout(this);
        badgeRow.setOrientation(LinearLayout.HORIZONTAL);
        badgeRow.setPadding(0, dp(10), 0, 0);
        badgeRow.setVisibility(View.GONE);
        statusCard.addView(badgeRow);

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setVisibility(View.GONE);
        progressBar.setMax(100);
        progressBar.setPadding(0, dp(12), 0, 0);
        statusCard.addView(progressBar);

        rootLayout.addView(statusCard);

        // 3. Action Buttons Container
        buttonContainer = new LinearLayout(this);
        buttonContainer.setOrientation(LinearLayout.VERTICAL);
        rootLayout.addView(buttonContainer);

        // 4. Candidates Container
        candidatesContainer = new LinearLayout(this);
        candidatesContainer.setOrientation(LinearLayout.VERTICAL);
        candidatesContainer.setPadding(0, dp(12), 0, 0);
        rootLayout.addView(candidatesContainer);

        scroll.addView(rootLayout);
        setContentView(scroll);
    }

    private void updateDiscReadyState() {
        if (installedDisc == null) return;
        long mb = installedDisc.length() / (1024 * 1024);
        setStatus("DISC READY: " + installedDisc.getName().toUpperCase(),
                mb + " MB\n" + installedDisc.getAbsolutePath());

        /* The serial is whichever one the header actually matched. The old
         * hard-coded "SLUS-007.07" labelled every European and Japanese rip as
         * the US release. */
        String serial = (installedSerial != null) ? installedSerial.replace('_', '-') : "UNKNOWN SERIAL";
        showBadges(serial, regionOf(installedSerial));

        buttonContainer.removeAllViews();

        // Hero Play Button
        View playBtn = createHeroButton("ENTER SILENT HILL", "LAUNCH GAME • RESUME NIGHTMARE", new View.OnClickListener() {
            @Override public void onClick(View v) { launchGame(); }
        });
        buttonContainer.addView(playBtn);

        // Settings Button
        View settingsBtn = createActionCard("⚙   GAME & DISPLAY SETTINGS", "Camera style, touch HUD, orientation, flashlight, frame pacing", new View.OnClickListener() {
            @Override public void onClick(View v) { showModernSettingsModal(); }
        });
        buttonContainer.addView(settingsBtn);

        // Disc Vault Button
        View swapBtn = createActionCard("💿   DISC VAULT / SWAP DISC", "Rescan storage or load another .BIN / .CUE image", new View.OnClickListener() {
            @Override public void onClick(View v) { onScanClicked(); }
        });
        buttonContainer.addView(swapBtn);
    }

    private void updateNoDiscState() {
        showBadges();
        buttonContainer.removeAllViews();

        View scanBtn = createHeroButton("FIND MY DISC", "AUTO-SCAN DEVICE STORAGE FOR SILENT HILL .BIN / .CUE", new View.OnClickListener() {
            @Override public void onClick(View v) { onScanClicked(); }
        });
        buttonContainer.addView(scanBtn);

        View pickBtn = createActionCard("📁   CHOOSE FILE...", "Select .BIN or .CUE image manually via file picker", new View.OnClickListener() {
            @Override public void onClick(View v) { onPickClicked(); }
        });
        buttonContainer.addView(pickBtn);

        View settingsBtn = createActionCard("⚙   SETTINGS PRE-CONFIGURATION", "Configure controls, orientation and display options", new View.OnClickListener() {
            @Override public void onClick(View v) { showModernSettingsModal(); }
        });
        buttonContainer.addView(settingsBtn);
    }

    private void setStatus(final String title, final String subtitle) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (statusTitle != null) statusTitle.setText(title);
                if (statusSubtitle != null) statusSubtitle.setText(subtitle);
            }
        });
    }

    private void setBusy(final boolean busy) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (progressBar != null) progressBar.setVisibility(busy ? View.VISIBLE : View.GONE);
                if (buttonContainer != null) {
                    for (int i = 0; i < buttonContainer.getChildCount(); i++) {
                        buttonContainer.getChildAt(i).setEnabled(!busy);
                    }
                }
            }
        });
    }

    private void setCopyProgress(final int pct) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (progressBar != null) progressBar.setProgress(pct);
            }
        });
    }

    // --------------------------------------------------- Custom Dark Horror Views ---

    private View createHeroButton(String titleText, String subText, View.OnClickListener listener) {
        LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.VERTICAL);
        btn.setGravity(Gravity.CENTER_VERTICAL);
        btn.setPadding(dp(22), dp(16), dp(22), dp(16));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(14));
        btn.setLayoutParams(lp);

        GradientDrawable normalGrad = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { COLOR_CRIMSON_LIGHT, COLOR_CRIMSON_DARK }
        );
        normalGrad.setCornerRadius(dp(14));
        normalGrad.setStroke(dp(2), COLOR_ACCENT_GLOW);

        GradientDrawable pressedGrad = new GradientDrawable(
                GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { Color.parseColor("#C0392B"), Color.parseColor("#781812") }
        );
        pressedGrad.setCornerRadius(dp(14));
        pressedGrad.setStroke(dp(2), Color.WHITE);

        StateListDrawable sld = new StateListDrawable();
        sld.addState(new int[] { android.R.attr.state_pressed }, pressedGrad);
        sld.addState(new int[] {}, normalGrad);

        btn.setBackground(sld);
        btn.setClickable(true);
        btn.setFocusable(true);
        btn.setOnClickListener(listener);

        TextView t = new TextView(this);
        t.setText(titleText);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17);
        t.setTextColor(COLOR_TEXT_WHITE);
        t.setTypeface(Typeface.create("sans-serif-black", Typeface.BOLD));
        t.setLetterSpacing(0.06f);
        btn.addView(t);

        TextView st = new TextView(this);
        st.setText(subText);
        st.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        st.setTextColor(Color.parseColor("#FADBD8"));
        st.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        st.setPadding(0, dp(3), 0, 0);
        st.setLetterSpacing(0.04f);
        btn.addView(st);

        return btn;
    }

    private View createActionCard(String titleText, String subText, View.OnClickListener listener) {
        LinearLayout btn = new LinearLayout(this);
        btn.setOrientation(LinearLayout.VERTICAL);
        btn.setGravity(Gravity.CENTER_VERTICAL);
        btn.setPadding(dp(20), dp(14), dp(20), dp(14));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, 0, 0, dp(12));
        btn.setLayoutParams(lp);

        GradientDrawable normalBg = createCardDrawable(Color.parseColor("#D9181414"), COLOR_BORDER_RUST, dp(12));
        GradientDrawable pressedBg = createCardDrawable(Color.parseColor("#F22E1A18"), COLOR_ACCENT_GLOW, dp(12));

        StateListDrawable sld = new StateListDrawable();
        sld.addState(new int[] { android.R.attr.state_pressed }, pressedBg);
        sld.addState(new int[] {}, normalBg);

        btn.setBackground(sld);
        btn.setClickable(true);
        btn.setFocusable(true);
        btn.setOnClickListener(listener);

        TextView t = new TextView(this);
        t.setText(titleText);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        t.setTextColor(COLOR_TEXT_WHITE);
        t.setTypeface(Typeface.create("sans-serif-bold", Typeface.BOLD));
        t.setLetterSpacing(0.04f);
        btn.addView(t);

        TextView st = new TextView(this);
        st.setText(subText);
        st.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        st.setTextColor(COLOR_TEXT_MUTED);
        st.setPadding(0, dp(3), 0, 0);
        btn.addView(st);

        return btn;
    }

    /** Ash-and-fog radial fall-off, lit slightly above centre so the header sits
     *  in the light and the corners rot away. */
    private GradientDrawable createVignette() {
        GradientDrawable vig = new GradientDrawable();
        vig.setGradientType(GradientDrawable.RADIAL_GRADIENT);
        vig.setGradientCenter(0.5f, 0.30f);
        vig.setColors(new int[] {
                Color.parseColor("#181212"),
                Color.parseColor("#0C0A0A"),
                COLOR_BG_DARK,
        });
        int w = getResources().getDisplayMetrics().widthPixels;
        int h = getResources().getDisplayMetrics().heightPixels;
        vig.setGradientRadius(Math.max(w, h) * 0.85f);
        return vig;
    }

    private GradientDrawable createCardDrawable(int bgColor, int strokeColor, int radius) {
        GradientDrawable gd = new GradientDrawable();
        gd.setColor(bgColor);
        gd.setCornerRadius(radius);
        if (strokeColor != 0) {
            gd.setStroke(dp(1), strokeColor);
        }
        return gd;
    }

    private int dp(int val) {
        return (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, val, getResources().getDisplayMetrics());
    }

    // --------------------------------------------- 2026 Modern Settings Modal ---

    private void showModernSettingsModal() {
        final Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        }

        ScrollView scroll = new ScrollView(this);
        scroll.setBackground(createCardDrawable(Color.parseColor("#F8100C0C"), COLOR_BORDER_RUST, dp(20)));
        scroll.setPadding(dp(24), dp(22), dp(24), dp(22));

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);

        // Header
        TextView title = new TextView(this);
        title.setText("SYSTEM & GAMEPLAY SETTINGS");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        title.setTextColor(COLOR_TEXT_WHITE);
        title.setTypeface(Typeface.create("sans-serif-black", Typeface.BOLD));
        title.setLetterSpacing(0.06f);
        content.addView(title);

        TextView sub = new TextView(this);
        sub.setText("Saved to config.cfg  •  In game, press L3 or tap the ☰ overlay button for Quick Options");
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        sub.setTextColor(COLOR_TEXT_MUTED);
        sub.setPadding(0, dp(2), 0, dp(16));
        content.addView(sub);

        // 1. Screen Orientation
        final int[] curOrient = { 0 };
        try { curOrient[0] = Integer.parseInt(readConfigValue("screen_orientation", "0")); } catch (Exception ignored) {}
        addSegmentedOptionRow(content, "SCREEN ORIENTATION",
                new String[] { "AUTO (SENSOR)", "LANDSCAPE LOCK", "PORTRAIT LOCK" },
                curOrient[0], new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curOrient[0] = idx; }
                });

        // 2. Touch Controls Overlay
        final int[] curTouch = { 2 };
        String ct = readConfigValue("touch_controls", "2");
        if (ct.equals("1")) curTouch[0] = 1;
        else if (ct.equals("0")) curTouch[0] = 2;
        else curTouch[0] = 0;
        addSegmentedOptionRow(content, "TOUCH CONTROLS HUD",
                new String[] { "ALWAYS VISIBLE", "AUTO-HIDE (GAMEPAD)", "DISABLED" },
                curTouch[0], new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curTouch[0] = idx; }
                });

        /* 3. Camera style. The engine key is control_style and it takes a style
         *    id STRING, not an index -- "camera_style" was never read by
         *    anything, so this row used to do nothing at all. */
        final String[] STYLE_IDS = { "classic", "tps", "ots", "fps" };
        final int[] curCam = { 0 };
        String cs = readConfigValue("control_style", "classic").toLowerCase();
        for (int i = 0; i < STYLE_IDS.length; i++) {
            if (STYLE_IDS[i].equals(cs)) { curCam[0] = i; break; }
        }
        addSegmentedOptionRow(content, "CAMERA / CONTROL STYLE",
                new String[] { "CLASSIC TANK", "THIRD PERSON", "OVER-THE-SHOULDER", "FIRST PERSON" },
                curCam[0], new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curCam[0] = idx; }
                });

        /* 4. Frame pacing. fps_cap parses into the config struct and is then read
         *    by no limiter anywhere in pc_port or PsyCross, so vsync is the only
         *    setting that actually changes the frame rate. */
        final int[] curVsync = { 0 };
        try { curVsync[0] = (Integer.parseInt(readConfigValue("vsync", "1")) != 0) ? 0 : 1; } catch (Exception ignored) {}
        addSegmentedOptionRow(content, "FRAME PACING",
                new String[] { "VSYNC ON (MATCH DISPLAY)", "VSYNC OFF (UNCAPPED)" },
                curVsync[0], new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curVsync[0] = idx; }
                });

        // 5. Flashlight Lighting
        final int[] curFl = { 0 };
        try { curFl[0] = Integer.parseInt(readConfigValue("flashlight_mode", "2")); } catch (Exception ignored) {}
        addSegmentedOptionRow(content, "FLASHLIGHT LIGHTING",
                new String[] { "DYNAMIC SPOTLIGHT", "CLASSIC PSX", "DYNAMIC SHADOWS" },
                curFl[0] == 2 ? 0 : (curFl[0] == 0 ? 1 : 2), new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curFl[0] = (idx == 0 ? 2 : (idx == 1 ? 0 : 3)); }
                });

        // 6. Skip Intros
        final int[] curIntro = { 0 };
        try { curIntro[0] = Integer.parseInt(readConfigValue("skip_intros", "0")); } catch (Exception ignored) {}
        addSegmentedOptionRow(content, "BOOT LOGOS & INTROS",
                new String[] { "SHOW ALL LOGOS", "SKIP LOGOS", "DIRECT TO GAME" },
                curIntro[0], new OnOptionSelected() {
                    @Override public void onSelected(int idx) { curIntro[0] = idx; }
                });

        // Save Button
        View saveBtn = createHeroButton("APPLY & SAVE SETTINGS", "WRITE CONFIGURATION AND CLOSE", new View.OnClickListener() {
            @Override public void onClick(View v) {
                writeConfigValue("screen_orientation", String.valueOf(curOrient[0]));

                int touchVal = (curTouch[0] == 0) ? 2 : (curTouch[0] == 1 ? 1 : 0);
                writeConfigValue("touch_controls", String.valueOf(touchVal));

                writeConfigValue("control_style", STYLE_IDS[curCam[0]]);
                removeConfigKey("camera_style");

                writeConfigValue("vsync", (curVsync[0] == 0) ? "1" : "0");

                writeConfigValue("flashlight_mode", String.valueOf(curFl[0]));
                writeConfigValue("skip_intros", String.valueOf(curIntro[0]));

                dialog.dismiss();
                setStatus("SETTINGS APPLIED & SAVED", "Configuration written to config.cfg.\nReady to enter Silent Hill.");
            }
        });
        saveBtn.setPadding(dp(16), dp(12), dp(16), dp(12));
        content.addView(saveBtn);

        scroll.addView(content);
        dialog.setContentView(scroll);
        dialog.show();

        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(
                    (int) (getResources().getDisplayMetrics().widthPixels * 0.90),
                    (int) (getResources().getDisplayMetrics().heightPixels * 0.90)
            );
        }
    }

    private interface OnOptionSelected {
        void onSelected(int idx);
    }

    private void addSegmentedOptionRow(LinearLayout parent, String rowTitle, String[] options, int selectedIdx, final OnOptionSelected listener) {
        TextView label = new TextView(this);
        label.setText(rowTitle);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        label.setTextColor(COLOR_TEXT_RED);
        label.setTypeface(Typeface.create("sans-serif-bold", Typeface.BOLD));
        label.setLetterSpacing(0.08f);
        label.setPadding(0, dp(10), 0, dp(6));
        parent.addView(label);

        HorizontalScrollView hScroll = new HorizontalScrollView(this);
        hScroll.setHorizontalScrollBarEnabled(false);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);

        final List<Button> chipButtons = new ArrayList<Button>();

        for (int i = 0; i < options.length; i++) {
            final int optIdx = i;
            final Button chip = new Button(this);
            chip.setText(options[i]);
            chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
            chip.setTypeface(Typeface.create("sans-serif-bold", Typeface.NORMAL));
            chip.setAllCaps(false);
            chip.setPadding(dp(14), dp(8), dp(14), dp(8));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMargins(0, 0, dp(8), dp(12));
            chip.setLayoutParams(lp);

            updateChipStyle(chip, optIdx == selectedIdx);

            chip.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) {
                    for (int j = 0; j < chipButtons.size(); j++) {
                        updateChipStyle(chipButtons.get(j), j == optIdx);
                    }
                    listener.onSelected(optIdx);
                }
            });

            chipButtons.add(chip);
            row.addView(chip);
        }

        hScroll.addView(row);
        parent.addView(hScroll);
    }

    private void updateChipStyle(Button chip, boolean isSelected) {
        if (isSelected) {
            GradientDrawable gd = new GradientDrawable();
            gd.setColor(COLOR_CRIMSON_DARK);
            gd.setCornerRadius(dp(8));
            gd.setStroke(dp(1), COLOR_ACCENT_GLOW);
            chip.setBackground(gd);
            chip.setTextColor(COLOR_TEXT_WHITE);
        } else {
            GradientDrawable gd = new GradientDrawable();
            gd.setColor(Color.parseColor("#1C1717"));
            gd.setCornerRadius(dp(8));
            gd.setStroke(dp(1), Color.parseColor("#382422"));
            chip.setBackground(gd);
            chip.setTextColor(COLOR_TEXT_MUTED);
        }
    }

    // -------------------------------------------------------- Permissions ---

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            setStatus("STORAGE PERMISSION REQUIRED",
                    "Android requires all-files access to discover .BIN and .CUE disc images.\nSwitch on 'Allow access to manage all files' on the next screen.");
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivityForResult(i, REQ_ALL_FILES);
            } catch (Exception e) {
                try {
                    startActivityForResult(
                            new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                            REQ_ALL_FILES);
                } catch (Exception e2) {
                    setStatus("PERMISSION PROMPT FAILED", "Use 'Choose File...' to select your disc directly.");
                }
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                    REQ_READ_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_READ_STORAGE) {
            if (hasStorageAccess()) {
                startScan();
            } else {
                setStatus("PERMISSION NOT GRANTED", "Use 'Choose File...' to select your disc directly.");
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        if (requestCode == REQ_ALL_FILES) {
            if (hasStorageAccess()) {
                startScan();
            } else {
                setStatus("PERMISSION NOT GRANTED", "Use 'Choose File...' to select your disc image.");
            }
            return;
        }

        if (requestCode == REQ_PICK_DISC && resultCode == RESULT_OK && data != null && data.getData() != null) {
            processPickedUri(data.getData());
        }
    }

    // ------------------------------------------------ Disc Picking & Scanning ---

    private void onScanClicked() {
        if (!hasStorageAccess()) {
            showPermissionExplainer();
            return;
        }
        startScan();
    }

    /** Android drops you on a bare system toggle with no idea why an app wants
     *  every file on the device. Say it first, in the launcher's own voice, and
     *  offer the picker as a way to never grant it at all. */
    private void showPermissionExplainer() {
        final Dialog dialog = new Dialog(this);
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        }

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackground(createCardDrawable(Color.parseColor("#F8100C0C"), COLOR_BORDER_RUST, dp(20)));
        content.setPadding(dp(24), dp(22), dp(24), dp(22));

        TextView title = new TextView(this);
        title.setText("STORAGE ACCESS NEEDED");
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        title.setTextColor(COLOR_TEXT_WHITE);
        title.setTypeface(Typeface.create("sans-serif-black", Typeface.BOLD));
        title.setLetterSpacing(0.06f);
        content.addView(title);

        TextView body = new TextView(this);
        body.setText(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                ? "To find your Silent Hill disc image, the launcher reads the headers of .BIN and .CUE files in Download, Documents, ROMs and Games.\n\nAndroid gates that behind 'Allow access to manage all files'. Nothing is uploaded and nothing outside those folders is touched.\n\nPrefer not to grant it? Use CHOOSE FILE and pick the disc yourself."
                : "To find your Silent Hill disc image, the launcher needs permission to read files in your storage.\n\nNothing is uploaded. Prefer not to grant it? Use CHOOSE FILE and pick the disc yourself.");
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        body.setTextColor(COLOR_TEXT_MUTED);
        body.setLineSpacing(dp(3), 1.0f);
        body.setPadding(0, dp(12), 0, dp(18));
        content.addView(body);

        content.addView(createHeroButton("GRANT STORAGE ACCESS", "OPENS THE ANDROID PERMISSION SCREEN",
                new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        dialog.dismiss();
                        requestStorageAccess();
                    }
                }));

        content.addView(createActionCard("📁   CHOOSE FILE INSTEAD", "Pick your .BIN or .CUE without granting anything",
                new View.OnClickListener() {
                    @Override public void onClick(View v) {
                        dialog.dismiss();
                        onPickClicked();
                    }
                }));

        dialog.setContentView(content);
        dialog.show();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(
                    (int) (getResources().getDisplayMetrics().widthPixels * 0.90),
                    ViewGroup.LayoutParams.WRAP_CONTENT);
        }
    }

    private void onPickClicked() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_PICK_DISC);
        } catch (Exception e) {
            setStatus("NO FILE PICKER AVAILABLE", "Could not open document picker on this device.");
        }
    }

    private void processPickedUri(final Uri uri) {
        setBusy(true);
        String name = getFileNameFromUri(uri);

        if (name.toLowerCase().endsWith(".cue")) {
            processPickedCue(uri, name);
            return;
        }

        copyFromUri(uri, name);
    }

    /** A cue sheet holds no track data, so picking one has to reach the .BIN it
     *  names. Try the real file first (possible with all-files access), then the
     *  sibling document URI, and only then fall back to telling the player what
     *  to pick. */
    private void processPickedCue(final Uri uri, final String cueName) {
        final String binRef = readCueBinRef(uri);
        if (binRef == null) {
            setBusy(false);
            restoreButtons();
            setStatus("CUE SHEET UNREADABLE (" + cueName + ")",
                    "No FILE \"...\" BINARY line found in this cue sheet.\nSelect the .BIN itself with CHOOSE FILE.");
            return;
        }

        File sibling = resolveCueSiblingFile(uri, binRef);
        if (sibling != null && sibling.isFile()) {
            if (discSerial(sibling) == null) {
                setBusy(false);
                restoreButtons();
                setStatus("NOT A SILENT HILL DISC",
                        binRef + " sits next to this cue sheet but carries no Silent Hill serial.");
                return;
            }
            final File src = sibling;
            new Thread(new Runnable() {
                @Override public void run() { moveIntoPlace(src); }
            }).start();
            return;
        }

        Uri siblingUri = resolveCueSiblingUri(uri, binRef);
        if (siblingUri != null) {
            copyFromUri(siblingUri, binRef);
            return;
        }

        setBusy(false);
        restoreButtons();
        setStatus("CUE SHEET SELECTED (" + cueName + ")",
                "It points at \"" + binRef + "\", which this picker did not grant access to.\nRun CHOOSE FILE again and select \"" + binRef + "\" itself.");
    }

    /** The track filename from the cue sheet's FILE "..." BINARY line. */
    private String readCueBinRef(Uri uri) {
        InputStream in = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            BufferedReader reader = new BufferedReader(new InputStreamReader(in));
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.toUpperCase().startsWith("FILE ") && line.toUpperCase().contains("BINARY")) {
                    int q1 = line.indexOf('"');
                    int q2 = line.lastIndexOf('"');
                    if (q1 != -1 && q2 > q1) return line.substring(q1 + 1, q2);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "failed reading cue: " + e.getMessage());
        } finally {
            if (in != null) try { in.close(); } catch (Exception ignored) { }
        }
        return null;
    }

    /** ExternalStorageProvider document ids are "primary:relative/path", so with
     *  all-files access the cue sheet's directory can be reached on disk and the
     *  .BIN taken directly -- no 700 MB copy through a content stream. */
    private File resolveCueSiblingFile(Uri cueUri, String binName) {
        if (!hasStorageAccess()) return null;
        try {
            if (!DocumentsContract.isDocumentUri(this, cueUri)) return null;
            String docId = DocumentsContract.getDocumentId(cueUri);
            int colon = docId.indexOf(':');
            if (colon < 0) return null;
            String rel = docId.substring(colon + 1);
            File cue = new File(Environment.getExternalStorageDirectory(), rel);
            File dir = cue.getParentFile();
            if (dir == null) return null;
            return new File(dir, binName);
        } catch (Exception e) {
            return null;
        }
    }

    /** Same directory, expressed as a document URI. Only works where the provider
     *  grants the sibling too; a SecurityException here just means "fall back". */
    private Uri resolveCueSiblingUri(Uri cueUri, String binName) {
        InputStream probe = null;
        try {
            if (!DocumentsContract.isDocumentUri(this, cueUri)) return null;
            String docId = DocumentsContract.getDocumentId(cueUri);
            int slash = docId.lastIndexOf('/');
            if (slash < 0) return null;
            String siblingId = docId.substring(0, slash + 1) + binName;
            Uri sibling = DocumentsContract.buildDocumentUri(cueUri.getAuthority(), siblingId);
            probe = getContentResolver().openInputStream(sibling);
            return (probe != null) ? sibling : null;
        } catch (Exception e) {
            return null;
        } finally {
            if (probe != null) try { probe.close(); } catch (Exception ignored) { }
        }
    }

    private String getFileNameFromUri(Uri uri) {
        String result = null;
        if ("content".equals(uri.getScheme())) {
            try (android.database.Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (idx >= 0) result = cursor.getString(idx);
                }
            } catch (Exception ignored) {}
        }
        if (result == null) {
            result = uri.getLastPathSegment();
        }
        return result != null ? result : "disc.bin";
    }

    private void copyFromUri(final Uri uri, final String fileName) {
        setStatus("IMPORTING DISC IMAGE", "Streaming " + fileName + " into game storage...");

        new Thread(new Runnable() {
            @Override public void run() {
                File dest = new File(targetDir, "disc.bin");
                try {
                    InputStream in = getContentResolver().openInputStream(uri);
                    if (in == null) {
                        fail("Could not open the selected file stream.");
                        return;
                    }
                    long size = 0;
                    try {
                        android.content.res.AssetFileDescriptor afd = getContentResolver().openAssetFileDescriptor(uri, "r");
                        if (afd != null) { size = afd.getLength(); afd.close(); }
                    } catch (Exception ignored) {}

                    if (copyStream(in, dest, size)) {
                        done(dest);
                    } else {
                        fail("Copying the disc image failed. Check available storage space.");
                    }
                } catch (Exception e) {
                    fail("Could not copy the file: " + e.getMessage());
                }
            }
        }).start();
    }

    private void startScan() {
        setBusy(true);
        setStatus("SCANNING STORAGE", "Searching for Silent Hill (.BIN / .CUE) disc images...");

        new Thread(new Runnable() {
            @Override public void run() {
                final List<File> found = scanForDiscs();

                if (found.isEmpty()) {
                    setBusy(false);
                    restoreButtons();
                    setStatus("NO SILENT HILL DISC FOUND",
                            "Looked in Download, Documents, ROMs and Games.\nPut your .BIN or .CUE in one of those and scan again, or use CHOOSE FILE to browse.");
                    return;
                }

                if (found.size() == 1) {
                    moveIntoPlace(found.get(0));
                    return;
                }

                setBusy(false);
                offerChoice(found);
            }
        }).start();
    }

    private void offerChoice(final List<File> found) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                setStatus("MULTIPLE DISCS FOUND (" + found.size() + ")", "Select the Silent Hill disc image to use:");
                candidatesContainer.removeAllViews();

                for (final File f : found) {
                    View item = createActionCard(
                            f.getName().toUpperCase(),
                            (f.length() / (1024 * 1024)) + " MB  •  " + f.getAbsolutePath(),
                            new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    candidatesContainer.removeAllViews();
                                    setBusy(true);
                                    new Thread(new Runnable() {
                                        @Override public void run() { moveIntoPlace(f); }
                                    }).start();
                                }
                            }
                    );
                    candidatesContainer.addView(item);
                }
            }
        });
    }

    private List<File> scanForDiscs() {
        List<File> out  = new ArrayList<File>();
        File       root = Environment.getExternalStorageDirectory();

        List<File> dirs = new ArrayList<File>();
        dirs.add(new File(root, Environment.DIRECTORY_DOWNLOADS));
        dirs.add(new File(root, "Download"));
        dirs.add(new File(root, "Downloads"));
        dirs.add(new File(root, Environment.DIRECTORY_DOCUMENTS));
        dirs.add(new File(root, "ROMs"));
        dirs.add(new File(root, "Games"));
        dirs.add(root);

        Set<String> seen = new HashSet<String>();

        for (File dir : dirs) {
            collectDiscs(dir, out, seen, 2);
        }
        return out;
    }

    private void collectDiscs(File dir, List<File> out, Set<String> seen, int depth) {
        if (dir == null || depth < 0 || !dir.isDirectory()) return;
        if (!seen.add("d:" + realPath(dir))) return;

        File[] entries = dir.listFiles();
        if (entries == null) return;

        for (File f : entries) {
            if (f.isDirectory()) {
                String name = f.getName();
                if (name.equals("Android") || f.equals(targetDir)) continue;
                collectDiscs(f, out, seen, depth - 1);
                continue;
            }

            String lower = f.getName().toLowerCase();

            // Support .CUE sheets pointing to .BIN
            if (lower.endsWith(".cue")) {
                File refBin = parseCueForBin(f);
                if (refBin != null && refBin.isFile() && refBin.length() >= MIN_DISC_BYTES && refBin.length() <= MAX_DISC_BYTES) {
                    if (seen.add("f:" + realPath(refBin)) && isSilentHillDisc(refBin)) {
                        Log.i(TAG, "disc candidate from CUE: " + refBin.getAbsolutePath());
                        out.add(refBin);
                    }
                }
                continue;
            }

            if (!lower.endsWith(".bin") && !lower.endsWith(".img") && !lower.endsWith(".iso")) continue;
            if (f.length() < MIN_DISC_BYTES || f.length() > MAX_DISC_BYTES) continue;
            if (!seen.add("f:" + realPath(f))) continue;
            if (!isSilentHillDisc(f)) continue;

            Log.i(TAG, "disc candidate: " + f.getAbsolutePath() + " (" + f.length() + ")");
            out.add(f);
        }
    }

    private File parseCueForBin(File cueFile) {
        try (BufferedReader reader = new BufferedReader(new FileReader(cueFile))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.toUpperCase().startsWith("FILE ") && line.toUpperCase().contains("BINARY")) {
                    int q1 = line.indexOf('"');
                    int q2 = line.lastIndexOf('"');
                    if (q1 != -1 && q2 > q1) {
                        String binName = line.substring(q1 + 1, q2);
                        File candidate = new File(cueFile.getParentFile(), binName);
                        if (candidate.isFile()) return candidate;
                    }
                }
            }
        } catch (Exception ignored) {}
        return null;
    }

    private static String realPath(File f) {
        try {
            return f.getCanonicalPath();
        } catch (IOException e) {
            return f.getAbsolutePath();
        }
    }

    private static final String[] SH_SERIALS = {
            "SLUS_007.07",  // USA
            "SLES_015.14",  // Europe
            "SLPM_861.92",  // Japan
            "SLPM_872.70",  // Japan, Konami The Best
            "SLES_025.38",  // Europe, budget re-release
    };

    private boolean isSilentHillDisc(File f) {
        return discSerial(f) != null;
    }

    /** The SLUS/SLES/SLPM serial in the disc header, or null if it is not a
     *  Silent Hill disc. Callers needing only a yes/no use isSilentHillDisc. */
    private String discSerial(File f) {
        RandomAccessFile raf = null;
        try {
            raf = new RandomAccessFile(f, "r");
            byte[] buf = new byte[2 * 1024 * 1024];
            int n = raf.read(buf);
            if (n <= 0) return null;

            String head = new String(buf, 0, n, "ISO-8859-1");
            for (String serial : SH_SERIALS) {
                if (head.contains(serial)) {
                    Log.i(TAG, "identified " + serial + " in " + f.getName());
                    return serial;
                }
            }
            return null;
        } catch (Exception e) {
            return null;
        } finally {
            if (raf != null) try { raf.close(); } catch (IOException ignored) { }
        }
    }

    private static String regionOf(String serial) {
        if (serial == null)            return "UNVERIFIED";
        if (serial.startsWith("SLUS")) return "NTSC-U • VERIFIED";
        if (serial.startsWith("SLES")) return "PAL • VERIFIED";
        if (serial.startsWith("SLPM")) return "NTSC-J • VERIFIED";
        return "VERIFIED";
    }

    /** Pill chips under the status text. Called with no arguments, hides the row. */
    private void showBadges(final String... labels) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (badgeRow == null) return;
                badgeRow.removeAllViews();
                if (labels.length == 0) {
                    badgeRow.setVisibility(View.GONE);
                    return;
                }
                for (String label : labels) {
                    TextView chip = new TextView(SetupActivity.this);
                    chip.setText(label);
                    chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
                    chip.setTextColor(COLOR_TEXT_RED);
                    chip.setTypeface(Typeface.create("sans-serif-bold", Typeface.BOLD));
                    chip.setLetterSpacing(0.10f);
                    chip.setPadding(dp(10), dp(5), dp(10), dp(5));
                    chip.setBackground(createCardDrawable(Color.parseColor("#26E74C3C"), COLOR_BORDER_RUST, dp(999)));
                    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                    lp.setMargins(0, 0, dp(8), 0);
                    chip.setLayoutParams(lp);
                    badgeRow.addView(chip);
                }
                badgeRow.setVisibility(View.VISIBLE);
            }
        });
    }

    private void moveIntoPlace(final File src) {
        setStatus("IMPORTING DISC", "Preparing " + src.getName() + "...");
        File dest = new File(targetDir, src.getName());

        if (src.renameTo(dest)) {
            Log.i(TAG, "moved disc to " + dest.getAbsolutePath());
            done(dest);
            return;
        }

        try {
            InputStream in = new java.io.FileInputStream(src);
            if (copyStream(in, dest, src.length())) {
                if (src.delete()) Log.i(TAG, "removed source copy " + src.getAbsolutePath());
                done(dest);
            } else {
                fail("Copying the disc image failed. Check available storage space.");
            }
        } catch (Exception e) {
            fail("Could not read the disc image: " + e.getMessage());
        }
    }

    private boolean copyStream(InputStream in, File dest, long expected) {
        OutputStream out = null;
        try {
            out = new FileOutputStream(dest);
            byte[] buf = new byte[1024 * 1024];
            long   total = 0;
            int    n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                total += n;
                if (expected > 0) {
                    setCopyProgress((int) ((total * 100) / expected));
                }
            }
            out.flush();
            return total > 0;
        } catch (Exception e) {
            Log.e(TAG, "copy failed: " + e.getMessage());
            dest.delete();
            return false;
        } finally {
            try { in.close(); } catch (Exception ignored) { }
            if (out != null) try { out.close(); } catch (Exception ignored) { }
        }
    }

    private void done(File dest) {
        setAsideForeignDiscs(dest);
        s_rejectedInstalled = null;
        installedDisc = dest;
        installedSerial = discSerial(dest);
        Log.i(TAG, "disc ready at " + dest.getAbsolutePath());
        setBusy(false);
        runOnUiThread(new Runnable() {
            @Override public void run() {
                updateDiscReadyState();
            }
        });
    }

    private void setAsideForeignDiscs(File keep) {
        File[] entries = targetDir.listFiles();
        if (entries == null) return;

        for (File f : entries) {
            if (!f.isFile() || f.equals(keep)) continue;
            if (!f.getName().toLowerCase().endsWith(".bin")) continue;
            if (isSilentHillDisc(f)) continue;

            File aside = new File(targetDir, f.getName() + ".not-silent-hill");
            if (f.renameTo(aside)) {
                Log.i(TAG, "set aside foreign disc: " + f.getName());
            }
        }
    }

    private void fail(String why) {
        setBusy(false);
        restoreButtons();
        setStatus("DISC SETUP ERROR", why + "\n\nYou can also use CHOOSE FILE to select your rip.");
    }

    /** Puts the action buttons back after a state that cleared or disabled them.
     *  Every failure path has to end here: a status line with no controls under
     *  it is a dead end. */
    private void restoreButtons() {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (installedDisc != null) updateDiscReadyState();
                else                       updateNoDiscState();
            }
        });
    }

    private File resolveTargetDir() {
        File[] mediaDirs = getExternalMediaDirs();
        if (mediaDirs != null && mediaDirs.length > 0 && mediaDirs[0] != null) {
            if (mediaDirs[0].isDirectory() || mediaDirs[0].mkdirs()) {
                return mediaDirs[0];
            }
        }
        File files = getExternalFilesDir(null);
        File data  = new File(files, "gamedata");
        if (!data.isDirectory()) {
            data.mkdirs();
        }
        return data;
    }

    private File findInstalledDisc() {
        List<File> roots = new ArrayList<File>();
        roots.add(targetDir);
        roots.add(new File(targetDir, "gamedata"));

        File files = getExternalFilesDir(null);
        if (files != null) {
            roots.add(new File(files, "gamedata"));
        }

        for (File dir : roots) {
            if (dir == null || !dir.isDirectory()) continue;
            File[] entries = dir.listFiles();
            if (entries == null) continue;
            for (File f : entries) {
                if (f.isFile() && f.getName().toLowerCase().endsWith(".bin")
                        && f.length() >= MIN_DISC_BYTES) {
                    String serial = discSerial(f);
                    if (serial != null) {
                        installedSerial = serial;
                        return f;
                    }
                    Log.w(TAG, "installed .bin is not Silent Hill: " + f.getAbsolutePath());
                    s_rejectedInstalled = f;
                }
            }
        }
        return null;
    }

    private void launchGame() {
        startActivity(new Intent(this, SilentHillActivity.class));
        finish();
    }
}

