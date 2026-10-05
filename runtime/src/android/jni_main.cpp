// Android entry points (JNI, class org.wwhdrecomp.app.Native). The activity owns the UI thread;
// the runtime boots on a thread of its own and the game runs on its guest threads as on macOS.
#include <android/native_window_jni.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

#include "../audio_out.h"
#include "../disc/wud.h"
#include "../gx2/gx2.h"
#include "../input.h"
#include "../mods/climb.h"
#include "../mods/mods.h"
#include "../platform.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../vk/lsfg.h"
#include "../vk/vk_window.h"
#include "fps60.h"
#include "jni_bridge.h"
#include "touch_hud.h"
#include "ui_icons.h"
#include "../motion.h"
#include "recomp_table.h"

namespace gfx {
int ao_mode();
void set_ao_mode(int m);
bool aniso_enabled();
void set_aniso(bool v);
bool ao_hires_enabled();
void set_ao_hires(bool v);
void request_capture();
}  // namespace gfx

void run_selftest();  // selftest.cpp

namespace {
JavaVM* g_vm = nullptr;
jclass g_native_class = nullptr;
jmethodID g_request_text = nullptr;
std::atomic<bool> g_started{false};

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(s, c);
    return r;
}

// a JNIEnv for the calling thread (attached for the thread's lifetime)
JNIEnv* env_for_thread() {
    JNIEnv* env = nullptr;
    if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    static thread_local struct Detach {
        ~Detach() { g_vm->DetachCurrentThread(); }
    } detach;
    return env;
}
}  // namespace

namespace jni {
bool request_text_input(const std::u16string& initial, int maxLen) {
    JNIEnv* env = env_for_thread();
    if (!env || !g_request_text) return false;
    jstring s = env->NewString((const jchar*)initial.data(), (jsize)initial.size());
    env->CallStaticVoidMethod(g_native_class, g_request_text, s, (jint)maxLen);
    env->DeleteLocalRef(s);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return true;
}
}  // namespace jni

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass c = env->FindClass("org/wwhdrecomp/app/Native");
    if (!c) return JNI_ERR;
    g_native_class = (jclass)env->NewGlobalRef(c);
    g_request_text = env->GetStaticMethodID(c, "requestTextInput", "(Ljava/lang/String;I)V");
    return JNI_VERSION_1_6;
}

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_wwhdrecomp_app_Native_##name

// null if `gameDir` holds the executable this build was recompiled from, else a message
JNI_FN(jstring, checkGame)(JNIEnv* env, jclass, jstring gameDir) {
    std::string path = jstr(env, gameDir) + "/code/cking.rpx";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return env->NewStringUTF(("cannot open " + path).c_str());
    uint8_t h[0x1C];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n != sizeof h || memcmp(h, "\x7F" "ELF", 4) != 0) return env->NewStringUTF((path + " is not an RPX executable").c_str());
    uint32_t entry = (uint32_t)h[0x18] << 24 | h[0x19] << 16 | h[0x1A] << 8 | h[0x1B];
    if (entry != g_recomp_entry_point) return env->NewStringUTF((path + " does not match the recompiled code in this build").c_str());
    return nullptr;
}

// the licenses of everything in the APK (assembled by CMakeLists.txt)
extern "C" const char wwhd_licenses[], wwhd_licenses_end[];
JNI_FN(jstring, licenses)(JNIEnv* env, jclass) {
    return env->NewStringUTF(std::string(wwhd_licenses, wwhd_licenses_end).c_str());
}

// null if `path` is a Lossless.dll usable for frame generation, else why not
JNI_FN(jstring, checkFrameGenDll)(JNIEnv* env, jclass, jstring path) {
    std::string err = gfx::fg::check_dll(jstr(env, path));
    return err.empty() ? nullptr : env->NewStringUTF(err.c_str());
}

// true if `path` is a usable Lossless.dll whose shaders are a version this build was tested with
JNI_FN(jboolean, frameGenDllTested)(JNIEnv* env, jclass, jstring path) {
    bool tested = false;
    return gfx::fg::check_dll(jstr(env, path), &tested).empty() && tested;
}

// why frame generation couldn't start ("" if it runs or is off)
JNI_FN(jstring, frameGenError)(JNIEnv* env, jclass) { return env->NewStringUTF(gfx::fg::last_error().c_str()); }

