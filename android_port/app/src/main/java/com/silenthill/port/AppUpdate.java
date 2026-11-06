package com.silenthill.port;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.Build;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Updates from the realtime release: every push to android-port builds an APK
 * signed with the maintainer key and uploads it there as SilentHill-android.apk.
 *
 * Which build it is comes from the asset's label, which CI sets to
 * "SilentHill-android.apk (build <sha>, <commit unix time>)". This app was
 * built from a commit too (BuildConfig), and the newer COMMIT TIME wins. Not
 * "different sha", because a local build of unpushed work is newer than
 * anything on the release and must not be offered a downgrade.
 *
 * The switch is check_updates in config.cfg, shared with the in-game Options
 * row: the game writes it there, and "Don't ask again" here writes it too.
 */
final class AppUpdate {

    private static final String TAG = "SilentHill";

    private static final String RELEASE_API =
        "https://api.github.com/repos/SlickAmogus/silent-hill-decomp/releases/tags/realtime";
    private static final String ASSET = "SilentHill-android.apk";
    private static final Pattern LABEL = Pattern.compile("\\(build ([0-9a-f]+), (\\d+)\\)");

    static final String CONFIG_KEY = "check_updates";
    static final String ACTION_STATUS = "com.silenthill.port.UPDATE_STATUS";

    /** What the release offers, when it is newer than this app. */
    static final class Offer {
        String sha;
        long   commitTime;
        long   size;
        String url;
    }

    private AppUpdate() { }

    // ------------------------------------------------------------------------
    // The switch

    /** On unless config.cfg says check_updates = 0. A missing file or key is on. */
    static boolean enabled(File root) {
        String v = readKey(new File(root, "config.cfg"), CONFIG_KEY);
        return v == null || !v.trim().equals("0");
    }

    static void disable(File root) {
        writeKey(new File(root, "config.cfg"), CONFIG_KEY, "0");
    }

