package org.wwhdrecomp.app;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * Hands the crash logs (CrashLogs) to the app the user shares them with: read only, one file per
 * content://org.wwhdrecomp.app.crashlogs/crash-YYYYmmdd-HHMMSS.log. Not exported; the share
 * intent grants the receiving app access to the URIs it carries.
 */
public final class CrashLogProvider extends ContentProvider {
    static final String AUTHORITY = "org.wwhdrecomp.app.crashlogs";

    static Uri uri(File log) { return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath(log.getName()).build(); }

    private File file(Uri u) throws FileNotFoundException {
        String name = u.getLastPathSegment();
        if (name == null || !CrashLogs.isLogName(name)) throw new FileNotFoundException(String.valueOf(u));
        File f = new File(CrashLogs.dir(getContext()), name);
        if (!f.isFile()) throw new FileNotFoundException(name);
        return f;
    }

    @Override
    public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read only");
        return ParcelFileDescriptor.open(file(u), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri u, String[] projection, String selection, String[] args, String sort) {
        File f;
        try {
            f = file(u);
        } catch (FileNotFoundException e) {
            return null;
        }
        String[] cols = projection != null ? projection : new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++)
            row[i] = OpenableColumns.DISPLAY_NAME.equals(cols[i]) ? f.getName() : OpenableColumns.SIZE.equals(cols[i]) ? (Object) f.length() : null;
        MatrixCursor c = new MatrixCursor(cols, 1);
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri u) { return "text/plain"; }

    @Override
    public Uri insert(Uri u, ContentValues v) { throw new UnsupportedOperationException(); }

    @Override
    public int delete(Uri u, String s, String[] a) { throw new UnsupportedOperationException(); }

    @Override
    public int update(Uri u, ContentValues v, String s, String[] a) { throw new UnsupportedOperationException(); }
}