// ---- extracting the game from the user's disc image (runtime/src/disc), on a Java thread
static std::atomic<uint64_t> g_extract_done{0}, g_extract_total{0};
static std::atomic<bool> g_extract_cancel{false};

// null when done, else why not. `fd` (the image, opened by the app) is closed here.
JNI_FN(jstring, extractGame)(JNIEnv* env, jclass, jint fd, jbyteArray discKey, jbyteArray commonKey, jstring outDir) {
    uint8_t dk[16], ck[16];
    if (env->GetArrayLength(discKey) != 16 || env->GetArrayLength(commonKey) != 16) {
        close(fd);
        return env->NewStringUTF("a key is not 16 bytes");
    }
    env->GetByteArrayRegion(discKey, 0, 16, (jbyte*)dk);
    env->GetByteArrayRegion(commonKey, 0, 16, (jbyte*)ck);
    g_extract_done = 0;
    g_extract_total = 0;
    g_extract_cancel = false;
    disc::Image img;
    std::string err;
    bool ok = img.open(fd, dk, ck, err);
    if (ok) {
        LOG("[disc] title %s, %zu files", img.title_id().c_str(), img.files().size());
        ok = img.extract(jstr(env, outDir), [](uint64_t done, uint64_t total, const std::string&) {
            g_extract_done = done;
            g_extract_total = total;
            return !g_extract_cancel.load();
        }, err);
    }
    close(fd);
    if (!ok) LOG("[disc] extraction failed: %s", err.c_str());
    return ok ? nullptr : env->NewStringUTF(err.c_str());
}
JNI_FN(jlongArray, extractProgress)(JNIEnv* env, jclass) {
    jlong v[2] = {(jlong)g_extract_done.load(), (jlong)g_extract_total.load()};
    jlongArray a = env->NewLongArray(2);
    env->SetLongArrayRegion(a, 0, 2, v);
    return a;
}
JNI_FN(void, extractCancel)(JNIEnv*, jclass) { g_extract_cancel = true; }
/** "16 raw bytes or 32 hex digits": the key, or null */
JNI_FN(jbyteArray, parseKey)(JNIEnv* env, jclass, jbyteArray data) {
    std::string s(env->GetArrayLength(data), '\0');
    env->GetByteArrayRegion(data, 0, (jsize)s.size(), (jbyte*)s.data());
    uint8_t k[16];
    if (!disc::parse_key(s, k)) return nullptr;
    jbyteArray a = env->NewByteArray(16);
    env->SetByteArrayRegion(a, 0, 16, (jbyte*)k);
    return a;
}

#ifdef WWHD_RECOMP
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include "../recomp/compile.h"
extern "C" const char wwhd_ops_bc[], wwhd_ops_bc_end[], wwhd_hooks[], wwhd_hooks_end[];  // recomp_data.S

#ifdef WWHD_DEVICE_RECOMP
#include "../recomp/loader.h"
#endif

static std::atomic<size_t> g_compile_done{0}, g_compile_total{0};
static std::atomic<bool> g_compile_cancel{false};

// recompiles the game with LLVM on the device into `outDir`, logging time and memory; "" or why not
static std::string compile_code(const std::string& gameDir, const std::string& outDir, bool resume) {
    recomp::CompileOptions opt;
    opt.rpxPath = gameDir + "/code/cking.rpx";
    opt.opsBitcode.assign(wwhd_ops_bc, wwhd_ops_bc_end);
    opt.hooksText.assign(wwhd_hooks, wwhd_hooks_end);
    opt.outDir = outDir;
    opt.keepExisting = resume;
    // about 500 MB per job; more than 4 barely help on big.LITTLE CPUs
    struct sysinfo si;
    unsigned long long ram = sysinfo(&si) == 0 ? (unsigned long long)si.totalram * si.mem_unit : 0;
    opt.jobs = ram >= (7ull << 30) ? 4 : ram >= (5ull << 30) ? 3 : 2;
    if (const char* j = getenv("WWHD_RECOMPILE_JOBS")) opt.jobs = (unsigned)atoi(j);
    if (const char* o = getenv("WWHD_RECOMPILE_OPT")) opt.optLevel = atoi(o);  // testing
    g_compile_done = 0;
    g_compile_total = 0;
    g_compile_cancel = false;
    opt.modulesDone = &g_compile_done;
    opt.modulesTotal = &g_compile_total;
    opt.cancel = &g_compile_cancel;
    std::atomic<bool> finished{false};
    std::thread progress([&] {
        for (int t = 0; !finished; t++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (t % 100 != 99) continue;
            struct rusage ru;
            getrusage(RUSAGE_SELF, &ru);
            LOG("[recomp] %zu of %zu modules done, peak memory %ld MB", g_compile_done.load(), g_compile_total.load(), ru.ru_maxrss / 1024);
        }
    });
    LOG("[recomp] compiling with %u jobs at -O%d%s", opt.jobs, opt.optLevel, resume ? ", resuming" : "");
    recomp::CompileResult res;
    std::string err;
    bool ok = recomp::compile_game(opt, res, err);
    finished = true;
    progress.join();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    LOG("[recomp] %s: %zu functions, %zu modules, %zu instructions in %.1f s, peak memory %ld MB%s%s", ok ? "done" : "FAILED",
        res.functions, res.modules, res.instructions, res.seconds, ru.ru_maxrss / 1024, ok ? "" : ": ", ok ? "" : err.c_str());
    return ok ? "" : err.empty() ? "failed" : err;
}
#endif

