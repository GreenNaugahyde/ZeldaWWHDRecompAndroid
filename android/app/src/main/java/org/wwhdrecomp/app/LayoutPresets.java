package org.wwhdrecomp.app;

import android.content.Intent;
import android.net.Uri;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Named touch-control layouts (the layout editor's Presets): each keeps its style (Classic / Flexible), the controls'
 * layout (ControlsView.layoutString), opacity and outline / filled. Kept in the prefs ("layout_presets", a JSON
 * array); exported and imported as JSON files ({"wwhd_layouts": 1, "presets": [...]}, one preset or all).
 */
final class LayoutPresets {
    static final int PICK_EXPORT = 10, PICK_IMPORT = 11;
    private static final String KEY = "layout_presets";

    static final class Preset {
        String name, layout;
        boolean flexible, filled;
        float opacity;

        JSONObject json() throws JSONException {
            return new JSONObject().put("name", name).put("style", flexible ? "flexible" : "classic").put("layout", layout)
                    .put("opacity", opacity).put("filled", filled);
        }

        static Preset of(JSONObject o) {
            String name = o.optString("name", "").trim(), style = o.optString("style", "");
            if (name.isEmpty() || !(style.equals("classic") || style.equals("flexible"))) return null;
            Preset p = new Preset();
            p.name = name;
            p.flexible = style.equals("flexible");
            p.layout = o.optString("layout", "");
            p.opacity = Math.max(0.05f, Math.min(1f, (float) o.optDouble("opacity", 0.45)));
            p.filled = o.optBoolean("filled", false);
            return p;
        }
    }

    private final MainActivity a;
    private List<Preset> toExport;  // waiting for the file the player picks

    LayoutPresets(MainActivity a) { this.a = a; }

    List<Preset> load() {
        List<Preset> list = new ArrayList<>();
        try {
            JSONArray arr = new JSONArray(a.prefs.getString(KEY, "[]"));
            for (int i = 0; i < arr.length(); i++) {
                Preset p = Preset.of(arr.getJSONObject(i));
                if (p != null) list.add(p);
            }
        } catch (JSONException ignored) {
        }
        return list;
    }

    private void store(List<Preset> list) {
        JSONArray arr = new JSONArray();
        try {
            for (Preset p : list) arr.put(p.json());
        } catch (JSONException ignored) {
        }
        a.prefs.edit().putString(KEY, arr.toString()).apply();
    }

    private String styleName(boolean flexible) {
        return a.getString(flexible ? R.string.controls_flexible : R.string.controls_classic);
    }

    private boolean flexibleNow() { return a.prefs.getInt("controls_style", 0) == 1; }

    /** the picker: the saved layouts (tap one: Load / Export / Delete), Import, Export all, Save current */
    void show() {
        ControlsView cv = a.controlsView();
        if (cv == null) return;
        List<Preset> list = load();
        LinearLayout rows = new LinearLayout(a);
        rows.setOrientation(LinearLayout.VERTICAL);
        GameDialog d = new GameDialog(a).title(R.string.lp_title);
        if (list.isEmpty()) {
            TextView t = GameUi.text(a, true);
            t.setText(R.string.lp_none);
            t.setTextColor(GameUi.INK);
            t.setTextSize(17);
            rows.addView(t);
        }
        // the current style's layouts first
        List<Preset> sorted = new ArrayList<>();
        for (Preset p : list) if (p.flexible == flexibleNow()) sorted.add(p);
        for (Preset p : list) if (p.flexible != flexibleNow()) sorted.add(p);
        float dp = a.getResources().getDisplayMetrics().density;
        for (Preset p : sorted) {
            TextView b = GameUi.barButton(a, p.name + "  ·  " + styleName(p.flexible), v -> {
                d.dismiss();
                showPreset(p);
            }, false, true);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, Math.round(52 * dp));
            lp.bottomMargin = Math.round(8 * dp);
            rows.addView(b, lp);
        }
        d.content(rows)
                .button(R.string.opt_cancel, null)
                .button(R.string.lp_import, this::pickImport)
                .button(R.string.lp_export_all, () -> pickExport(list, a.getString(R.string.lp_file_all)), !list.isEmpty())
                .button(R.string.lp_save, this::saveCurrent)
                .show();
    }

    private void showPreset(Preset p) {
        new GameDialog(a).title(p.name).message(a.getString(R.string.lp_preset_info, styleName(p.flexible)))
                .button(R.string.lp_delete, () -> new GameDialog(a).title(p.name).message(R.string.lp_delete_confirm)
                        .button(R.string.opt_cancel, this::show)
                        .button(R.string.lp_delete, () -> {
                            List<Preset> list = load();
                            list.removeIf(q -> q.name.equals(p.name) && q.flexible == p.flexible);
                            store(list);
                            show();
                        }).show())
                .button(R.string.lp_export, () -> {
                    List<Preset> one = new ArrayList<>();
                    one.add(p);
                    pickExport(one, p.name);
                })
                .button(R.string.opt_cancel, this::show)
                .button(R.string.lp_load, () -> apply(p)).show();
    }

    /** the edited layout under a name; the same name in the same style replaces it (asked first) */
    private void saveCurrent() {
        ControlsView cv = a.controlsView();
        if (cv == null) return;
        boolean flexible = flexibleNow();
        List<Preset> list = load();
        int n = 1;
        for (Preset p : list) if (p.flexible == flexible) n++;
        a.askName(a.getString(R.string.lp_save_title, styleName(flexible)), styleName(flexible) + " " + n, name -> {
            if (name.isEmpty()) {
                show();
                return;
            }
            Preset p = new Preset();
            p.name = name;
            p.flexible = flexible;
            p.layout = cv.layoutString();
            p.opacity = cv.editOpacity();
            p.filled = cv.filled();
            boolean exists = false;
            for (Preset q : list) exists |= q.name.equals(name) && q.flexible == flexible;
            Runnable save = () -> {
                List<Preset> l = load();
                l.removeIf(q -> q.name.equals(name) && q.flexible == flexible);
                l.add(p);
                store(l);
                Toast.makeText(a, a.getString(R.string.lp_saved, name), Toast.LENGTH_SHORT).show();
            };
            if (!exists) save.run();
            else new GameDialog(a).title(name).message(R.string.lp_replace).button(R.string.opt_cancel, this::show)
                    .button(R.string.lp_replace_yes, save).show();
        });
    }

    /** into the editor (Done keeps it); a layout of the other style switches the touch controls to it (asked first) */
    private void apply(Preset p) {
        Runnable load = () -> {
            ControlsView cv = a.controlsView();
            if (cv == null) return;
            if (p.flexible != flexibleNow()) {
                a.prefs.edit().putInt("controls_style", p.flexible ? 1 : 0).apply();
                a.applyControlsAppearance();  // (the editor keeps its layout: set below)
            }
            cv.setEditLayout(p.layout, p.opacity, p.filled);
        };
        if (p.flexible == flexibleNow()) load.run();
        else new GameDialog(a).title(p.name).message(a.getString(R.string.lp_switch_style, styleName(p.flexible)))
                .button(R.string.opt_cancel, this::show).button(R.string.lp_load, load).show();
    }

    // ---- files
    private void pickExport(List<Preset> list, String name) {
        toExport = list;
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/json");
        i.putExtra(Intent.EXTRA_TITLE, name.replaceAll("[\\\\/:*?\"<>|]", "_") + ".json");
        a.startActivityForResult(i, PICK_EXPORT);
    }

    private void pickImport() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");  // (JSON files often have no type the picker matches)
        i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        a.startActivityForResult(i, PICK_IMPORT);
    }

    void exportTo(Uri file) {
        if (toExport == null) return;
        try (OutputStream out = a.getContentResolver().openOutputStream(file, "wt")) {
            JSONArray arr = new JSONArray();
            for (Preset p : toExport) arr.put(p.json());
            out.write(new JSONObject().put("wwhd_layouts", 1).put("presets", arr).toString(2).getBytes(StandardCharsets.UTF_8));
            Toast.makeText(a, a.getString(R.string.lp_exported, toExport.size()), Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(a, a.getString(R.string.lp_failed, String.valueOf(e.getMessage())), Toast.LENGTH_LONG).show();
        }
        toExport = null;
        show();
    }

    /** adds the files' layouts; a name already taken in that style gets " (2)", " (3)", ... */
    void importFrom(List<Uri> files) {
        List<Preset> list = load();
        int added = 0, bad = 0;
        for (Uri u : files) {
            try (InputStream in = a.getContentResolver().openInputStream(u)) {
                ByteArrayOutputStream buf = new ByteArrayOutputStream();
                byte[] b = new byte[8192];
                for (int n; (n = in.read(b)) > 0 && buf.size() < (1 << 20); ) buf.write(b, 0, n);
                JSONArray arr = new JSONObject(buf.toString("UTF-8")).getJSONArray("presets");
                for (int i = 0; i < arr.length(); i++) {
                    Preset p = Preset.of(arr.getJSONObject(i));
                    if (p == null) {
                        bad++;
                        continue;
                    }
                    String base = p.name;
                    for (int k = 2; taken(list, p); k++) p.name = base + " (" + k + ")";
                    list.add(p);
                    added++;
                }
            } catch (Exception e) {
                bad++;
            }
        }
        store(list);
        String msg = a.getString(R.string.lp_imported, added);
        if (bad > 0) msg += " (" + a.getString(R.string.lp_import_skipped, bad) + ")";
        Toast.makeText(a, msg, Toast.LENGTH_LONG).show();
        show();
    }

    private static boolean taken(List<Preset> list, Preset p) {
        for (Preset q : list) if (q.name.equals(p.name) && q.flexible == p.flexible) return true;
        return false;
    }
}
