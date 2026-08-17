package com.silenthill.port;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;

/**
 * First-run disc setup, and the app's launcher entry point.
 *
 * The game needs a disc image the user supplies themselves, and the folder it
 * has to end up in (Android/media/<package>) is not somewhere most people can
 * navigate to on a phone. A ripped copy re-downloaded from cloud storage lands
 * in Downloads, so this looks there (and in the other usual places) and offers
 * to move it. Moving within the same volume is a rename, so it costs no time
 * and no extra space.
 *
 * The file picker is the fallback and needs no permission at all -- it also
 * reaches cloud providers, which a filesystem scan cannot. That path has to
 * copy rather than move, since a picked document gives no delete rights.
 *
 * When a disc is already in place this finishes immediately and the user never
 * sees it.
 */
public class SetupActivity extends Activity {

    private static final String TAG = "SilentHill";

    private static final int REQ_PICK_DISC   = 1001;
    private static final int REQ_ALL_FILES   = 1002;
    private static final int REQ_READ_STORAGE = 1003;

    /** A PSX disc image is ~500-750 MB; anything far outside that is not one. */
    private static final long MIN_DISC_BYTES = 200L * 1024 * 1024;
    private static final long MAX_DISC_BYTES = 1024L * 1024 * 1024;

    private TextView    statusView;
    private Button      scanButton;
    private Button      pickButton;
    private ProgressBar progress;
    private LinearLayout candidates;

    private File targetDir;
    private static File s_rejectedInstalled;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        targetDir = resolveTargetDir();

        if (findInstalledDisc() != null) {
            launchGame();
            return;
        }

        buildUi();