    private static String readKey(File cfg, String key) {
        if (!cfg.isFile()) return null;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(cfg), "UTF-8"))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0 && line.substring(0, eq).trim().equals(key)) {
                    return line.substring(eq + 1).trim();
                }
            }
        } catch (IOException e) {
            Log.w(TAG, "update: reading config.cfg: " + e.getMessage());
        }
        return null;
    }

    /** Replaces the key's line, or appends one, leaving every other line as it was. */
    private static void writeKey(File cfg, String key, String value) {
        List<String> lines = new ArrayList<String>();
        boolean found = false;

        if (cfg.isFile()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(cfg), "UTF-8"))) {
                String line;
                while ((line = r.readLine()) != null) {
                    int eq = line.indexOf('=');
                    if (eq > 0 && line.substring(0, eq).trim().equals(key)) {
                        line = key + " = " + value;
                        found = true;
                    }
                    lines.add(line);
                }
            } catch (IOException e) {
                Log.w(TAG, "update: reading config.cfg: " + e.getMessage());
                return;
            }
        }
        if (!found) {
            lines.add(key + " = " + value);
        }

        try (OutputStream out = new FileOutputStream(cfg)) {
            StringBuilder sb = new StringBuilder();
            for (String l : lines) sb.append(l).append('\n');
            out.write(sb.toString().getBytes("UTF-8"));
        } catch (IOException e) {
            Log.w(TAG, "update: writing config.cfg: " + e.getMessage());
        }
    }

    // ------------------------------------------------------------------------
    // The check (network: never on the main thread)

    /** The newer build on the release, or null: none, unknown, or no network. */
    static Offer check() {
        if (BuildConfig.GIT_COMMIT_TIME <= 0) {
            Log.i(TAG, "update: this build has no commit stamp, not checking");
            return null;
        }

        HttpURLConnection c = null;
        try {
            c = open(RELEASE_API, 5000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            if (c.getResponseCode() != 200) {
                Log.i(TAG, "update: release query answered " + c.getResponseCode());
                return null;
            }

            JSONArray assets = new JSONObject(readAll(c.getInputStream())).getJSONArray("assets");
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                if (!ASSET.equals(a.optString("name"))) continue;

                Matcher m = LABEL.matcher(a.optString("label", ""));
                if (!m.find()) {
                    Log.i(TAG, "update: the release APK carries no build label yet");
                    return null;
                }

                Offer o = new Offer();
                o.sha        = m.group(1);
                o.commitTime = Long.parseLong(m.group(2));
                o.size       = a.optLong("size", 0);
                o.url        = a.optString("browser_download_url");

                Log.i(TAG, "update: release " + o.sha + " @" + o.commitTime
                         + ", this app " + BuildConfig.GIT_SHA + " @" + BuildConfig.GIT_COMMIT_TIME);
                return (o.commitTime > BuildConfig.GIT_COMMIT_TIME && o.url.length() > 0) ? o : null;
            }
            Log.i(TAG, "update: no " + ASSET + " on the release");
        } catch (Exception e) {
            Log.i(TAG, "update: check failed: " + e);
        } finally {
            if (c != null) c.disconnect();
        }
        return null;
    }

    // ------------------------------------------------------------------------
    // Download and install

    interface Progress {
        void onProgress(long done, long total);
    }

    /** Downloads the APK into the app's cache. Null on failure. */
    static File download(Context ctx, Offer o, Progress p) {
        File dest = new File(ctx.getCacheDir(), "update.apk");
        HttpURLConnection c = null;

        try {
            c = open(o.url, 15000);
            if (c.getResponseCode() != 200) {
                Log.w(TAG, "update: download answered " + c.getResponseCode());
                return null;
            }
            long total = c.getContentLength() > 0 ? c.getContentLength() : o.size;
            long done  = 0;
            byte[] buf = new byte[64 * 1024];

            try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(dest)) {
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    p.onProgress(done, total);
                }
            }
            if (total > 0 && done != total) {
                Log.w(TAG, "update: download stopped at " + done + " of " + total);
                return null;
            }
            return dest;
        } catch (IOException e) {
            Log.w(TAG, "update: download failed: " + e);
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /**
     * Hands the APK to the system installer. Android asks the player to confirm
     * (and, the first time, to allow installs from this app); the outcome comes
     * back as ACTION_STATUS to the receiver SetupActivity registers.
     *
     * PackageInstaller rather than an ACTION_VIEW of the file: that needs a
     * FileProvider, and this way the install is the same on every API level the
     * app supports.
     */
    static boolean install(Context ctx, File apk) {
        PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
            new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(ctx.getPackageName());

        try {
            int id = pi.createSession(params);
            try (PackageInstaller.Session s = pi.openSession(id)) {
                try (InputStream in = new FileInputStream(apk);
                     OutputStream out = s.openWrite("base.apk", 0, apk.length())) {
                    byte[] buf = new byte[64 * 1024];
                    int n;
                    while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    s.fsync(out);
                }

                Intent status = new Intent(ACTION_STATUS).setPackage(ctx.getPackageName());
                /* MUTABLE: the installer fills in the result extras, and from
                 * Android 12 a PendingIntent must say which it is. */
                int flags = PendingIntent.FLAG_UPDATE_CURRENT;
                if (Build.VERSION.SDK_INT >= 31) flags |= PendingIntent.FLAG_MUTABLE;
                PendingIntent pend = PendingIntent.getBroadcast(ctx, id, status, flags);
                s.commit(pend.getIntentSender());
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "update: install failed to start: " + e);
            return false;
        }
    }

    // ------------------------------------------------------------------------

    private static HttpURLConnection open(String url, int timeoutMs) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setConnectTimeout(timeoutMs);
        c.setReadTimeout(timeoutMs * 3);
        c.setRequestProperty("User-Agent", "SilentHill-android/" + BuildConfig.GIT_SHA);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[16 * 1024];
        int n;
        try {
            while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        } finally {
            in.close();
        }
        return b.toString("UTF-8");
    }
}