#ifdef WWHD_DEVICE_RECOMP
// ---- the game code, compiled on the device (runtime/src/recomp): the app checks the cache
// before starting and runs the compile on a Java thread with a progress screen

// true if the game code has to be compiled (first start, new APK or game files)
JNI_FN(jboolean, needsCompile)(JNIEnv* env, jclass, jstring gameDir, jstring codeDir) {
    std::string key = recomp::code_cache_key(jstr(env, gameDir) + "/code/cking.rpx");
    return !recomp::code_cache_ready(jstr(env, codeDir), key);
}

// null when done, else why not ("cancelled" after compileCancel); resumes an interrupted compile
JNI_FN(jstring, compileGame)(JNIEnv* env, jclass, jstring gameDir, jstring codeDir) {
    std::string game = jstr(env, gameDir), dir = jstr(env, codeDir);
    std::string key = recomp::code_cache_key(game + "/code/cking.rpx");
    if (key.empty()) return env->NewStringUTF("cannot read the game executable");
    bool resume = recomp::begin_code_cache(dir, key);
    std::string err = compile_code(game, dir, resume);
    if (err.empty()) recomp::finish_code_cache(dir, key, err);
    return err.empty() ? nullptr : env->NewStringUTF(err.c_str());
}

// {modules done, modules in total (0 until known)}
JNI_FN(jlongArray, compileProgress)(JNIEnv* env, jclass) {
    jlong v[2] = {(jlong)g_compile_done.load(), (jlong)g_compile_total.load()};
    jlongArray a = env->NewLongArray(2);
    env->SetLongArrayRegion(a, 0, 2, v);
    return a;
}

JNI_FN(void, compileCancel)(JNIEnv*, jclass) { g_compile_cancel = true; }
JNI_FN(jboolean, buildsGameCode)(JNIEnv*, jclass) { return true; }
#else
JNI_FN(jboolean, needsCompile)(JNIEnv*, jclass, jstring, jstring) { return false; }
JNI_FN(jboolean, buildsGameCode)(JNIEnv*, jclass) { return false; }
#endif

// boots the runtime and starts the game (once per process)
JNI_FN(void, start)(JNIEnv* env, jclass, jstring gameDir, jstring saveDir, jstring cacheDir, jstring workDir) {
    if (g_started.exchange(true)) return;
#ifdef WWHD_RECOMP
    if (getenv("WWHD_RECOMPILE_TEST")) {
        // measuring the first-start compile instead of starting the game
        std::string cache = jstr(env, cacheDir);
        std::thread(compile_code, jstr(env, gameDir), cache.substr(0, cache.find_last_of('/')) + "/codecache", false).detach();
        return;
    }
#endif
    config::game_dir = jstr(env, gameDir);
    config::save_dir = jstr(env, saveDir);
    config::cache_dir = jstr(env, cacheDir);
    config::code_dir = config::cache_dir.substr(0, config::cache_dir.find_last_of('/')) + "/codecache";
    std::string work = jstr(env, workDir);
    mkdir(config::save_dir.c_str(), 0755);
    mkdir(config::cache_dir.c_str(), 0755);
    // debug output (captures, dumps, trace files) goes to relative paths
    if (!work.empty()) {
        mkdir(work.c_str(), 0755);
        chdir(work.c_str());
    }
    if (getenv("WWHD_SELFTEST")) {  // renderer test without game files (selftest.cpp)
        std::thread([] {
            platform::set_thread_name("selftest");
            mem::init();
            gfx::init();
            run_selftest();
        }).detach();
        return;
    }
    std::thread([] {
        platform::set_thread_name("boot");
        LOG("[boot] game %s, saves %s, caches %s", config::game_dir.c_str(), config::save_dir.c_str(), config::cache_dir.c_str());
        boot_runtime();
        gfx::init();
        start_game_thread();
    }).detach();
}

