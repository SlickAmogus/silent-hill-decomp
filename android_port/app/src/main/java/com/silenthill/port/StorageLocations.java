package com.silenthill.port;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.os.Environment;
import android.util.Log;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Where the game keeps its data: the disc image, gamedata/, config.cfg, the
 * memory cards and the log.
 *
 * This used to be getExternalFilesDir(null) -- /sdcard/Android/data/<pkg>/files.
 * From Android 11 that path cannot be opened by file-manager apps at all, so
 * players could neither deliver a disc image nor retrieve a save, and reported
 * the game as saving "internally where I cannot get at it". The media dirs
 * (Android/media/<pkg>) are exempt from that restriction and still need no
 * permission, which makes them the one place both sides can reach.
 *
 * getExternalMediaDirs returns one entry per storage VOLUME: index 0 is the
 * built-in one, and a physical SD card (or a USB stick on a device that mounts
 * one) appears after it. That is the whole reason this is a choice rather than
 * a constant -- a removable card is preferred by default, because it is the one
 * a player can take out and read on something else.
 */
final class StorageLocations {

    private static final String TAG   = "SilentHill";
    private static final String PREFS = "storage";
    private static final String KEY   = "data_root";

    /** A place the game could keep its data, with a name to show the player. */
    static final class Option {
        final File   dir;
        final String label;
        final boolean removable;

        Option(File dir, String label, boolean removable) {
            this.dir = dir;
            this.label = label;
            this.removable = removable;
        }
    }

    private StorageLocations() { }

    /**
     * Every volume the game can write to, removable ones first.
     *
     * Writability is tested rather than assumed: a card can be present but
     * mounted read-only, and vendor builds have been seen returning a media dir
     * that cannot be created. An entry that fails is left out entirely, so
     * nothing offers the player a location that will not work.
     */
    static List<Option> candidates(Context ctx) {
        List<Option> out = new ArrayList<Option>();
        File[] media = ctx.getExternalMediaDirs();

        if (media != null) {
            for (int i = 0; i < media.length; i++) {
                File d = media[i];
                if (d == null) {
                    continue;
                }
                if (!d.isDirectory() && !d.mkdirs()) {
                    Log.w(TAG, "media dir " + i + " will not create: " + d);
                    continue;
                }
                if (!d.canWrite()) {
                    Log.w(TAG, "media dir " + i + " is not writable: " + d);
                    continue;
                }

                boolean removable = isRemovable(d, i);
                out.add(new Option(d, removable ? "SD card" : "Phone storage", removable));
            }
        }

        /* Older devices, and some vendor builds, have no usable media dir. A
         * plain folder on the primary volume is browsable everywhere and needs
         * no permission on the API levels where that is true. */
        if (out.isEmpty()) {
            File pub = new File(Environment.getExternalStorageDirectory(), "SilentHill");
            if ((pub.isDirectory() || pub.mkdirs()) && pub.canWrite()) {
                out.add(new Option(pub, "Phone storage", false));
            }
        }

        /* Last resort. Invisible to file managers from Android 11, which is the
         * complaint this class exists to answer -- but a location the game
         * cannot lose is better than none, and it always exists. */
        if (out.isEmpty()) {
            File files = ctx.getExternalFilesDir(null);
            if (files != null && (files.isDirectory() || files.mkdirs())) {
                out.add(new Option(files, "App storage", false));
            }
        }

        /* Removable first: that is the default, and the order the chooser
         * shows. */
        for (int i = 0; i < out.size(); i++) {
            if (out.get(i).removable) {
                out.add(0, out.remove(i));
                break;
            }
        }

        return out;
    }

