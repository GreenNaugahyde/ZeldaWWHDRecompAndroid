// libwwhdrando.so: TWWHD Randomizer (runtime/third_party/twwhd_rando, WWHD.md) for the app's
// Game > Randomizer (Randomizer.java). The settings live in <work>/config.yaml and preferences.yaml, as
// the randomizer keeps them; each call here loads, changes and writes them. A run (generate) happens in
// the app's process ":rando" (RandoService), once per process: the randomizer keeps global state.
#include <jni.h>

#include <android/log.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

#include <command/Log.hpp>
#include <command/RandoSession.hpp>
#include <options.hpp>
#include <randomizer.hpp>
#include <seedgen/config.hpp>
#include <seedgen/seed.hpp>
#include <utility/color.hpp>
#include <utility/path.hpp>
#include <utility/wwhd_region.hpp>
#include <version.hpp>

#include "link_colors_make.h"

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_wwhdrecomp_app_RandoNative_##name

namespace {

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(s, c);
    return r;
}

std::mutex g_mu;
fspath g_progress;  // <work>/progress: "<percent>\t<label>", read by the app while a run goes on
std::atomic<int> g_percent{0};

void write_progress(const std::string& label) {
    if (g_progress.empty()) return;
    fspath tmp = g_progress;
    tmp += ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "%d\t%s\n", g_percent.load(), label.c_str());
        fclose(f);
        rename(tmp.c_str(), g_progress.c_str());
    }
}

void set_paths(const std::string& data, const std::string& work) {
    wwhd_rando::data_path = fspath(data) / "";
    wwhd_rando::save_path = fspath(work) / "";
    std::filesystem::create_directories(work);
}

fspath config_path() { return wwhd_rando::save_path / "config.yaml"; }
fspath prefs_path() { return wwhd_rando::save_path / "preferences.yaml"; }

// the config as it is (defaults written first if there is none); false and `err` if it can't be read
bool load(Config& c, std::string& err) {
    ConfigError e = Config::writeDefault(config_path(), prefs_path());
    if (e != ConfigError::NONE) return err = "cannot write the default settings: " + ConfigErrorGetName(e), false;
    e = c.loadFromFile(config_path(), prefs_path(), true);
    if (e != ConfigError::NONE && e != ConfigError::DIFFERENT_RANDO_VERSION)
        return err = "cannot read the settings: " + ConfigErrorGetName(e), false;
    return true;
}

bool save(const Config& c, std::string& err) {
    ConfigError e = c.writeToFile(config_path(), prefs_path());
    if (e != ConfigError::NONE) return err = "cannot write the settings: " + ConfigErrorGetName(e), false;
    return true;
}

jstring result(JNIEnv* env, const std::string& err) { return err.empty() ? nullptr : env->NewStringUTF(err.c_str()); }

}  // namespace

namespace wwhd_rando {
void progress_value(int percent) {
    g_percent = percent;
}
void progress_label(const std::string& label) {
    __android_log_print(ANDROID_LOG_INFO, "wwhd", "[rando] %s", label.c_str());
    write_progress(label);
}
}  // namespace wwhd_rando

// a setting by the randomizer's own name (options.cpp settingToName, e.g. "progression_dungeons"), as
// the index of its value; -1 if there is no such setting
JNI_FN(jint, get)(JNIEnv* env, jclass, jstring data, jstring work, jstring name) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return -1;
    Option o = nameToSetting(jstr(env, name));
    if (o == Option::INVALID) return -1;
    return c.settings.getSetting(o);
}

// null when set, else why not
JNI_FN(jstring, set)(JNIEnv* env, jclass, jstring data, jstring work, jstring name, jint value) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    Option o = nameToSetting(jstr(env, name));
    if (o == Option::INVALID) return result(env, "no setting " + jstr(env, name));
    c.settings.setSetting(o, (size_t)value);
    save(c, err);
    return result(env, err);
}

JNI_FN(jstring, seed)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return env->NewStringUTF("");
    return env->NewStringUTF(c.seed.c_str());
}

JNI_FN(jstring, setSeed)(JNIEnv* env, jclass, jstring data, jstring work, jstring seed) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    c.seed = jstr(env, seed);
    save(c, err);
    return result(env, err);
}

// a new random seed name (the randomizer's words), kept in the config; "" when the config can't be read
JNI_FN(jstring, newSeed)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return env->NewStringUTF("");
    c.seed = generate_seed();
    save(c, err);
    return env->NewStringUTF(c.seed.c_str());
}

