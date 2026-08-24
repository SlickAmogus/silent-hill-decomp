package com.silenthill.port;

import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.res.AssetManager;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.system.ErrnoException;
import android.system.Os;
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
        publishDataRoot();
        unpackBundledAssets();
        publishDiscDropDir();
        super.onCreate(savedInstanceState);
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
    /**
     * The working directory the game anchors every relative path to -- the disc
     * image, gamedata/, config.cfg, the memory cards, the log. main() reads
     * SH_DATA_ROOT and chdir()s there.
     *
     * This used to be getExternalFilesDir(null) unconditionally, which from
     * Android 11 no file manager can open: players could not deliver a disc or
     * retrieve a save, and reported the game as keeping its data somewhere they
     * could not reach. StorageLocations picks a volume the player chose, an SD
     * card by default, and SetupActivity resolves the same answer -- they used
     * to disagree, so a disc installed by setup could land where the game did
     * not look.
     */
    /**
     * RetroAchievements sign-in.
     *
     * Called from the options menu (PCK_RALOGIN) through JNI, and returns
     * immediately: SDL runs the game loop on this thread, so blocking for the
     * button would stop the very loop that has to service the dialog.
     *
     * The engine's own 2D screens cannot ask for this. They have no text input,
     * no keyboard and no masking, and a password typed into a PSX menu with a
     * D-pad would be its own kind of hostile -- so this one row leaves the
     * game's UI entirely, the way the launcher does on desktop.
     */
    public static void showRetroAchievementsLogin() {
        final Context ctx = getContext();
        if (!(ctx instanceof SilentHillActivity)) {
            Log.w(TAG, "no activity for the sign-in dialog");
            return;
        }

        final SilentHillActivity act = (SilentHillActivity) ctx;

        act.runOnUiThread(new Runnable() {
            @Override public void run() {
                final EditText user = new EditText(act);
                user.setHint("RetroAchievements username");
                user.setInputType(InputType.TYPE_CLASS_TEXT
                                | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
                user.setSingleLine(true);

                final EditText pass = new EditText(act);
                pass.setHint("Password");
                pass.setInputType(InputType.TYPE_CLASS_TEXT
                                | InputType.TYPE_TEXT_VARIATION_PASSWORD);
                pass.setSingleLine(true);

                LinearLayout box = new LinearLayout(act);
                box.setOrientation(LinearLayout.VERTICAL);
                int pad = (int) (16 * act.getResources().getDisplayMetrics().density);
                box.setPadding(pad, pad, pad, 0);
                box.addView(user);
                box.addView(pass);

                new AlertDialog.Builder(act)
                    .setTitle("RetroAchievements")
                    .setMessage("Your password is exchanged once for a token and is "
                              + "never stored. Softcore only.")
                    .setView(box)
                    .setPositiveButton("Sign in", new DialogInterface.OnClickListener() {
                        @Override public void onClick(DialogInterface d, int which) {
                            nativeRaLogin(user.getText().toString(),
                                          pass.getText().toString());
                        }
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
            }
        });
    }

    /** Hands the credentials to Pc_Ra_BeginPasswordLogin, which does the rest. */
    private static native void nativeRaLogin(String username, String password);

    private void publishDataRoot() {
        File root = StorageLocations.resolve(this);
        if (root == null) {
            Log.w(TAG, "no data root resolved; falling back to the SDL default");
            return;
        }

        try {
            Os.setenv("SH_DATA_ROOT", root.getAbsolutePath(), true);
            /* The in-game options row shows this. The path alone does not say
             * which volume it is, and only this side knows. */
            Os.setenv("SH_DATA_LABEL", StorageLocations.labelFor(this, root), true);
            Log.i(TAG, "data root: " + root.getAbsolutePath());
        } catch (ErrnoException e) {
            Log.w(TAG, "setenv SH_DATA_ROOT failed: " + e.getMessage());
        }
    }

    private void publishDiscDropDir() {
        /* Same place the data lives. Two different answers here is what let a
         * disc image sit in a directory the game never searched. */
        File drop = StorageLocations.resolve(this);

        if (drop == null) {
            Log.w(TAG, "no user-visible dir available; log/config stay in the app's files dir.");
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
        File target = StorageLocations.resolve(this);
        if (target == null) {
            Log.e(TAG, "No data root; cannot stage bundled assets.");
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
