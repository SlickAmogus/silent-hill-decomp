package com.silenthill.port;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.database.Cursor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

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
    private static final int REQ_PICK_FOLDER = 1002;
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

        /* Before anything reads targetDir. Everything downstream -- the disc
         * install, the asset unpack, the game's working directory -- hangs off
         * this one answer.
         *
         * Order matters: an upgrading install has to be recognised BEFORE the
         * chooser runs, or the player is asked a question whose every answer
         * silently abandons the disc image and saves they already have. */
        boolean askAgain = askAgainRequested();

        if (StorageLocations.stored(this) == null
                && StorageLocations.hasGameData(StorageLocations.legacyRoot(this))) {
            offerToMoveExistingData();
            return;
        }

        if (StorageLocations.worthAsking(this) || askAgain) {
            askWhereToKeepData();
            return;
        }

        continueSetup();
    }

    /**
     * An install from before the data location was a choice. Its files are in
     * the app's own folder, which Android 11 stopped letting file managers open
     * -- so the player cannot reach their saves, which is the complaint -- but
     * the game reads them fine, and simply pointing somewhere else would look
     * exactly like losing them.
     *
     * So: offer, never assume. Leaving them put is a legitimate answer and the
     * game keeps working, which is why it is not phrased as a warning.
     */
    private void offerToMoveExistingData() {
        final List<StorageLocations.Option> opts = StorageLocations.candidates(this);
        final File legacy = StorageLocations.legacyRoot(this);

        if (opts.isEmpty()) {
            StorageLocations.store(this, legacy);
            continueSetup();
            return;
        }

        final StorageLocations.Option target = opts.get(0);

        new AlertDialog.Builder(this)
            .setTitle("Move your game data?")
            .setCancelable(false)
            .setMessage(
                "Your disc image and saves are in a folder that Android no longer "
              + "lets file managers open, so you cannot get at your saves.\n\n"
              + "Move them to " + target.label + "?\n" + target.dir.getAbsolutePath()
              + "\n\nThe game works either way.")
            .setPositiveButton("Move", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    migrateThen(legacy, target.dir);
                }
            })
            .setNegativeButton("Leave it", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    StorageLocations.store(SetupActivity.this, legacy);
                    continueSetup();
                }
            })
            .show();
    }

    /**
     * Move an install between volumes, then carry on.
     *
     * renameTo first, per entry: the old folder and the built-in media dir are
     * on the same volume, so that is a directory-entry change and a 700 MB disc
     * image moves instantly. It fails across volumes (to a real SD card), and
     * only then is anything copied.
     *
     * Nothing at the destination is ever overwritten. The two locations could
     * BOTH hold a disc -- setup used to install to one while the game ran from
     * the other -- and the destination's copy is the newer answer.
     */
    private void migrateThen(final File from, final File to) {
        buildUi();
        setBusy(true);
        setStatus("Moving your game data. This can take a while on an SD card.");

        new Thread(new Runnable() {
            @Override public void run() {
                int moved = 0, kept = 0, failed = 0;
                File[] entries = from.listFiles();

                if (entries != null) {
                    for (int i = 0; i < entries.length; i++) {
                        File src = entries[i];
                        File dst = new File(to, src.getName());

                        setCopyProgress((i * 100) / entries.length);

                        if (dst.exists()) {
                            kept++;
                            continue;
                        }
                        if (src.renameTo(dst)) {
                            moved++;
                            continue;
                        }
                        if (copyTree(src, dst)) {
                            deleteTree(src);
                            moved++;
                        } else {
                            failed++;
                            Log.w(TAG, "could not move " + src.getAbsolutePath());
                        }
                    }
                }

                Log.i(TAG, "migrated " + moved + " entries, kept " + kept
                         + " already at the destination, " + failed + " failed");

                /* The new location even after a partial move: what did move is
                 * there, and the alternative is data split across two roots
                 * with the game reading the one the player cannot see. */
                StorageLocations.store(SetupActivity.this, to);

                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        setBusy(false);
                        continueSetup();
                    }
                });
            }
        }).start();
    }

    private boolean copyTree(File src, File dst) {
        if (src.isDirectory()) {
            if (!dst.isDirectory() && !dst.mkdirs()) {
                return false;
            }
            File[] kids = src.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    if (!copyTree(k, new File(dst, k.getName()))) {
                        return false;
                    }
                }
            }
            return true;
        }

        byte[] buf = new byte[64 * 1024];
        try (InputStream in = new java.io.FileInputStream(src);
             OutputStream out = new FileOutputStream(dst)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            return true;
        } catch (IOException e) {
            Log.w(TAG, "copy failed: " + src + " -> " + dst + ": " + e.getMessage());
            dst.delete();
            return false;
        }
    }

    private void deleteTree(File f) {
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) {
                for (File k : kids) {
                    deleteTree(k);
                }
            }
        }
        f.delete();
    }

    /**
     * The in-game option writes this marker into the data root rather than
     * calling back into Java: the game is a separate activity by then, the
     * choice cannot take effect until the next launch anyway, and a file the
     * setup screen already has a path to needs no JNI.
     */
    private boolean askAgainRequested() {
        File flag = new File(StorageLocations.resolve(this), ".ask_storage");
        if (!flag.isFile()) {
            return false;
        }
        flag.delete();
        StorageLocations.clear(this);
        return true;
    }

    /**
     * Removable first, and preselected: a card is the answer for the players who
     * asked for this, and it is the one they can take out and read elsewhere.
     * Not cancellable -- there is no sensible "neither".
     */
    private void askWhereToKeepData() {
        final List<StorageLocations.Option> opts = StorageLocations.candidates(this);

        if (opts.isEmpty()) {
            continueSetup();
            return;
        }

        String[] labels = new String[opts.size()];
        for (int i = 0; i < opts.size(); i++) {
            StorageLocations.Option o = opts.get(i);
            labels[i] = o.label + "\n" + o.dir.getAbsolutePath();
        }

        new AlertDialog.Builder(this)
            .setTitle("Where should the game keep its data?")
            .setCancelable(false)
            .setItems(labels, new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    StorageLocations.store(SetupActivity.this, opts.get(which).dir);
                    continueSetup();
                }
            })
            .show();
    }

    private void continueSetup() {
        targetDir = resolveTargetDir();

        if (findInstalledDisc() != null) {
            launchGame();
            return;
        }

        buildUi();

        // If access was granted on an earlier run, go straight to scanning
        // rather than making the user press a button for no reason.
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R && hasStorageAccess()) {
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
        return StorageLocations.resolve(this);
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

    /** Path access for the Android 10-and-older scan. */
    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    private void requestStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
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

        if (requestCode == REQ_PICK_FOLDER) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) {
                startTreeScan(data.getData());
            } else {
                setStatus("No folder was chosen.\n\n"
                        + "Press \"Find my disc\" and pick the folder the .bin is in, or use "
                        + "\"Choose file...\" to point straight at it.");
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            pickFolder();
            return;
        }
        if (!hasStorageAccess()) {
            requestStorageAccess();
            return;
        }
        startScan();
    }

    /**
     * Android 11+: the player picks the folder in the system file picker,
     * which opens on Download, and the search runs inside that folder only.
     * Reading a .bin by path there would need all-files access.
     */
    private void pickFolder() {
        setStatus("Pick the folder your Silent Hill .bin is in (usually Download), "
                + "then tap \"Use this folder\".");
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, DocumentsContract.buildDocumentUri(
                "com.android.externalstorage.documents", "primary:" + Environment.DIRECTORY_DOWNLOADS));
        } catch (Exception ignored) { }
        try {
            startActivityForResult(i, REQ_PICK_FOLDER);
        } catch (Exception e) {
            setStatus("No folder picker is available on this device.\n\nUse \"Choose file...\" instead.");
        }
    }

    /** A disc found inside a picked folder. */
    private static final class TreeDisc {
        final Uri    uri;
        final String name;
        final long   size;

        TreeDisc(Uri uri, String name, long size) {
            this.uri = uri;
            this.name = name;
            this.size = size;
        }
    }

    private void startTreeScan(final Uri tree) {
        setBusy(true);
        setStatus("Looking for a disc image...");

        new Thread(new Runnable() {
            @Override public void run() {
                final List<TreeDisc> found = new ArrayList<TreeDisc>();
                try {
                    collectTreeDiscs(tree, DocumentsContract.getTreeDocumentId(tree), found, 2);
                } catch (Exception e) {
                    Log.w(TAG, "folder scan failed: " + e);
                }

                if (found.isEmpty()) {
                    setBusy(false);
                    setStatus("No Silent Hill disc image in that folder.\n\n"
                            + "Press \"Find my disc\" and pick the folder the .bin is in, "
                            + "or use \"Choose file...\" to point straight at it.");
                    return;
                }
                if (found.size() == 1) {
                    copyTreeDisc(found.get(0));
                    return;
                }

                setBusy(false);
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        setStatus("Found " + found.size() + " Silent Hill discs. Which one?");
                        candidates.removeAllViews();
                        for (final TreeDisc d : found) {
                            Button btn = new Button(SetupActivity.this);
                            btn.setText(d.name + "  (" + (d.size / (1024 * 1024)) + " MB)");
                            btn.setAllCaps(false);
                            btn.setOnClickListener(new View.OnClickListener() {
                                @Override public void onClick(View v) {
                                    candidates.removeAllViews();
                                    setBusy(true);
                                    new Thread(new Runnable() {
                                        @Override public void run() { copyTreeDisc(d); }
                                    }).start();
                                }
                            });
                            candidates.addView(btn);
                        }
                    }
                });
            }
        }).start();
    }

    /** Same depth and filters as the path scan (collectDiscs). */
    private void collectTreeDiscs(Uri tree, String docId, List<TreeDisc> out, int depth) {
        if (depth < 0) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = {
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        };
        Cursor c = null;
        try {
            c = getContentResolver().query(children, cols, null, null, null);
            if (c == null) return;
            while (c.moveToNext()) {
                String id   = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                long   size = c.isNull(3) ? 0 : c.getLong(3);

                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    if (!"Android".equals(name)) collectTreeDiscs(tree, id, out, depth - 1);
                    continue;
                }
                if (name == null || !name.toLowerCase().endsWith(".bin")) continue;
                if (size < MIN_DISC_BYTES || size > MAX_DISC_BYTES) continue;

                Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                if (!isSilentHillDisc(doc)) continue;

                Log.i(TAG, "disc candidate: " + name + " (" + size + ")");
                out.add(new TreeDisc(doc, name, size));
            }
        } catch (Exception e) {
            Log.w(TAG, "folder scan: " + e);
        } finally {
            if (c != null) c.close();
        }
    }

    /** Copies the picked folder's disc into place under its own name, then
     * removes the original as the path scan's move does, so the phone does not
     * end up holding the 600 MB image twice. */
    private void copyTreeDisc(TreeDisc d) {
        setStatus("Found:\n" + d.name + "\n\nCopying it into place...");
        File dest = new File(targetDir, d.name);
        try {
            InputStream in = getContentResolver().openInputStream(d.uri);
            if (in == null) {
                fail("Could not open the disc image.");
                return;
            }
            if (copyStream(in, dest, d.size)) {
                try {
                    if (DocumentsContract.deleteDocument(getContentResolver(), d.uri))
                        Log.i(TAG, "removed source copy " + d.name);
                } catch (Exception ignored) { }
                done(dest);
            } else {
                fail("Copying the disc image failed. Is there enough free space?");
            }
        } catch (Exception e) {
            fail("Could not read the disc image: " + e.getMessage());
        }
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
        try {
            return headHasSerial(new java.io.FileInputStream(f), f.getName());
        } catch (Exception e) {
            return false;
        }
    }

    private boolean isSilentHillDisc(Uri doc) {
        try {
            return headHasSerial(getContentResolver().openInputStream(doc), doc.getLastPathSegment());
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean headHasSerial(InputStream in, String name) {
        if (in == null) return false;
        try {
            /* The ISO descriptors and root directory live near the start, but
             * 2352-byte raw sectors push them further in than a plain ISO would,
             * so read generously rather than assuming an offset. */
            byte[] buf = new byte[2 * 1024 * 1024];
            int n = 0, r;
            while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0) n += r;
            if (n <= 0) return false;

            String head = new String(buf, 0, n, "ISO-8859-1");
            for (String serial : SH_SERIALS) {
                if (head.contains(serial)) {
                    Log.i(TAG, "identified " + serial + " in " + name);
                    return true;
                }
            }
            return false;
        } catch (Exception e) {
            return false;
        } finally {
            try { in.close(); } catch (IOException ignored) { }
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

    /**
     * The update check, when check_updates allows it, then the game. Asked
     * before the bundled files are staged: an update brings its own, and the
     * new version stages them on its first launch.
     */
    private void launchGame() {
        final File root = StorageLocations.resolve(this);

        if (root == null || !AppUpdate.enabled(root)) {
            continueLaunch();
            return;
        }

        showMessage("Checking for updates...");
        new Thread(new Runnable() {
            @Override public void run() {
                final AppUpdate.Offer offer = AppUpdate.check();
                runOnUiThread(new Runnable() {
                    @Override public void run() {
                        if (offer == null) continueLaunch();
                        else askToUpdateApp(root, offer);
                    }
                });
            }
        }, "SH update check").start();
    }

    /** A bare line of text, for the moments before the game takes the screen. */
    private void showMessage(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.LTGRAY);
        t.setGravity(Gravity.CENTER);
        t.setBackgroundColor(Color.BLACK);
        setContentView(t);
    }

    private void askToUpdateApp(final File root, final AppUpdate.Offer offer) {
        String when = java.text.DateFormat.getDateTimeInstance(
            java.text.DateFormat.MEDIUM, java.text.DateFormat.SHORT)
            .format(new java.util.Date(offer.commitTime * 1000L));

        new AlertDialog.Builder(this)
            .setTitle("Update available")
            .setMessage("A newer build of the game is available (" + when + ", "
                      + offer.sha.substring(0, Math.min(9, offer.sha.length())) + ", "
                      + (offer.size / (1024 * 1024)) + " MB).\n\n"
                      + "Download and install it now? Your saves, settings and disc "
                      + "image are kept.")
            .setCancelable(false)
            .setPositiveButton("Yes", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    downloadUpdate(offer);
                }
            })
            .setNegativeButton("No", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    continueLaunch();
                }
            })
            .setNeutralButton("Don't ask again", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    AppUpdate.disable(root);
                    Toast.makeText(SetupActivity.this,
                        "Update checks are off. Turn them back on in Options.",
                        Toast.LENGTH_LONG).show();
                    continueLaunch();
                }
            })
            .show();
    }

    private BroadcastReceiver installReceiver;

    private void downloadUpdate(final AppUpdate.Offer offer) {
        final ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        final TextView label = new TextView(this);
        label.setText("Downloading the update...");
        label.setTextColor(Color.LTGRAY);

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setGravity(Gravity.CENTER);
        box.setBackgroundColor(Color.BLACK);
        int pad = (int) (32 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad, pad, pad);
        box.addView(label);
        box.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                                                       LinearLayout.LayoutParams.WRAP_CONTENT));
        setContentView(box);

        new Thread(new Runnable() {
            @Override public void run() {
                final File apk = AppUpdate.download(SetupActivity.this, offer, new AppUpdate.Progress() {
                    private int last = -1;
                    @Override public void onProgress(final long done, final long total) {
                        final int permille = (total > 0) ? (int) (done * 1000 / total) : 0;
                        if (permille == last) return;
                        last = permille;
                        runOnUiThread(new Runnable() {
                            @Override public void run() { bar.setProgress(permille); }
                        });
                    }
                });
                runOnUiThread(new Runnable() {
                    @Override public void run() { installUpdate(apk); }
                });
            }
        }, "SH update download").start();
    }

    private void installUpdate(File apk) {
        if (apk == null) {
            updateFailed("The update could not be downloaded.");
            return;
        }

        installReceiver = new BroadcastReceiver() {
            @Override public void onReceive(Context ctx, Intent intent) {
                int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
                if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                    Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
                    if (confirm != null) {
                        startActivity(confirm);
                        return;
                    }
                }
                if (status == PackageInstaller.STATUS_SUCCESS) {
                    return; /* the system replaces this process */
                }
                Log.w(TAG, "update: install status " + status + " "
                         + intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
                updateFailed(status == PackageInstaller.STATUS_FAILURE_ABORTED
                             ? "The update was not installed." : "The update could not be installed.");
            }
        };
        IntentFilter filter = new IntentFilter(AppUpdate.ACTION_STATUS);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(installReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(installReceiver, filter);
        }

        showMessage("Installing the update...");
        if (!AppUpdate.install(this, apk)) {
            updateFailed("The update could not be installed.");
        }
    }

    /** Never strands the player: whatever went wrong, the current version plays. */
    private void updateFailed(String why) {
        if (installReceiver != null) {
            try { unregisterReceiver(installReceiver); } catch (IllegalArgumentException ignored) { }
            installReceiver = null;
        }
        Toast.makeText(this, why, Toast.LENGTH_LONG).show();
        continueLaunch();
    }

    @Override
    protected void onDestroy() {
        if (installReceiver != null) {
            try { unregisterReceiver(installReceiver); } catch (IllegalArgumentException ignored) { }
            installReceiver = null;
        }
        super.onDestroy();
    }

    private void continueLaunch() {
        final File root = StorageLocations.resolve(this);
        final List<String> outdated =
            (root != null) ? BundledAssets.stage(this, root) : new ArrayList<String>();

        if (outdated.isEmpty()) {
            startGame();
            return;
        }

        int mode = BundledAssets.mode(this);
        if (mode == BundledAssets.MODE_ALWAYS) {
            BundledAssets.update(this, root, outdated);
            startGame();
            return;
        }
        if (mode == BundledAssets.MODE_NEVER) {
            startGame();
            return;
        }
        askToUpdateFiles(root, outdated);
    }

    /**
     * Asked on every launch until it is answered for good: a player who says
     * "not now" is still running text and fonts the new code was not written
     * for, and should hear about it again rather than forget it happened.
     */
    private void askToUpdateFiles(final File root, final List<String> outdated) {
        StringBuilder list = new StringBuilder();
        int shown = Math.min(outdated.size(), 12);
        for (int i = 0; i < shown; i++) {
            list.append("\n  ").append(outdated.get(i).replaceFirst("^gamedata/", ""));
        }
        if (outdated.size() > shown) {
            list.append("\n  and ").append(outdated.size() - shown).append(" more");
        }

        TextView msg = new TextView(this);
        msg.setText("This version of the app comes with newer copies of "
                  + outdated.size() + (outdated.size() == 1 ? " file" : " files")
                  + " the game already has:\n" + list + "\n\n"
                  + "Old copies can make text show the wrong letters. Your current "
                  + "copies are moved to " + BundledAssets.BACKUP_DIR
                  + ", so nothing is lost.");

        final CheckBox dontAsk = new CheckBox(this);
        dontAsk.setText("Don't ask again");

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(msg);
        box.addView(dontAsk);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(box);

        new AlertDialog.Builder(this)
            .setTitle("Update game files?")
            .setView(scroll)
            .setCancelable(false)
            .setPositiveButton("Update", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    if (dontAsk.isChecked()) {
                        BundledAssets.setMode(SetupActivity.this, BundledAssets.MODE_ALWAYS);
                    }
                    BundledAssets.update(SetupActivity.this, root, outdated);
                    startGame();
                }
            })
            .setNegativeButton("Keep mine", new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int which) {
                    if (dontAsk.isChecked()) {
                        BundledAssets.setMode(SetupActivity.this, BundledAssets.MODE_NEVER);
                        BundledAssets.keep(SetupActivity.this, root, outdated);
                    }
                    startGame();
                }
            })
            .show();
    }

    private void startGame() {
        startActivity(new Intent(this, SilentHillActivity.class));
        finish();
    }
}
