package com.silenthill.port;

import android.content.res.AssetManager;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

import android.content.pm.ActivityInfo;
import android.os.Build;
import android.view.View;
import android.view.WindowManager;

import java.io.BufferedReader;
import java.io.FileReader;

public class SilentHillActivity extends SDLActivity {

    private static final String TAG = "SilentHill";

    @Override
    protected String[] getLibraries() {
        // c++_shared has to be loaded before anything that links the STL, and
        // libmain.so is the whole game (pc_port builds as a shared object on
        // Android instead of an executable).
        return new String[] {
            "c++_shared",
            "SDL2",
            "main"
        };
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        // Before super.onCreate, which is what starts SDL and therefore
        // SDL_main. main() chdir()s to this same directory expecting the data
        // to already be in place.
        unpackBundledAssets();
        publishDiscDropDir();
        applyOrientationConfig();
        applyImmersiveMode();
        super.onCreate(savedInstanceState);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            applyImmersiveMode();
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        applyImmersiveMode();
    }

    /**
     * Optimizes display for Retroid Pocket 6 and modern Android devices:
     * enables edge-to-edge rendering around cutouts, sticky immersive mode,
     * and keeps screen awake during gameplay.
     */
    private void applyImmersiveMode() {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            }
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            );
        } catch (Exception e) {
            Log.w(TAG, "applyImmersiveMode: " + e.getMessage());
        }
    }

    /**
     * Reads screen_orientation from config.cfg:
     *   0 = Auto / Sensor (Landscape and Portrait)
     *   1 = Lock Landscape
     *   2 = Lock Portrait
     */
    private void applyOrientationConfig() {
        File cfg = new File(getExternalFilesDir(null), "config.cfg");
        int mode = 0; // default to Auto / Sensor
        if (cfg.exists()) {
            try (BufferedReader r = new BufferedReader(new FileReader(cfg))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (line.startsWith("screen_orientation")) {
                        String[] parts = line.split("=");
                        if (parts.length > 1) {
                            String val = parts[1].trim().toLowerCase();
                            if (val.contains("landscape") || val.equals("1")) {
                                mode = 1;
                            } else if (val.contains("portrait") || val.equals("2")) {
                                mode = 2;
                            } else {
                                mode = 0;
                            }
                        }
                    }
                }
            } catch (Exception ignored) {}
        }

        if (mode == 1) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        } else if (mode == 2) {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT);
        } else {
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR);
        }
    }

    /**
     * Creates the folder users drop their disc image into, and hands the path
     * to native code (BuildDiscSearchRoots reads SH_DISC_DROP_DIR).
     *
     * From Android 11 on, Android/data -- where the app's files dir and its
     * gamedata/ live -- cannot be opened by file-manager apps, so a user with
     * no adb and no built-in Files app cannot deliver a disc image at all.
     * Android/media/<package> is exempt from that restriction and still needs
     * no permission, which makes it the one location both sides can reach.
     *
     * Must run before super.onCreate: that starts SDL and therefore SDL_main,
     * and the environment has to be in place before native code reads it.
     */
    private void publishDiscDropDir() {
        File[] mediaDirs = getExternalMediaDirs();
        if (mediaDirs == null || mediaDirs.length == 0 || mediaDirs[0] == null) {
            Log.w(TAG, "No external media dir; disc image must go in the files dir.");
            return;
        }

        File drop = mediaDirs[0];
        if (!drop.isDirectory() && !drop.mkdirs()) {
            Log.w(TAG, "Could not create disc drop dir: " + drop);
            return;
        }

        try {
            Os.setenv("SH_DISC_DROP_DIR", drop.getAbsolutePath(), true);
            Log.i(TAG, "disc drop dir: " + drop.getAbsolutePath());
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv SH_DISC_DROP_DIR failed: " + e.getMessage());
        }
    }

    /**
     * Copies the port's own assets out of the APK and into the app's external
     * files dir, which is where native code looks for them (main() chdir()s
     * there, and fs_pc.c resolves "./gamedata" against it).
     *
     * This is only the redistributable half of the data: decal.png, the
     * OFL-licensed fonts, the language pack, UI sounds. The game's own content
     * is NOT shipped and never will be -- the user drops their own
     * "Silent Hill (USA).bin" and its extracted gamedata/ into the same
     * directory.
     */
    private void unpackBundledAssets() {
        File target = getExternalFilesDir(null);
        if (target == null) {
            Log.e(TAG, "No external files dir; cannot stage bundled assets.");
            return;
        }
        try {
            copyAssetTree(getAssets(), "gamedata", target);
            copyAssetTree(getAssets(), "licenses", target);
        } catch (IOException e) {
            Log.e(TAG, "Failed staging bundled assets: " + e.getMessage());
        }
    }

    private void copyAssetTree(AssetManager am, String path, File destRoot) throws IOException {
        String[] children = am.list(path);

        if (children == null || children.length == 0) {
            copyAssetFile(am, path, new File(destRoot, path));
            return;
        }

        File dir = new File(destRoot, path);
        if (!dir.isDirectory() && !dir.mkdirs()) {
            throw new IOException("mkdir failed: " + dir);
        }
        for (String child : children) {
            copyAssetTree(am, path + "/" + child, destRoot);
        }
    }

    private void copyAssetFile(AssetManager am, String assetPath, File dest) throws IOException {
        // Overwriting every launch would stomp a user's edited pl.lang, so only
        // write when the file is missing. Size is not compared: these assets do
        // not change without an app update, and the whole point is that the
        // user may legitimately replace them.
        if (dest.exists()) {
            return;
        }
        File parent = dest.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
            throw new IOException("mkdir failed: " + parent);
        }

        byte[] buf = new byte[16 * 1024];
        try (InputStream in = am.open(assetPath);
             OutputStream out = new FileOutputStream(dest)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
        Log.i(TAG, "staged " + assetPath);
    }
}
