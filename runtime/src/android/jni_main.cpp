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
#include "../disc/wua.h"
#include "../crash_info.h"
#include "../hud.h"
#include "link_icons.h"
#include "ui_icons.h"
#include "warp_catalog.h"
#include "../gx2/gx2.h"
#include "../input.h"
#include "../interp.h"
#include "../mods/climb.h"
#include "../mods/mods.h"
#include "../motion.h"
#include "../packs.h"
#include "../platform.h"
#include "../release.h"
#include "../guest_addr.h"
#include "../runtime.h"
#include "../savestate.h"
#include "../vk/lsfg.h"
#include "../vk/vk_window.h"
#include "jni_bridge.h"
#include "recomp_table.h"

namespace gfx {
int ao_mode();
void set_ao_mode(int m);
bool aniso_enabled();
void set_aniso(bool v);
bool vsync();
void set_vsync(bool v);
int upscale_mode();
void set_upscale_mode(int m);
bool ao_hires_enabled();
void set_ao_hires(bool v);
void request_capture();
}  // namespace gfx

void run_selftest();  // selftest.cpp

namespace {
JavaVM* g_vm = nullptr;
jclass g_native_class = nullptr;
jmethodID g_request_text = nullptr;
jmethodID g_rumble = nullptr, g_rumble_hold = nullptr;
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

void rumble(const uint8_t* pattern, int bits) {
    JNIEnv* env = env_for_thread();
    if (!env || !g_rumble) return;
    int bytes = bits > 0 && pattern ? (bits + 7) / 8 : 0;
    jbyteArray a = env->NewByteArray(bytes);
    if (bytes) env->SetByteArrayRegion(a, 0, bytes, (const jbyte*)pattern);
    env->CallStaticVoidMethod(g_native_class, g_rumble, a, (jint)(bytes ? bits : 0));
    env->DeleteLocalRef(a);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void rumble_hold(bool on) {
    JNIEnv* env = env_for_thread();
    if (!env || !g_rumble_hold) return;
    env->CallStaticVoidMethod(g_native_class, g_rumble_hold, (jboolean)on);
    if (env->ExceptionCheck()) env->ExceptionClear();
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
    g_rumble = env->GetStaticMethodID(c, "rumble", "([BI)V");
    g_rumble_hold = env->GetStaticMethodID(c, "rumbleHold", "(Z)V");
    return JNI_VERSION_1_6;
}

#define JNI_FN(ret, name) extern "C" JNIEXPORT ret JNICALL Java_org_wwhdrecomp_app_Native_##name

namespace gx2 { void set_clock_rate(double rate); }  // gx2_core.cpp

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
#ifdef WWHD_DEVICE_RECOMP
    // compiled on the device: any release with an address map (release.h)
    if (!release::known_entry(entry)) return env->NewStringUTF((path + " is not a supported version of the game").c_str());
#else
    if (entry != g_recomp_entry_point) return env->NewStringUTF((path + " does not match the recompiled code in this build").c_str());
#endif
    return nullptr;
}

// the game release in `gameDir`: "USA", "EUR", "JPN", or "" (none or unknown)
JNI_FN(jstring, gameRelease)(JNIEnv* env, jclass, jstring gameDir) {
    FILE* f = fopen((jstr(env, gameDir) + "/code/cking.rpx").c_str(), "rb");
    uint8_t h[0x1C] = {};
    if (f) {
        fread(h, 1, sizeof h, f);
        fclose(f);
    }
    uint32_t entry = (uint32_t)h[0x18] << 24 | h[0x19] << 16 | h[0x1A] << 8 | h[0x1B];
    const char* r = release::name_of_entry(entry);
    return env->NewStringUTF(r);
}

// the crash log's context from the app ("app": version, device, settings), see crash_info.h
JNI_FN(void, setCrashInfo)(JNIEnv* env, jclass, jstring section, jstring text) {
    crash_info::set(jstr(env, section), jstr(env, text));
}

// touch controls: the right side turns the camera by swiping (mods/camera.cpp's mouse camera)
namespace mods { extern std::atomic<bool> g_touch_swipe; }
JNI_FN(void, setTouchSwipeCamera)(JNIEnv*, jclass, jboolean on) {
    mods::g_touch_swipe = on;
    mods::set_mouse_camera(on);
}
// a swipe on the right side: movement in density-independent pixels (+y down)
JNI_FN(void, cameraSwipe)(JNIEnv*, jclass, jfloat dx, jfloat dy) { mods::mouse_add(dx, dy); }
// a mouse wheel (notches, + forward): Mods > First person on the mouse wheel
JNI_FN(void, mouseWheel)(JNIEnv*, jclass, jfloat dy) { mods::mouse_wheel(dy); }

// context-aware touch controls: the game's state (mods/touch_state.cpp) and the hidden HUD parts (hud.h)
// the item ring (ItemRing.java): its state, and an inventory slot put on X / Y / R
JNI_FN(jintArray, itemRing)(JNIEnv* env, jclass) {
    int32_t v[mods::kItemRingLen];
    mods::item_ring_state(v);
    jintArray a = env->NewIntArray(mods::kItemRingLen);
    env->SetIntArrayRegion(a, 0, mods::kItemRingLen, (const jint*)v);
    return a;
}
JNI_FN(void, equipItem)(JNIEnv*, jclass, jint button, jint slot) { mods::request_equip(button, slot); }
JNI_FN(void, conductWith)(JNIEnv*, jclass, jint button, jint slot) { mods::request_conduct(button, slot); }
JNI_FN(void, setGamePace)(JNIEnv*, jclass, jint mode) { mods::set_game_pace(mode); }
// the message window's box (hud.h hud_msg_window): {shown, x, y, half width, half height} in layout units
JNI_FN(jintArray, msgWindow)(JNIEnv* env, jclass) {
    int32_t v[5];
    hud_msg_window(v);
    jintArray a = env->NewIntArray(5);
    env->SetIntArrayRegion(a, 0, 5, (const jint*)v);
    return a;
}
// the wind compass after the Wind's Requiem (hud.h hud_wind_compass): {shown, x, y} in layout units
JNI_FN(jintArray, windCompass)(JNIEnv* env, jclass) {
    int32_t v[3] = {0, 0, 0};
    v[0] = hud_wind_compass(v + 1);
    jintArray a = env->NewIntArray(3);
    env->SetIntArrayRegion(a, 0, 3, (const jint*)v);
    return a;
}
// minigames played by touch (mods/minigames.cpp)
JNI_FN(jint, minigameMode)(JNIEnv*, jclass) { return mods::minigame_mode(); }
JNI_FN(void, minigameTouch)(JNIEnv*, jclass, jint action, jfloat x, jfloat y) { mods::minigame_touch(action, x, y); }
JNI_FN(jfloatArray, minigameMarks)(JNIEnv* env, jclass) {
    float v[2 * mods::kMinigameMarks];
    int n = mods::minigame_marks(v);
    jfloatArray a = env->NewFloatArray(2 * n);
    env->SetFloatArrayRegion(a, 0, 2 * n, v);
    return a;
}
JNI_FN(jintArray, songState)(JNIEnv* env, jclass) {
    int32_t v[mods::kSongStateLen];
    mods::song_state(v);
    jintArray a = env->NewIntArray(mods::kSongStateLen);
    env->SetIntArrayRegion(a, 0, mods::kSongStateLen, (const jint*)v);
    return a;
}
JNI_FN(void, playSong)(JNIEnv*, jclass, jint melody) { mods::play_song(melody); }
JNI_FN(void, countBeat)(JNIEnv*, jclass, jint time, jboolean last) { mods::count_beat(time, last); }
JNI_FN(jintArray, actionTexts)(JNIEnv* env, jclass) {
    int32_t v[mods::kActionCodes];
    bool ok = mods::action_texts(v);
    jintArray a = env->NewIntArray(ok ? mods::kActionCodes : 0);
    if (ok) env->SetIntArrayRegion(a, 0, mods::kActionCodes, (const jint*)v);
    return a;
}
JNI_FN(jintArray, touchState)(JNIEnv* env, jclass) {
    int32_t v[mods::kTouchStateLen];
    mods::touch_state(v);
    jintArray a = env->NewIntArray(mods::kTouchStateLen);
    env->SetIntArrayRegion(a, 0, mods::kTouchStateLen, (const jint*)v);
    return a;
}
JNI_FN(void, setHudHidden)(JNIEnv* env, jclass, jstring names) { hud_set_hidden(jstr(env, names)); }
JNI_FN(void, setSlotLabels)(JNIEnv* env, jclass, jstring labels) { hud_set_slot_labels(jstr(env, labels)); }
JNI_FN(void, setHudFade)(JNIEnv*, jclass, jint which, jboolean on) {
    if (which >= 0 && which < kFadeCount) hud_set_fade_auto(which, on);
}
JNI_FN(jboolean, extractUiFont)(JNIEnv* env, jclass, jstring gameDir, jstring outDir, jstring font) {
    return ui_icons::extract_font(jstr(env, gameDir), jstr(env, outDir), jstr(env, font));
}
JNI_FN(jboolean, extractUiTexts)(JNIEnv* env, jclass, jstring gameDir, jstring outFile, jstring layout) {
    return ui_icons::extract_texts(jstr(env, gameDir), jstr(env, outFile), jstr(env, layout));
}
JNI_FN(jint, dumpUiTextures)(JNIEnv* env, jclass, jstring gameDir, jstring outDir) {
    return ui_icons::dump_all(jstr(env, gameDir), jstr(env, outDir));
}
// Link in his own poses for the touch controls as outDir/link_*.rgba (link_icons.cpp): how many were written
JNI_FN(jint, renderLinkIcons)(JNIEnv* env, jclass, jstring gameDir, jstring outDir) {
    return link_icons::render_buttons(jstr(env, gameDir), jstr(env, outDir));
}
// textures from the game's 2D pack as outDir/<texture>.rgba (ui_icons.cpp): how many were written
JNI_FN(jint, extractUiTextures)(JNIEnv* env, jclass, jstring gameDir, jstring outDir, jobjectArray layouts, jobjectArray textures) {
    std::vector<ui_icons::Request> req;
    jsize n = env->GetArrayLength(layouts);
    for (jsize i = 0; i < n; i++) {
        auto l = (jstring)env->GetObjectArrayElement(layouts, i), t = (jstring)env->GetObjectArrayElement(textures, i);
        req.push_back({jstr(env, l), jstr(env, t)});
        env->DeleteLocalRef(l);
        env->DeleteLocalRef(t);
    }
    return ui_icons::extract(jstr(env, gameDir), jstr(env, outDir), req);
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
JNI_FN(jboolean, frameGenRiskyGpu)(JNIEnv*, jclass) { return gfx::fg::risky_gpu(); }
JNI_FN(void, setFrameGenRiskAccepted)(JNIEnv*, jclass, jboolean on) { gfx::fg::set_risk_accepted(on); }
JNI_FN(jstring, frameGenError)(JNIEnv* env, jclass) { return env->NewStringUTF(gfx::fg::last_error().c_str()); }

// GamePad motion: one sensor sample (SDL controller axes, rad/s and m/s²), or forget the state
JNI_FN(void, motionSample)(JNIEnv*, jclass, jfloat dt, jfloat gx, jfloat gy, jfloat gz, jfloat ax, jfloat ay, jfloat az) {
    motion::sample(dt, gx, gy, gz, ax, ay, az);
}
JNI_FN(void, motionReset)(JNIEnv*, jclass) { motion::reset(); }

// the running GPU driver ("" until the renderer has started)
JNI_FN(jstring, gpuDriverInfo)(JNIEnv* env, jclass) { return env->NewStringUTF(gfx::driver_info()); }
// the GPU's name as Vulkan reports it ("" until the renderer has started)
JNI_FN(jstring, gpuName)(JNIEnv* env, jclass) { return env->NewStringUTF(gfx::gpu_name()); }
JNI_FN(jboolean, gpuDriverFellBack)(JNIEnv*, jclass) { return gfx::driver_fallback(); }
JNI_FN(jlongArray, cacheProgress)(JNIEnv* env, jclass) {
    uint64_t p[4];
    gfx::cache_progress(p);
    jlongArray a = env->NewLongArray(4);
    jlong v[4] = {(jlong)p[0], (jlong)p[1], (jlong)p[2], (jlong)p[3]};
    env->SetLongArrayRegion(a, 0, 4, v);
    return a;
}

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
// the same for a Cemu .wua archive (decrypted: no keys)
JNI_FN(jstring, extractArchive)(JNIEnv* env, jclass, jint fd, jstring outDir) {
    g_extract_done = 0;
    g_extract_total = 0;
    g_extract_cancel = false;
    disc::Archive wua;
    std::string err;
    bool ok = wua.open(fd, err);
    if (ok) {
        LOG("[wua] title %s, %zu files", wua.title_id().c_str(), wua.files().size());
        ok = wua.extract(jstr(env, outDir), [](uint64_t done, uint64_t total, const std::string&) {
            g_extract_done = done;
            g_extract_total = total;
            return !g_extract_cancel.load();
        }, err);
    }
    close(fd);
    if (!ok) LOG("[wua] extraction failed: %s", err.c_str());
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
#include "../recomp/locals.h"
extern "C" const char wwhd_ops_bc[], wwhd_ops_bc_end[], wwhd_hooks[], wwhd_hooks_end[], wwhd_hot[], wwhd_hot_end[];  // recomp_data.S

#ifdef WWHD_DEVICE_RECOMP
#include "../recomp/loader.h"
#include "../aspect.h"
#endif

static std::atomic<size_t> g_compile_done{0}, g_compile_total{0};
static std::atomic<bool> g_compile_cancel{false};

// recompiles the game with LLVM on the device into `outDir`, logging time and memory; "" or why not
static std::string compile_code(const std::string& gameDir, const std::string& outDir, std::vector<std::string>& objects) {
    recomp::CompileOptions opt;
    opt.rpxPath = gameDir + "/code/cking.rpx";
    opt.opsBitcode.assign(wwhd_ops_bc, wwhd_ops_bc_end);
    opt.hooksText.assign(wwhd_hooks, wwhd_hooks_end);
    opt.hotText.assign(wwhd_hot, wwhd_hot_end);
    opt.outDir = outDir;
    opt.baseKey = recomp::code_base_key(opt.rpxPath);
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
    LOG("[recomp] compiling with %u jobs at -O%d", opt.jobs, opt.optLevel);
    recomp::CompileResult res;
    std::string err;
    bool ok = recomp::compile_game(opt, res, err);
    finished = true;
    progress.join();
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    objects = res.objects;
    LOG("[recomp] %s: %zu functions, %zu modules (%zu compiled now), %zu instructions in %.1f s, peak memory %ld MB; registers in locals: %zu "
        "functions, %zu kept in the struct (%s)%s%s", ok ? "done" : "FAILED", res.functions, res.modules, res.compiled, res.instructions, res.seconds,
        ru.ru_maxrss / 1024, res.localized, res.localFallback, recomp::code_switches().c_str(), ok ? "" : ": ", ok ? "" : err.c_str());
    return ok ? "" : err.empty() ? "failed" : err;
}
#endif

#ifdef WWHD_DEVICE_RECOMP
// ---- the game code, compiled on the device (runtime/src/recomp): the app checks the cache
// before starting and runs the compile on a Java thread with a progress screen

// true if the game code has to be compiled (first start, new APK or game files)
JNI_FN(jboolean, needsCompile)(JNIEnv* env, jclass, jstring gameDir, jstring codeDir) {
    packs::init(jstr(env, gameDir));  // mod packs change the code (packs.h)
    std::string key = recomp::code_cache_key(jstr(env, gameDir) + "/code/cking.rpx");
    return !recomp::code_cache_ready(jstr(env, codeDir), key);
}

// null when done, else why not ("cancelled" after compileCancel); resumes an interrupted compile
JNI_FN(jstring, compileGame)(JNIEnv* env, jclass, jstring gameDir, jstring codeDir) {
    std::string game = jstr(env, gameDir), dir = jstr(env, codeDir);
    packs::init(game);
    std::string key = recomp::code_cache_key(game + "/code/cking.rpx");
    if (key.empty()) return env->NewStringUTF("cannot read the game executable");
    recomp::begin_code_cache(dir);
    std::vector<std::string> objects;
    std::string err = compile_code(game, dir, objects);
    if (err.empty()) recomp::finish_code_cache(dir, key, objects, err);
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
        // (into a directory of its own: the code cache would already hold every module)
        std::string game = jstr(env, gameDir), dir = cache.substr(0, cache.find_last_of('/')) + "/codecache_test";
        std::thread([game, dir] {
            std::vector<std::string> objects;
            compile_code(game, dir, objects);
        }).detach();
        return;
    }
#endif
    config::game_dir = jstr(env, gameDir);
    config::save_dir = jstr(env, saveDir);
    config::cache_dir = jstr(env, cacheDir);
    config::code_dir = config::cache_dir.substr(0, config::cache_dir.find_last_of('/')) + "/codecache";
    // debug: the shader caches (records, SPIR-V, pipelines) in another folder, e.g. a fresh one to
    // measure a first start while the usual one stays as it is (the game code's cache stays)
    if (const char* e = getenv("WWHD_SHADER_CACHE_DIR"); e && *e) config::cache_dir = e;
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

// the GamePad's own display (a second screen): its surface, or null when it goes away
JNI_FN(void, drcSurfaceChanged)(JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    gfx::set_drc_window(w);
    if (w) ANativeWindow_release(w);  // the renderer holds its own reference
}

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

// the GamePad picture on a second display: its shape (0 = 16:9), and touches on it (u, v in 0..1 of
// the picture; aspect.cpp maps them to the game's 16:9 touch screen)
JNI_FN(void, setDrcAspect)(JNIEnv*, jclass, jfloat a) { aspect::set_drc_aspect(a); }
JNI_FN(void, setItemPageDrag)(JNIEnv*, jclass, jint mode, jfloat dx) { aspect::item_page_drag(mode, dx); }
JNI_FN(jboolean, itemPageGliding)(JNIEnv*, jclass) { return aspect::item_page_gliding(); }
JNI_FN(jfloat, itemPageX)(JNIEnv*, jclass) { return aspect::item_page_x(); }
JNI_FN(void, setItemSheetLeft)(JNIEnv*, jclass, jboolean on) { aspect::set_item_sheet_left(on); }
JNI_FN(float, drcPictureAspect)(JNIEnv*, jclass) { return gfx::drc_picture_aspect(); }
JNI_FN(void, setDrcTouch)(JNIEnv*, jclass, jboolean down, jfloat u, jfloat v) {
    float x = u, y = v;
    if (!aspect::drc_touch(u, v, gfx::drc_picture_aspect(), x, y)) down = false;  // empty space
    input::set_touch(down, x, y);
}

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

// settings shown in the app's menu
JNI_FN(void, setOption)(JNIEnv* env, jclass, jstring name, jint value) {
    std::string n = jstr(env, name);
    if (n != "capture") crash_info::option(n, value);
    if (n == "ao_mode") gfx::set_ao_mode(value);
    else if (n == "ao_hires") gfx::set_ao_hires(value != 0);
    else if (n == "aniso") gfx::set_aniso(value != 0);
    else if (n == "vsync") gfx::set_vsync(value != 0);
    else if (n == "trim_memory") gfx::trim_memory();
    else if (n == "upscale") gfx::set_upscale_mode(value);
    else if (n == "pro_controller") input::set_pro_controller(value != 0);
    else if (n == "invert_camera_y") mods::set_invert_y(value);
    else if (n == "steer_mode") mods::set_steer_mode(value);
    else if (n == "steer_swing") mods::set_steer_swing(value != 0);
    else if (n == "steer_swing_delay") mods::set_steer_swing_delay(value);
    else if (n == "steer_resync") mods::set_steer_resync(value);
    else if (n == "capture") gfx::request_capture();
    else if (n == "res_scale") gfx::set_resolution_scale(value / 100.0f);
    // dynamic resolution's lowest scale in percent, 0 = off (vk_dynres.cpp)
    else if (n == "dyn_res_min") {
        if (!getenv("WWHD_DYN_RES_MIN")) gfx::set_dynamic_resolution_min(value / 100.0f);  // debug: the env wins
    }
    else if (n == "tv_aspect") gfx::set_tv_aspect(value);
    // aspect ratio the game renders at (aspect.cpp): 0 = 16:9 (original), 1 = the screen's shape
    else if (n == "picto_big") aspect::set_picto_big(value != 0);
    else if (n == "ring_no_baton") mods::set_ring_no_baton(value != 0);
    // the 3D view zoomed out: the field of view's tangent in percent, 100 = the game's (aspect.cpp)
    else if (n == "view_zoom") aspect::set_view_zoom(value / 100.0f);
    // the 3D picture's centre moved down by layout units of 720 (the singing panel above it)
    else if (n == "view_shift") aspect::set_view_shift((float)value);
    else if (n == "render_aspect") {
        if (!getenv("WWHD_ASPECT")) aspect::set_mode(value == 1 ? aspect::kWindow : aspect::kOriginal);  // debug: the env wins
    }
    // frame interpolation (interp.cpp): 0 off, else the frame rate (60, 120)
    else if (n == "interp_fps") {
        if (value) interp::set_fps(value);
        interp::set_mode(value ? 1 : 0);
    }
    // gameplay mods (runtime/src/mods)
    else if (n == "mod_direct_camera") mods::set_direct_camera(value != 0);
    else if (n == "mod_camera_speed") mods::set_camera_speed(value / 100.0f);
    else if (n == "swipe_boat") mods::set_swipe_boat(value / 100.0f);
    else if (n == "mod_first_person") mods::set_first_person_wheel(value != 0);
    else if (n == "mod_climb") mods::set_climb_enabled(value != 0);
    else if (n == "mod_jump") mods::set_jump_anywhere(value != 0);
    else if (n == "mod_jump_double") mods::set_double_jump(value != 0);
    else if (n == "mod_jump_noauto") mods::set_no_auto_jump(value != 0);
    else if (n == "mod_quick_doors") mods::set_quick_doors(value != 0);
    else if (n == "mod_fast_scenes") mods::set_fast_scenes(value != 0);
    else if (n == "mod_ff_cutscenes") mods::set_ff_cutscenes(value != 0);
    else if (n == "mod_ff_dialogues") mods::set_ff_dialogues(value != 0);
    else if (n == "talk_hold") mods::set_talk_hold(value != 0);
    else if (n == "talk_hold_ff") mods::set_talk_hold_ff(value != 0);
    else if (n == "mod_song_replay") mods::set_song_replay(value != 0);
    else if (n == "mod_picto_pages") mods::set_picto_pages(value != 0);
    else if (n == "link_clothes") mods::set_link_clothes(value);
    else if (n == "postbox_mail") mods::set_postbox_mail(value != 0);
    else if (n == "postbox_same_day") mods::set_postbox_same_day(value != 0);
    else if (n == "postbox_present") mods::postbox_present();
    else if (n == "picto_page") mods::picto_page_request(value);
    else if (n == "mod_run_speed") mods::set_run_speed(value / 100.0f);
    else if (n == "mod_run_mode") mods::set_run_mode(value);
    else if (n == "mod_swim_mode") mods::set_swim_mode(value);
    else if (n == "mod_swim_speed") mods::set_swim_speed(value / 100.0f);
    else if (n == "mod_climb_speed") mods::set_climb_speed(value / 100.0f);
    else if (n == "mod_climb_mode") mods::set_climb_mode(value);
    else if (n == "mod_sidle_speed") mods::set_sidle_speed(value / 100.0f);
    else if (n == "mod_sidle_mode") mods::set_sidle_mode(value);
    // cheats (mods/cheats.cpp): a one-shot cheat's bit, applied once a save file is loaded
    else if (n == "cheat") mods::request_cheat(value);
    else if (n == "cheat_inf_health") mods::set_infinite(mods::kInfHealth, value != 0);
    else if (n == "cheat_inf_magic") mods::set_infinite(mods::kInfMagic, value != 0);
    else if (n == "cheat_inf_ammo") mods::set_infinite(mods::kInfAmmo, value != 0);
    else if (n == "cheat_inf_rupees") mods::set_infinite(mods::kInfRupees, value != 0);
    else if (n == "cheat_inf_bags") mods::set_infinite(mods::kInfBags, value != 0);
    else if (n == "cheat_inf_breath") mods::set_infinite(mods::kInfBreath, value != 0);
    else if (n == "cheat_one_hit") mods::set_infinite(mods::kOneHit, value != 0);
    else if (n == "cheat_moon_jump") mods::set_infinite(mods::kMoonJump, value != 0);
    else if (n == "cheat_moon_nofall") mods::set_infinite(mods::kMoonNoFall, value != 0);
    else if (n == "cheat_moon_anims") mods::set_infinite(mods::kMoonAllAnims, value != 0);
    else if (n == "cheat_moon_rise") mods::set_moon_jump_speeds(value, mods::moon_jump_fly());
    else if (n == "cheat_moon_fly") mods::set_moon_jump_speeds(mods::moon_jump_rise(), value);
    // auto save states (savestate.cpp): the interval in minutes, 0 off
    else if (n == "auto_state") ss::set_auto_interval(value);
    // the graphics benchmark (GraphicsPresets): GPU timing on, no sound, warps from the title screen
    else if (n == "benchmark") {
        gfx::set_benchmark(value != 0);
        audio::set_muted(value != 0);
        mods::set_warp_from_title(value != 0);
    }
    // the benchmark's measuring windows: the game's clock runs `value` times as fast (as fast forward
    // does), so the GPU works flat out at its full clocks and the frame rate shows what the device can do
    else if (n == "benchmark_quiet") gfx::set_background_compiles_paused(value != 0);
    else if (n == "benchmark_speed") {
        gx2::set_clock_rate(value > 1 ? value : 1.0);
        gfx::set_fast_forward(value > 1);
    }
}

// a mod pack for the menu (packs.h)
JNI_FN(jstring, packDescribe)(JNIEnv* env, jclass, jstring packDir, jstring selection, jstring gameDir) {
    return env->NewStringUTF(packs::describe(jstr(env, packDir), jstr(env, selection), jstr(env, gameDir)).c_str());
}

JNI_FN(jint, getOption)(JNIEnv* env, jclass, jstring name) {
    std::string n = jstr(env, name);
    if (n == "ao_mode") return gfx::ao_mode();
    if (n == "auto_state") return ss::auto_interval();
    if (n == "ao_hires") return gfx::ao_hires_enabled();
    if (n == "aniso") return gfx::aniso_enabled();
    if (n == "vsync") return gfx::vsync();
    if (n == "upscale") return gfx::upscale_mode();
    if (n == "pro_controller") return input::pro_controller();
    if (n == "invert_camera_y") return mods::invert_y();
    if (n == "steer_mode") return mods::steer_mode();
    if (n == "steer_swing") return mods::steer_swing();
    if (n == "tv_aspect") return gfx::tv_aspect();
    if (n == "bench_frames") return gfx::benchmark_frames();
    if (n == "work_done") return gfx::work_done();
    if (n == "boot_message") return hud_boot_message() ? 1 : 0;
    if (n == "album_screen") return hud_album_screen() ? 1 : 0;
    if (n == "dyn_res_min") return (int)lroundf(gfx::dynamic_resolution_min() * 100);
    if (n == "render_aspect") return aspect::mode() == aspect::kWindow ? 1 : 0;
    if (n == "interp_fps") return interp::mode() == 1 ? interp::fps() : 0;
    if (n == "mod_direct_camera") return mods::direct_camera();
    if (n == "mod_camera_speed") return (int)lroundf(mods::camera_speed() * 100);
    if (n == "swipe_boat") return (int)lroundf(mods::swipe_boat() * 100);
    if (n == "mod_first_person") return mods::first_person_wheel();
    if (n == "mod_climb") return mods::climb_enabled();
    if (n == "mod_jump") return mods::jump_anywhere();
    if (n == "mod_jump_double") return mods::double_jump();
    if (n == "mod_jump_noauto") return mods::no_auto_jump();
    if (n == "mod_quick_doors") return mods::quick_doors();
    if (n == "mod_fast_scenes") return mods::fast_scenes();
    if (n == "mod_ff_cutscenes") return mods::ff_cutscenes();
    if (n == "mod_ff_dialogues") return mods::ff_dialogues();
    if (n == "mod_picto_pages") return mods::picto_pages();
    if (n == "link_clothes") return mods::link_clothes();
    if (n == "postbox_mail") return mods::postbox_mail();
    if (n == "postbox_same_day") return mods::postbox_same_day();
    if (n == "postbox_notice") return mods::postbox_notice();
    if (n == "postbox_offer") return mods::postbox_offer();
    if (n == "postbox_near") return mods::postbox_near();
    if (n == "picto_page") return mods::picto_page_current();
    if (n == "picto_pages") return mods::picto_page_count();
    if (n == "picto_page_used") return mods::picto_page_used();
    if (n == "picto_subject") return mods::picto_subject();
    if (n == "picto_album") return mods::picto_album_shown();
    if (n == "mod_run_speed") return (int)lroundf(mods::run_speed() * 100);
    if (n == "mod_run_mode") return mods::run_mode();
    if (n == "mod_swim_mode") return mods::swim_mode();
    if (n == "mod_swim_speed") return (int)lroundf(mods::swim_speed() * 100);
    if (n == "mod_climb_mode") return mods::climb_mode();
    if (n == "mod_climb_speed") return (int)lroundf(mods::climb_speed() * 100);
    if (n == "mod_sidle_mode") return mods::sidle_mode();
    if (n == "mod_sidle_speed") return (int)lroundf(mods::sidle_speed() * 100);
    if (n == "cheat_inf_health") return mods::infinite(mods::kInfHealth);
    if (n == "cheat_inf_magic") return mods::infinite(mods::kInfMagic);
    if (n == "cheat_inf_ammo") return mods::infinite(mods::kInfAmmo);
    if (n == "cheat_inf_rupees") return mods::infinite(mods::kInfRupees);
    if (n == "cheat_inf_bags") return mods::infinite(mods::kInfBags);
    if (n == "cheat_inf_breath") return mods::infinite(mods::kInfBreath);
    if (n == "cheat_one_hit") return mods::infinite(mods::kOneHit);
    if (n == "cheat_moon_jump") return mods::infinite(mods::kMoonJump);
    if (n == "cheat_moon_nofall") return mods::infinite(mods::kMoonNoFall);
    if (n == "cheat_moon_anims") return mods::infinite(mods::kMoonAllAnims);
    if (n == "cheat_moon_rise") return mods::moon_jump_rise();
    if (n == "cheat_moon_fly") return mods::moon_jump_fly();
    return 0;
}

// ---- warp (mods/warp.cpp, developer menu): false when no save file is loaded
JNI_FN(jboolean, warp)(JNIEnv* env, jclass, jstring stage, jint room, jint spawn, jint layer) {
    return mods::request_warp(jstr(env, stage).c_str(), room, spawn, layer);
}
// the warp menu's places from the game files (warp_catalog.cpp): <gameDir>/warps.json; null when done,
// else why not. A few seconds (unpacks the stage archives); on a Java thread.
JNI_FN(jstring, buildWarps)(JNIEnv* env, jclass, jstring gameDir) {
    std::string dir = jstr(env, gameDir);
    auto t0 = std::chrono::steady_clock::now();
    int stages = 0, spawns = 0;
    std::string err = warp_catalog::build(dir, dir + "/warps.json", &stages, &spawns);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (err.empty()) LOG("[warp] catalog: %d stages, %d spawns in %.1f s", stages, spawns, s);
    else LOG("[warp] catalog failed after %.1f s: %s", s, err.c_str());
    return err.empty() ? nullptr : env->NewStringUTF(err.c_str());
}
// the same with a story preset (dSv_save_c bytes) written into the live save first
JNI_FN(jboolean, warpPreset)(JNIEnv* env, jclass, jbyteArray story, jstring stage, jint room, jint spawn, jint layer) {
    std::vector<uint8_t> b(env->GetArrayLength(story));
    env->GetByteArrayRegion(story, 0, (jsize)b.size(), (jbyte*)b.data());
    return mods::request_warp(jstr(env, stage).c_str(), room, spawn, layer, b.data(), b.size());
}
// the next warp puts Link at x, y, z facing angle (s16) once it has loaded
JNI_FN(void, warpPlace)(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z, jint angle) { mods::set_warp_place(x, y, z, angle); }
// the live save's story part (for a preset), null outside the game
JNI_FN(jbyteArray, storyBlock)(JNIEnv* env, jclass) {
    std::vector<uint8_t> b = mods::story_block();
    if (b.empty()) return nullptr;
    jbyteArray a = env->NewByteArray((jsize)b.size());
    env->SetByteArrayRegion(a, 0, (jsize)b.size(), (const jbyte*)b.data());
    return a;
}
// the latest warp's progress (mods::warp_status): {phase, ms, event mode}
JNI_FN(jlongArray, warpStatus)(JNIEnv* env, jclass) {
    int64_t v[3];
    mods::warp_status(v);
    jlong j[3] = {(jlong)v[0], (jlong)v[1], (jlong)v[2]};
    jlongArray a = env->NewLongArray(3);
    env->SetLongArrayRegion(a, 0, 3, j);
    return a;
}
// "stage,room,spawn,layer" of the current scene, "" outside the game
JNI_FN(jstring, warpWhere)(JNIEnv* env, jclass) { return env->NewStringUTF(mods::warp_where().c_str()); }

// ---- save states (runtime/src/savestate.cpp): 5 slots, 2 auto slots (6, 7) and the last session (8),
// the last 3 loaded only; saved and loaded at the next frame boundary
// {used, compatible, time, area, controller ("", "gamepad" or "pro")} of slot 1..8
JNI_FN(jobjectArray, saveSlotInfo)(JNIEnv* env, jclass, jint slot) {
    ss::SlotInfo i = ss::slot_info(slot);
    jobjectArray a = env->NewObjectArray(6, env->FindClass("java/lang/String"), nullptr);
    env->SetObjectArrayElement(a, 0, env->NewStringUTF(i.used ? "1" : "0"));
    env->SetObjectArrayElement(a, 1, env->NewStringUTF(i.compatible ? "1" : "0"));
    env->SetObjectArrayElement(a, 2, env->NewStringUTF(i.when.c_str()));
    env->SetObjectArrayElement(a, 3, env->NewStringUTF(i.area.c_str()));
    env->SetObjectArrayElement(a, 4, env->NewStringUTF(i.controller == 2 ? "pro" : i.controller == 1 ? "gamepad" : ""));
    env->SetObjectArrayElement(a, 5, env->NewStringUTF(std::to_string(i.created).c_str()));
    return a;
}
JNI_FN(void, saveState)(JNIEnv*, jclass, jint slot) { ss::request_save(slot); }
JNI_FN(void, loadState)(JNIEnv*, jclass, jint slot) { ss::request_load(slot); }
// the picker at start: armed before start(), released by "Start game" (or by the load)
JNI_FN(void, setStartHold)(JNIEnv*, jclass, jboolean on) { ss::set_start_hold(on); }
JNI_FN(jint, startHoldState)(JNIEnv*, jclass) { return ss::start_hold_state(); }
JNI_FN(jint, frontScreen)(JNIEnv*, jclass) { return g_started ? ss::front_screen() : 4; }
JNI_FN(jstring, drcTargets)(JNIEnv* env, jclass) { return env->NewStringUTF(g_started ? aspect::drc_targets().c_str() : ""); }
JNI_FN(jboolean, drcGameCursor)(JNIEnv*, jclass) { return g_started && aspect::drc_game_cursor(); }
// the world stops for a few frames: the play scene's hit-stop timer (dScnPly_ply_c::nextPauseTimer 101EACB7,
// taken into pauseTimer 101EACB6 by calcPauseTimer 025B02E8; actors and events wait while it runs). Not the
// menu's pause switch (dMenu_flag 101EA069): the game's own Options / Save states set that one and don't start
// while it is already on. Called each frame while wanted; it runs out by itself.
JNI_FN(void, setGamePaused)(JNIEnv*, jclass, jboolean on) {
    if (g_started && on) st8(GD(0x101EACB7), 3);
}
// the game's pause kind (101F8348, 0270E320): 0 while it plays, else one of its own pausing states runs
// (2 Options / Save on the GamePad, 4 "look at the GamePad")
JNI_FN(jint, pauseKind)(JNIEnv*, jclass) { return g_started ? (jint)ld32(GD(0x101F8348)) : 0; }

// the app goes to the background: the last session's state (returns once it is taken)
JNI_FN(void, saveStateOnLeave)(JNIEnv*, jclass) { if (g_started) ss::save_on_leave(); }
JNI_FN(jint, stateWritesPending)(JNIEnv*, jclass) { return ss::writes_pending(); }
// the latest save state result ("" when stale)
JNI_FN(jstring, saveStateMessage)(JNIEnv* env, jclass) { return env->NewStringUTF(ss::last_message().c_str()); }

// frame generation settings changed in the menu: applied from the next frame on
JNI_FN(void, applyFrameGen)(JNIEnv* env, jclass, jboolean on, jstring dll, jboolean quality, jfloat flowScale, jint multiplier,
                            jboolean uiDetection) {
    std::string path = dll ? jstr(env, dll) : std::string();
    gfx::request_frame_generation(on, path.c_str(), quality, flowScale, multiplier, uiDetection);
}

// performance overlay: {game fps, frame time avg ms, worst ms, presented fps, frame generation GPU ms,
// game frames so far, their time in seconds (without pauses), resolution scale, 3D view width, height,
// dynamic resolution's minimum (0 = off), its GPU busy share (-1 = off)}
// the graphics benchmark's numbers since the last call (gfx::benchmark_sample)
JNI_FN(jfloatArray, benchmarkSample)(JNIEnv* env, jclass) {
    float v[gfx::kBenchStats];
    gfx::benchmark_sample(v);
    jfloatArray a = env->NewFloatArray(gfx::kBenchStats);
    env->SetFloatArrayRegion(a, 0, gfx::kBenchStats, v);
    return a;
}

JNI_FN(jfloatArray, perfStats)(JNIEnv* env, jclass) {
    float v[gfx::kPerfStats];
    gfx::perf_stats(v);
    jfloatArray a = env->NewFloatArray(gfx::kPerfStats);
    env->SetFloatArrayRegion(a, 0, gfx::kPerfStats, v);
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