// all settings back to the randomizer's defaults (a new random seed name; the preferences stay)
JNI_FN(jstring, resetDefaults)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    c.resetDefaultSettings();
    save(c, err);
    return result(env, err);
}

// the settings and seed as the PC randomizer's permalink (the same version makes the same game)
JNI_FN(jstring, permalink)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return env->NewStringUTF("");
    return env->NewStringUTF(c.getPermalink().c_str());
}

// null when the permalink's settings and seed were taken, else why not
JNI_FN(jstring, loadPermalink)(JNIEnv* env, jclass, jstring data, jstring work, jstring permalink) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    PermalinkError e = c.loadPermalink(jstr(env, permalink));
    if (e != PermalinkError::NONE) return result(env, "permalink: " + PermalinkErrorGetName(e));
    save(c, err);
    return result(env, err);
}

// ---- Link's colours (the randomizer's customizer, customizer/data/Link: colour masks over Link's textures in
// permanent_3d.pack), kept in preferences.yaml for the hero's or the casual clothes, whichever Link starts in

namespace {
// the colours as they are now for the clothes chosen, by the customizer's order
std::vector<std::pair<std::string, std::string>> current_colors(const CustomModel& m) {
    std::vector<std::pair<std::string, std::string>> r;
    for (const std::string& n : m.getDefaultColorsOrdering()) r.emplace_back(n, m.getColor(n));
    return r;
}

bool same_colors(const CustomModel& m, const ColorMap_t& preset) {
    for (const std::string& n : m.getDefaultColorsOrdering()) {
        auto it = preset.find(n);
        if (it == preset.end() || strcasecmp(it->second.c_str(), m.getColor(n).c_str()) != 0) return false;
    }
    return true;
}
}  // namespace

// "casual\t0|1", then per preset "preset\t<name>\t<1 if the colours are its>", then per colour
// "color\t<name>\t<default>\t<now>" (hex RRGGBB), a line each; "" when the settings can't be read
JNI_FN(jstring, colors)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return env->NewStringUTF("");
    const CustomModel& m = c.settings.selectedModel;
    std::string r = std::string("casual\t") + (m.casual ? "1" : "0") + "\n";
    for (const ColorPreset& p : m.getPresets())
        r += "preset\t" + p.name + "\t" + (same_colors(m, m.casual ? p.casualColors : p.heroColors) ? "1" : "0") + "\n";
    const ColorMap_t& def = m.getDefaultColorsMap();
    for (const auto& [n, col] : current_colors(m)) {
        auto it = def.find(n);
        r += "color\t" + n + "\t" + (it != def.end() ? it->second : col) + "\t" + col + "\n";
    }
    return env->NewStringUTF(r.c_str());
}

// one colour (hex RRGGBB; "" back to its default); null when set, else why not
JNI_FN(jstring, setColor)(JNIEnv* env, jclass, jstring data, jstring work, jstring name, jstring hex) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    std::string n = jstr(env, name), h = jstr(env, hex);
    CustomModel& m = c.settings.selectedModel;
    if (h.empty()) m.resetSingleColor(n);
    else if (!isValidHexColor(h)) return result(env, "not a colour: " + h);
    else {
        for (char& ch : h) ch = (char)toupper((unsigned char)ch);
        m.setColor(n, h);
    }
    save(c, err);
    return result(env, err);
}

// Link starts in his casual clothes (and the colours are those clothes')
JNI_FN(jstring, setCasual)(JNIEnv* env, jclass, jstring data, jstring work, jboolean casual) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    c.settings.selectedModel.casual = casual;
    save(c, err);
    return result(env, err);
}

// preset `index` (as colors lists them; 0 = the game's colours) for both clothes
JNI_FN(jstring, loadPreset)(JNIEnv* env, jclass, jstring data, jstring work, jint index) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    CustomModel& m = c.settings.selectedModel;
    if (index < 0 || (size_t)index >= m.getPresets().size()) return result(env, "no preset " + std::to_string(index));
    bool casual = m.casual;
    m.loadPreset((size_t)index);
    m.casual = casual;
    save(c, err);
    return result(env, err);
}