    /**
     * isExternalStorageRemovable(File) arrived in API 21 but has been observed
     * throwing on paths it does not recognise, and index 0 is the built-in
     * volume on every device. Treat the index as the truth and the query as a
     * refinement.
     */
    private static boolean isRemovable(File dir, int index) {
        if (index == 0) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
            try {
                return Environment.isExternalStorageRemovable(dir);
            } catch (Throwable t) {
                /* Not fatal: anything past index 0 is a second volume, which is
                 * removable for every purpose this class has. */
            }
        }
        return true;
    }

    /**
     * Where every install before this change kept its data, and where an
     * upgrading player's disc image and saves still are. Android 11 made it
     * unopenable by file managers, which is the whole complaint -- but it is
     * still perfectly readable by the app, so nothing here may assume it is
     * empty.
     */
    static File legacyRoot(Context ctx) {
        return ctx.getExternalFilesDir(null);
    }

    /**
     * Does this directory hold an install worth preserving? Any one of the four
     * things a player would be upset to lose: their disc image, the extracted
     * data, their saves, or their settings.
     */
    static boolean hasGameData(File dir) {
        if (dir == null || !dir.isDirectory()) {
            return false;
        }
        if (new File(dir, "gamedata").isDirectory() || new File(dir, "config.cfg").isFile()) {
            return true;
        }

        File[] entries = dir.listFiles();
        if (entries == null) {
            return false;
        }
        for (File f : entries) {
            if (!f.isFile()) {
                continue;
            }
            String n = f.getName().toLowerCase();
            if (n.endsWith(".mcd") || (n.endsWith(".bin") && f.length() >= 32L * 1024 * 1024)) {
                return true;
            }
        }
        return false;
    }

    /** The stored choice, or null if the player has not been asked yet. */
    static File stored(Context ctx) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String path = p.getString(KEY, null);
        if (path == null) {
            return null;
        }

        /* A card can be taken out between launches. Re-validate rather than
         * hand back a path that no longer exists, or the game boots pointing at
         * nothing and reports a missing disc. */
        File d = new File(path);
        if ((d.isDirectory() || d.mkdirs()) && d.canWrite()) {
            return d;
        }

        Log.w(TAG, "stored data root is gone, falling back: " + path);
        return null;
    }

    static void store(Context ctx, File dir) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
           .edit().putString(KEY, dir.getAbsolutePath()).apply();
        Log.i(TAG, "data root: " + dir.getAbsolutePath());
    }

    /** Forget the choice, so the next launch asks again. */
    static void clear(Context ctx) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().remove(KEY).apply();
    }

    /**
     * Where the data lives. Every caller uses this, so the setup screen, the
     * asset unpack and the game's own working directory cannot disagree -- they
     * used to, which is how the disc could land somewhere the game did not look.
     */
    static File resolve(Context ctx) {
        File s = stored(ctx);
        if (s != null) {
            return s;
        }

        /* No choice recorded yet, and an existing install's data is in the
         * legacy folder. Nothing may point away from it until the player has
         * actually been asked -- and SilentHillActivity can start without the
         * setup screen that does the asking (the USB intent filter), so the
         * safe answer has to live here rather than only in the dialog. */
        File legacy = legacyRoot(ctx);
        if (hasGameData(legacy)) {
            return legacy;
        }

        List<Option> opts = candidates(ctx);
        return opts.isEmpty() ? legacy : opts.get(0).dir;
    }

    /**
     * The name for a resolved root, for the in-game row that shows where the
     * data lives. Matched against the candidate list rather than parsed out of
     * the path, which does not reliably say which volume it is.
     */
    static String labelFor(Context ctx, File dir) {
        String want = dir.getAbsolutePath();

        File legacy = legacyRoot(ctx);
        if (legacy != null && legacy.getAbsolutePath().equals(want)) {
            return "App";
        }

        for (Option o : candidates(ctx)) {
            if (o.dir.getAbsolutePath().equals(want)) {
                /* The options value column is narrow, and '_' draws as a space
                 * in the game's font. */
                return o.removable ? "SD_card" : "Phone";
            }
        }

        return "Other";
    }

    /** Only worth asking when there is more than one answer. */
    static boolean worthAsking(Context ctx) {
        return stored(ctx) == null && candidates(ctx).size() > 1;
    }
}
