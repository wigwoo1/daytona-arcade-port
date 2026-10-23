package org.daytona.local;
import org.libsdl.app.SDLActivity;
import java.io.*;

public final class DaytonaActivity extends SDLActivity {
    @Override protected String[] getLibraries() { return new String[] {"SDL3", "main"}; }
    @Override protected String[] getArguments() {
        File rom = new File(getFilesDir(), "daytona93.zip");
        // This private build contains the user's verified ROM set. Never copy saves.
        if (!rom.isFile()) {
            File temp = new File(getFilesDir(), "daytona93.zip.tmp");
            try (InputStream in = getAssets().open("roms/daytona93.zip"); OutputStream out = new FileOutputStream(temp)) {
                byte[] buffer = new byte[65536]; int n;
                while ((n = in.read(buffer)) != -1) out.write(buffer, 0, n);
            } catch (IOException e) { throw new RuntimeException("Cannot prepare game data", e); }
            if (!temp.renameTo(rom)) throw new RuntimeException("Cannot finish game data preparation");
        }
        return new String[] {"--rom", rom.getAbsolutePath()};
    }
}
