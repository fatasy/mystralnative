package com.mystral.engine;

import android.os.Bundle;
import org.libsdl.app.SDLActivity;

/**
 * MystralActivity - Main entry point for Mystral Native on Android.
 *
 * Extends SDLActivity which handles:
 * - Native library loading
 * - Surface creation (ANativeWindow)
 * - Input events (touch, keyboard, gamepad)
 * - Lifecycle management (pause/resume)
 *
 * The SDL3 Android backend provides the native window for surface creation.
 */
public class MystralActivity extends SDLActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    /**
     * Override to specify which native libraries to load.
     * SDL handles loading SDL3, then we load mystral-runtime.
     */
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL3",
            "mystral-runtime"
        };
    }

    /**
     * Override to specify the main function.
     * This is called by SDL after the libraries are loaded.
     */
    @Override
    protected String getMainFunction() {
        return "SDL_main";
    }

    /**
     * Get command line arguments for the main function.
     * Can be used to specify the script path.
     */
    @Override
    protected String[] getArguments() {
        // TODO: Read script path from intent extras or assets
        // For now, load a default script from assets
        return new String[] {
            "asset://scripts/main.js"
        };
    }
}