// random colours for the clothes chosen: all shifted alike (`chaotic` false: they still go together) or each its own way
JNI_FN(jstring, randomColors)(JNIEnv* env, jclass, jstring data, jstring work, jboolean chaotic) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    static bool seeded = false;
    if (!seeded) srand((unsigned)time(nullptr)), seeded = true;
    if (chaotic) c.settings.selectedModel.randomizeChaotically();
    else c.settings.selectedModel.randomizeOrderly();
    save(c, err);
    return result(env, err);
}

// ---- Link's colours in the game as it is (Mods > Link's colours; their own <work>, apart from the randomizer's): Link wears
// both clothes there, and the hands, mouth and eyes are one texture for both, so those colours are the same in both

namespace {
const char* const kSharedColors[] = {"Skin", "Mouth", "Eyes", "Sclera"};
}

// the shared colours of the clothes chosen into the other clothes' too
JNI_FN(jstring, syncShared)(JNIEnv* env, jclass, jstring data, jstring work) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    CustomModel& m = c.settings.selectedModel;
    std::vector<std::pair<std::string, std::string>> shared;
    for (const char* n : kSharedColors) shared.emplace_back(n, m.getColor(n));
    m.casual = !m.casual;
    for (const auto& [n, col] : shared)
        if (!col.empty()) m.setColor(n, col);
    m.casual = !m.casual;
    save(c, err);
    return result(env, err);
}

// Link's textures in these colours into the file `out` (link_colors_make.h; `separate`: the hero's own skin, mouth and
// eyes); null when done, else why not. About a second (Link.szs unpacked, recoloured, packed again).
JNI_FN(jstring, applyColors)(JNIEnv* env, jclass, jstring data, jstring work, jstring game, jstring out, jboolean separate) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    return result(env, make_link_colors(c, jstr(env, game), jstr(env, out), separate));
}

JNI_FN(jstring, version)(JNIEnv* env, jclass) { return env->NewStringUTF(RANDOMIZER_VERSION); }

// One run: the game in `game` (the USA release's code/content/meta) randomized into `out` (only the
// changed files: code/cking.rpx, content/..., meta/), the logs in <work>/logs. Null when done, else the
// randomizer's last errors. Once per process (RandoService).
JNI_FN(jstring, generate)(JNIEnv* env, jclass, jstring data, jstring work, jstring game, jstring out) {
    std::lock_guard<std::mutex> lk(g_mu);
    set_paths(jstr(env, data), jstr(env, work));
    g_progress = wwhd_rando::save_path / "progress";
    g_percent = 0;
    Config c;
    std::string err;
    if (!load(c, err)) return result(env, err);
    c.gameBaseDir = jstr(env, game);
    c.outputDir = jstr(env, out);
    // an output folder with code/, content/ and meta/meta.xml: the randomizer then writes only what it
    // changes instead of copying the whole game first
    std::error_code ec;
    for (const char* d : {"code", "content", "meta"}) std::filesystem::create_directories(c.outputDir / d, ec);
    std::filesystem::copy_file(c.gameBaseDir / "meta/meta.xml", c.outputDir / "meta/meta.xml",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return result(env, "cannot prepare " + c.outputDir.string() + ": " + ec.message());
    // the release (USA, EUR, JPN): its addresses, patches and language packs, before the worlds are made (item names
    // per language)
    if (!wwhd_rando::set_region(c.gameBaseDir, err)) return result(env, err);
    if (!std::filesystem::create_directories(Utility::get_logs_path(), ec) && ec) return result(env, "cannot make the log folder");
    wwhd_rando::progress_label("Randomizing...");
    int r = wwhdRandomize(c);
    if (r != 0) {
        std::string e = ErrorLog::getInstance().getLastErrors();
        return result(env, e.empty() ? "the randomizer failed" : e);
    }
    // an address the port didn't know was written to as it is: that game could break, so it isn't played
    if (wwhd_rando::port_failed()) return result(env, "the game's " + std::string(wwhd_rando::region_name()) + " addresses:\n" + wwhd_rando::port_errors());
    // what the app shows of this game: the seed hash (the words the game's title screen shows too) and
    // the permalink
    std::ofstream info(wwhd_rando::save_path / "last.txt");
    info << "release\t" << wwhd_rando::region_name() << "\nhash\t" << LogInfo::getSeedHash() << "\npermalink\t" << c.getPermalink() << "\nseed\t" << c.seed << "\n";
    g_percent = 100;
    write_progress("Done");
    return nullptr;
}
