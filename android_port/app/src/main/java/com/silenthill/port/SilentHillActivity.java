package com.silenthill.port;

import android.content.res.AssetManager;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

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
        super.onCreate(savedInstanceState);
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
