package org.wwhdrecomp.app;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;

import java.io.File;

/**
 * Restarts the game in a fresh process (settings like the resolution apply when the renderer
 * starts). Runs in its own process: ends the game's process, waits until it is gone so the two
 * never run side by side, then launches the game again. With EXTRA_CLEAR_SHADERS it deletes the
 * shader cache in between (once the game can no longer write it), with EXTRA_DELETE one file of it.
 */
public final class RestartActivity extends Activity {
    static final String EXTRA_PID = "pid";
    static final String EXTRA_CLEAR_SHADERS = "clear_shaders";
    static final String EXTRA_DELETE = "delete";  // a file in the shader cache to delete (a removed GPU driver's)

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        boolean clearShaders = getIntent().getBooleanExtra(EXTRA_CLEAR_SHADERS, false);
        String delete = getIntent().getStringExtra(EXTRA_DELETE);
        new Thread(() -> {
            if (pid > 0) {
                Process.killProcess(pid);
                for (int i = 0; i < 100 && new File("/proc/" + pid).exists(); i++) {
                    try {
                        Thread.sleep(20);
                    } catch (InterruptedException ignored) {
                    }
                }
            }
            if (clearShaders) Backup.deleteTree(new File(getNoBackupFilesDir(), "shadercache"));
            if (delete != null) {
                File f = new File(delete);
                File dir = new File(getNoBackupFilesDir(), "shadercache");
                if (dir.getAbsolutePath().equals(f.getParent())) //noinspection ResultOfMethodCallIgnored
                    f.delete();
            }
            runOnUiThread(() -> {
                Intent i = new Intent(this, MainActivity.class);
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
                finish();
                Process.killProcess(Process.myPid());
            });
        }).start();
    }
}
