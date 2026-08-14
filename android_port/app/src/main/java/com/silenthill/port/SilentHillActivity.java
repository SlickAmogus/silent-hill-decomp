package com.silenthill.port;

import org.libsdl.app.SDLActivity;

public class SilentHillActivity extends SDLActivity {
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
}