        // If access was granted on an earlier run, go straight to scanning
        // rather than making the user press a button for no reason.
        if (hasStorageAccess()) {
            startScan();
        } else {
            setStatus("The game needs a Silent Hill disc image (a .bin file).\n\n"
                    + "It is not included -- use a rip of your own disc. If you have "
                    + "already downloaded one, this can find it for you.");
        }
    }

    private String introText() {
        if (s_rejectedInstalled != null) {
            return "The disc image that was installed is not Silent Hill:\n"
                 + s_rejectedInstalled.getName() + "\n\n"
                 + "Pick the right one below, or choose the file yourself. The "
                 + "wrong one will be set aside, not deleted.";
        }
        return "The game needs a Silent Hill disc image (a .bin file).\n\n"
             + "It is not included -- use a rip of your own disc. If you have "
             + "already downloaded one, this can find it for you.";
    }

    private File resolveTargetDir() {
        File[] mediaDirs = getExternalMediaDirs();
        if (mediaDirs != null && mediaDirs.length > 0 && mediaDirs[0] != null) {
            if (mediaDirs[0].isDirectory() || mediaDirs[0].mkdirs()) {
                return mediaDirs[0];
            }
        }

        // Fall back to the app's own files dir, which always exists.
        File files = getExternalFilesDir(null);
        File data  = new File(files, "gamedata");
        if (!data.isDirectory()) {
            data.mkdirs();
        }
        return data;
    }

    // ---------------------------------------------------------------- UI ---

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundColor(Color.BLACK);

        TextView title = new TextView(this);
        title.setText("Silent Hill - Setup");
        title.setTextColor(Color.WHITE);
        title.setTextSize(24);
        title.setPadding(0, 0, 0, 24);
        root.addView(title);

        statusView = new TextView(this);
        statusView.setTextColor(Color.LTGRAY);
        statusView.setTextSize(16);
        root.addView(statusView);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setVisibility(View.GONE);
        progress.setMax(100);
        root.addView(progress);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.START);
        buttons.setPadding(0, 32, 0, 0);

        scanButton = new Button(this);
        scanButton.setText("Find my disc");
        scanButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { onScanClicked(); }
        });
        buttons.addView(scanButton);

        pickButton = new Button(this);
        pickButton.setText("Choose file...");
        pickButton.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { onPickClicked(); }
        });
        buttons.addView(pickButton);

        root.addView(buttons);

        candidates = new LinearLayout(this);
        candidates.setOrientation(LinearLayout.VERTICAL);
        candidates.setPadding(0, 24, 0, 0);
        root.addView(candidates);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    private void setStatus(final String text) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (statusView != null) statusView.setText(text);
            }
        });
    }

    private void setBusy(final boolean busy) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (scanButton != null) scanButton.setEnabled(!busy);
                if (pickButton != null) pickButton.setEnabled(!busy);
                if (progress != null) progress.setVisibility(busy ? View.VISIBLE : View.GONE);
            }
        });
    }

    private void setCopyProgress(final int pct) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                if (progress != null) progress.setProgress(pct);
            }
        });
    }

    // -------------------------------------------------------- permissions ---

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
            // All-files access is the only way to read a .bin (a non-media file)
            // by path on Android 11+. It is a Settings screen, not a dialog.
            setStatus("Android needs you to allow file access so the disc image can "
                    + "be found.\n\nOn the next screen, switch on \"Allow access to "
                    + "manage all files\", then come back.");
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
                    setStatus("This device would not open the file-access setting.\n\n"
                            + "Use \"Choose file...\" instead.");
                }
            }
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestPermissions(new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                               REQ_READ_STORAGE);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        if (requestCode == REQ_READ_STORAGE) {
            if (hasStorageAccess()) {
                startScan();
            } else {
                setStatus("Without file access the disc cannot be searched for.\n\n"
                        + "Use \"Choose file...\" to point at it directly.");
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
                setStatus("File access was not granted.\n\n"
                        + "Use \"Choose file...\" to point at the disc image directly.");
            }
            return;
        }

        if (requestCode == REQ_PICK_DISC && resultCode == RESULT_OK
                && data != null && data.getData() != null) {
            copyFromUri(data.getData());
        }
    }

    // ------------------------------------------------------------ actions ---

    private void onScanClicked() {
        if (!hasStorageAccess()) {
            requestStorageAccess();
            return;
        }
        startScan();
    }

    private void onPickClicked() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_PICK_DISC);
        } catch (Exception e) {
            setStatus("No file picker is available on this device.");
        }
    }

    private void startScan() {
        setBusy(true);
        setStatus("Looking for a disc image...");

        new Thread(new Runnable() {
            @Override public void run() {
                final List<File> found = scanForDiscs();

                if (found.isEmpty()) {
                    setBusy(false);
                    setStatus("No disc image found.\n\n"
                            + "Copy your .bin into the phone's Download folder and press "
                            + "\"Find my disc\" again, or use \"Choose file...\" to point "
                            + "straight at it.");
                    return;
                }

                if (found.size() == 1) {
                    moveIntoPlace(found.get(0));
                    return;
                }

                /* More than one verified disc: ask. Guessing (it used to take
                 * the largest) is how the wrong game ends up installed with no
                 * way to say otherwise. */
                setBusy(false);
                offerChoice(found);
            }
        }).start();
    }

    /** One button per candidate, so the player picks instead of the app guessing. */
    private void offerChoice(final List<File> found) {
        runOnUiThread(new Runnable() {
            @Override public void run() {
                setStatus("Found " + found.size() + " Silent Hill discs. Which one?");
                candidates.removeAllViews();

                for (final File f : found) {
                    Button b = new Button(SetupActivity.this);
                    b.setText(f.getName() + "  (" + (f.length() / (1024 * 1024)) + " MB)");
                    b.setAllCaps(false);
                    b.setOnClickListener(new View.OnClickListener() {
                        @Override public void onClick(View v) {
                            candidates.removeAllViews();
                            setBusy(true);
                            new Thread(new Runnable() {
                                @Override public void run() { moveIntoPlace(f); }
                            }).start();
                        }
                    });
                    candidates.addView(b);
                }
            }
        });
    }

    /**
     * Rename any other .bin already sitting in the target folder so the game
     * cannot pick it. The native disc search accepts any PlayStation boot
     * serial, so leaving a foreign disc beside the real one would let it win on
     * the next launch. Renamed rather than deleted -- it is the user's file.
     */
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

    // ------------------------------------------------------------ scanning ---

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

        // The usual folders overlap (DIRECTORY_DOWNLOADS *is* "Download", and the
        // root walk reaches it again), so dedupe by real path or one file gets
        // reported several times.
        java.util.Set<String> seen = new java.util.HashSet<String>();

        for (File dir : dirs) {
            collectDiscs(dir, out, seen, 2);
        }
        return out;
    }

    private static String realPath(File f) {
        try {
            return f.getCanonicalPath();
        } catch (IOException e) {
            return f.getAbsolutePath();
        }
    }

    /**
     * Depth-limited so a scan cannot walk an entire filesystem: the disc is
     * either in one of the usual folders or one level inside it.
     */
    private void collectDiscs(File dir, List<File> out, java.util.Set<String> seen, int depth) {
        if (dir == null || depth < 0 || !dir.isDirectory()) return;
        if (!seen.add("d:" + realPath(dir))) return;

        File[] entries = dir.listFiles();
        if (entries == null) return;

        for (File f : entries) {
            if (f.isDirectory()) {
                // Never descend into our own target or the Android sandbox.
                String name = f.getName();
                if (name.equals("Android") || f.equals(targetDir)) continue;
                collectDiscs(f, out, seen, depth - 1);
                continue;
            }

            String lower = f.getName().toLowerCase();
            if (!lower.endsWith(".bin")) continue;
            if (f.length() < MIN_DISC_BYTES || f.length() > MAX_DISC_BYTES) continue;
            if (!seen.add("f:" + realPath(f))) continue;
            if (!isSilentHillDisc(f)) continue;

            Log.i(TAG, "disc candidate: " + f.getAbsolutePath() + " (" + f.length() + ")");
            out.add(f);
        }
    }

    /**
     * Silent Hill's boot executable name, as it appears in the disc's ISO
     * directory. Testing only for "is this a PlayStation disc" was not enough:
     * a Resident Evil rip passed that happily, got auto-installed, and left no
     * way to undo it. These identify the game itself.
     *
     * Only the automatic scan is held to this. A file the user picks by hand is
     * accepted whatever its serial, since that is the escape hatch for fan
     * translations, modified images and releases not listed here.
     */
    private static final String[] SH_SERIALS = {
        "SLUS_007.07",  // USA
        "SLES_015.14",  // Europe
        "SLPM_861.92",  // Japan
        "SLPM_872.70",  // Japan, Konami The Best
        "SLES_025.38",  // Europe, budget re-release
    };

    private boolean isSilentHillDisc(File f) {
        RandomAccessFile raf = null;
        try {
            raf = new RandomAccessFile(f, "r");

            /* The ISO descriptors and root directory live near the start, but
             * 2352-byte raw sectors push them further in than a plain ISO would,
             * so read generously rather than assuming an offset. */
            byte[] buf = new byte[2 * 1024 * 1024];
            int n = raf.read(buf);
            if (n <= 0) return false;

            String head = new String(buf, 0, n, "ISO-8859-1");
            for (String serial : SH_SERIALS) {
                if (head.contains(serial)) {
                    Log.i(TAG, "identified " + serial + " in " + f.getName());
                    return true;
                }
            }
            return false;
        } catch (Exception e) {
            return false;
        } finally {
            if (raf != null) try { raf.close(); } catch (IOException ignored) { }
        }
    }

    // ------------------------------------------------------------- install ---

    private void moveIntoPlace(final File src) {
        setStatus("Found:\n" + src.getName() + "\n\nMoving it into place...");

        File dest = new File(targetDir, src.getName());

        // Same volume: a rename is instant and needs no extra free space.
        if (src.renameTo(dest)) {
            Log.i(TAG, "moved disc to " + dest.getAbsolutePath());
            done(dest);
            return;
        }

        setStatus("Found:\n" + src.getName() + "\n\nCopying it into place...");
        try {
            InputStream in = new java.io.FileInputStream(src);
            if (copyStream(in, dest, src.length())) {
                // Best effort: the copy is what matters, reclaiming the space is not.
                if (src.delete()) Log.i(TAG, "removed source copy " + src.getAbsolutePath());
                done(dest);
            } else {
                fail("Copying the disc image failed. Is there enough free space?");
            }
        } catch (Exception e) {
            fail("Could not read the disc image: " + e.getMessage());
        }
    }

    private void copyFromUri(final Uri uri) {
        setBusy(true);
        setStatus("Copying the disc image...");

        new Thread(new Runnable() {
            @Override public void run() {
                File dest = new File(targetDir, "disc.bin");
                try {
                    InputStream in = getContentResolver().openInputStream(uri);
                    if (in == null) {
                        fail("Could not open the selected file.");
                        return;
                    }
                    long size = 0;
                    try {
                        android.content.res.AssetFileDescriptor afd =
                            getContentResolver().openAssetFileDescriptor(uri, "r");
                        if (afd != null) { size = afd.getLength(); afd.close(); }
                    } catch (Exception ignored) { }

                    if (copyStream(in, dest, size)) {
                        done(dest);
                    } else {
                        fail("Copying the disc image failed. Is there enough free space?");
                    }
                } catch (Exception e) {
                    fail("Could not copy the file: " + e.getMessage());
                }
            }
        }).start();
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
        Log.i(TAG, "disc ready at " + dest.getAbsolutePath());
        setBusy(false);
        runOnUiThread(new Runnable() {
            @Override public void run() { launchGame(); }
        });
    }

    private void fail(String why) {
        setBusy(false);
        setStatus(why + "\n\nYou can also use \"Choose file...\".");
    }

    // -------------------------------------------------------------- launch ---

    /** Any .bin already sitting in a place the native searcher looks. */
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
                    /* Verify, don't assume. An installed file that is not
                     * Silent Hill has to drop through to setup, or the app
                     * launches into a disc the game rejects, exits, and does
                     * the same thing again on every future launch. */
                    if (isSilentHillDisc(f)) {
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