JNI_FN(void, surfaceChanged)(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    gfx::set_window(w);
    if (w) ANativeWindow_release(w);  // the renderer holds its own reference
}

JNI_FN(void, surfaceDestroyed)(JNIEnv*, jclass) { gfx::set_window(nullptr); }

JNI_FN(void, setLayout)(JNIEnv* env, jclass, jfloatArray tv, jfloatArray drc, jboolean drcVisible) {
    gfx::ScreenRect t, d;
    if (tv && env->GetArrayLength(tv) >= 4) env->GetFloatArrayRegion(tv, 0, 4, &t.x);
    if (drc && env->GetArrayLength(drc) >= 4) env->GetFloatArrayRegion(drc, 0, 4, &d.x);
    gfx::set_layout(t, d, drcVisible);
}

JNI_FN(void, setPad)(JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
    input::set_pad((uint32_t)buttons, lx, ly, rx, ry);
}

JNI_FN(void, setTouch)(JNIEnv*, jclass, jboolean down, jfloat x, jfloat y) { input::set_touch(down, x, y); }

JNI_FN(void, textInputDone)(JNIEnv* env, jclass, jboolean ok, jstring text) {
    std::u16string t;
    if (text) {
        const jchar* c = env->GetStringChars(text, nullptr);
        t.assign((const char16_t*)c, env->GetStringLength(text));
        env->ReleaseStringChars(text, c);
    }
    input::prompt_finished(ok, t);
}

JNI_FN(void, setPaused)(JNIEnv*, jclass, jboolean paused) {
    audio::set_paused(paused);
    if (paused && g_started) gfx::save_caches();
}

// gyro aiming: the phone's sensors as the GamePad's (MainActivity)
JNI_FN(void, setMotion)(JNIEnv*, jclass, jfloat gx, jfloat gy, jfloat gz, jfloat ax, jfloat ay, jfloat az, jlong t) {
    motion::push(gx, gy, gz, ax, ay, az, t);
}
JNI_FN(void, setMotionEnabled)(JNIEnv*, jclass, jboolean on) { motion::set_enabled(on); }
JNI_FN(void, recalibrateMotion)(JNIEnv*, jclass) { motion::recalibrate(); }

// official artwork for the touch controls from the game files (IconForge)
JNI_FN(jint, extractUiTextures)(JNIEnv* env, jclass, jstring gameDir, jstring outDir, jobjectArray layouts, jobjectArray textures) {
    std::vector<ui_icons::Request> req;
    jsize n = env->GetArrayLength(textures);
    for (jsize i = 0; i < n; i++) {
        auto l = (jstring)env->GetObjectArrayElement(layouts, i), t = (jstring)env->GetObjectArrayElement(textures, i);
        req.push_back({jstr(env, l), jstr(env, t)});
        env->DeleteLocalRef(l);
        env->DeleteLocalRef(t);
    }
    return ui_icons::extract(jstr(env, gameDir), jstr(env, outDir), req);
}

// game state for the touch controls' icons (ControlsView)
JNI_FN(jintArray, hudState)(JNIEnv* env, jclass) {
    int32_t s[touch_hud::kSize];
    touch_hud::state(s);
    jintArray a = env->NewIntArray(touch_hud::kSize);
    env->SetIntArrayRegion(a, 0, touch_hud::kSize, s);
    return a;
}

// settings shown in the app's menu
JNI_FN(void, setOption)(JNIEnv* env, jclass, jstring name, jint value) {
    std::string n = jstr(env, name);
    if (n == "ao_mode") gfx::set_ao_mode(value);
    else if (n == "ao_hires") gfx::set_ao_hires(value != 0);
    else if (n == "aniso") gfx::set_aniso(value != 0);
    else if (n == "pro_controller") input::set_pro_controller(value != 0);
    else if (n == "capture") gfx::request_capture();
    else if (n == "res_scale") gfx::set_resolution_scale(value / 100.0f);
    else if (n == "tv_aspect") gfx::set_tv_aspect(value);
    else if (n == "fps_mode") fps60::set_mode(value);
    else if (n == "drawdone_mode") gx2::set_drawdone_mode(value);
    // gameplay mods (runtime/src/mods)
    else if (n == "mod_direct_camera") mods::set_direct_camera(value != 0);
    else if (n == "mod_camera_speed") mods::set_camera_speed(value / 100.0f);
    else if (n == "mod_first_person") mods::set_first_person_wheel(value != 0);
    else if (n == "mod_climb") mods::set_climb_enabled(value != 0);
    else if (n == "mod_quick_doors") mods::set_quick_doors(value != 0);
    else if (n == "mod_fast_scenes") mods::set_fast_scenes(value != 0);
}

JNI_FN(jint, getOption)(JNIEnv* env, jclass, jstring name) {
    std::string n = jstr(env, name);
    if (n == "ao_mode") return gfx::ao_mode();
    if (n == "ao_hires") return gfx::ao_hires_enabled();
    if (n == "aniso") return gfx::aniso_enabled();
    if (n == "pro_controller") return input::pro_controller();
    if (n == "tv_aspect") return gfx::tv_aspect();
    if (n == "fps_mode") return fps60::mode();
    if (n == "drawdone_mode") return gx2::drawdone_mode();
    if (n == "mod_direct_camera") return mods::direct_camera();
    if (n == "mod_camera_speed") return (int)lroundf(mods::camera_speed() * 100);
    if (n == "mod_first_person") return mods::first_person_wheel();
    if (n == "mod_climb") return mods::climb_enabled();
    if (n == "mod_quick_doors") return mods::quick_doors();
    if (n == "mod_fast_scenes") return mods::fast_scenes();
    return 0;
}

// ---- save states (runtime/src/savestate.cpp): 5 slots, saved and loaded at the next frame boundary
// {used, compatible, time, area} of slot 1..5
JNI_FN(jobjectArray, saveSlotInfo)(JNIEnv* env, jclass, jint slot) {
    ss::SlotInfo i = ss::slot_info(slot);
    jobjectArray a = env->NewObjectArray(4, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(a, 0, env->NewStringUTF(i.used ? "1" : "0"));
    env->SetObjectArrayElement(a, 1, env->NewStringUTF(i.compatible ? "1" : "0"));
    env->SetObjectArrayElement(a, 2, env->NewStringUTF(i.when.c_str()));
    env->SetObjectArrayElement(a, 3, env->NewStringUTF(i.area.c_str()));
    return a;
}
JNI_FN(void, saveState)(JNIEnv*, jclass, jint slot) { ss::request_save(slot); }
JNI_FN(void, loadState)(JNIEnv*, jclass, jint slot) { ss::request_load(slot); }
// the latest save state result ("" when stale)
JNI_FN(jstring, saveStateMessage)(JNIEnv* env, jclass) { return env->NewStringUTF(ss::last_message().c_str()); }

// frame generation settings changed in the menu: applied from the next frame on
JNI_FN(void, applyFrameGen)(JNIEnv* env, jclass, jboolean on, jstring dll, jboolean quality, jfloat flowScale, jint multiplier,
                            jboolean uiDetection) {
    std::string path = dll ? jstr(env, dll) : std::string();
    gfx::request_frame_generation(on, path.c_str(), quality, flowScale, multiplier, uiDetection);
}

// performance overlay: {game fps, frame time avg ms, worst ms, presented fps, frame generation GPU ms}
JNI_FN(jfloatArray, perfStats)(JNIEnv* env, jclass) {
    float v[5];
    gfx::perf_stats(v);
    jfloatArray a = env->NewFloatArray(5);
    env->SetFloatArrayRegion(a, 0, 5, v);
    return a;
}

// climb mod stamina wheel: {stamina 0..1, alpha 0..1 (0 = hidden), exhausted 0/1}
JNI_FN(jfloatArray, climbHud)(JNIEnv* env, jclass) {
    mods::ClimbHud h = mods::climb_enabled() ? mods::climb_hud() : mods::ClimbHud{0, 0, false};
    float v[3] = {h.stamina, h.alpha, h.exhausted ? 1.0f : 0.0f};
    jfloatArray a = env->NewFloatArray(3);
    env->SetFloatArrayRegion(a, 0, 3, v);
    return a;
}
