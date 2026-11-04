// The mod pack import (ModPacks.java) for GameCube player models (runtime/src/gcmodel): Link.arc -> a converted
// Link.szs kept in the model's pack; the game's permanent_3d.pack assembled with it for the model switched on.
#include <jni.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "gcmodel/gcmodel.h"

namespace {
std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c;
    env->ReleaseStringUTFChars(s, c);
    return r;
}

bool load(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// written beside it first, then moved over: a file the game reads is never half written
bool save(const std::string& p, const std::vector<uint8_t>& d) {
    const std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f.write((const char*)d.data(), d.size())) return false;
    }
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}
}  // namespace

// Link.arc -> Link.szs at outPath: the report (a line per part and texture) when done, "!" and why not otherwise.
// Native.gcProgress(percent) now and then (on this thread).
extern "C" JNIEXPORT jstring JNICALL Java_org_wwhdrecomp_app_Native_convertGcModel(JNIEnv* env, jclass cls, jstring arcPath, jstring packPath,
                                                                                  jstring outPath) {
    std::vector<uint8_t> arc, pack, out;
    if (!load(jstr(env, arcPath), arc)) return env->NewStringUTF("!cannot read Link.arc");
    if (!load(jstr(env, packPath), pack)) return env->NewStringUTF("!cannot read the game's permanent_3d.pack");
    jmethodID progress = env->GetStaticMethodID(cls, "gcProgress", "(I)V");
    gcmodel::Result r = gcmodel::convert(arc, pack, out, [&](int pc, const std::string&) {
        if (progress) env->CallStaticVoidMethod(cls, progress, (jint)pc);
    });
    if (!r.error.empty()) return env->NewStringUTF(("!" + r.error).c_str());
    if (!save(jstr(env, outPath), out)) return env->NewStringUTF("!cannot write the converted model");
    return env->NewStringUTF(r.report.c_str());
}

// the game's permanent_3d.pack with a converted Link.szs, at outPath: null when done, else why not
extern "C" JNIEXPORT jstring JNICALL Java_org_wwhdrecomp_app_Native_assembleGcModel(JNIEnv* env, jclass, jstring packPath, jstring szsPath,
                                                                                   jstring outPath) {
    std::vector<uint8_t> pack, szs;
    if (!load(jstr(env, packPath), pack)) return env->NewStringUTF("cannot read the game's permanent_3d.pack");
    if (!load(jstr(env, szsPath), szs)) return env->NewStringUTF("cannot read the converted model");
    std::string err;
    const auto out = gcmodel::assemble(pack, szs, err);
    if (out.empty()) return env->NewStringUTF(err.c_str());
    if (!save(jstr(env, outPath), out)) return env->NewStringUTF("cannot write permanent_3d.pack");
    return nullptr;
}

// Link.szs out of a converted permanent_3d.pack (model packs made before they kept only that): true when written
extern "C" JNIEXPORT jboolean JNICALL Java_org_wwhdrecomp_app_Native_extractGcModel(JNIEnv* env, jclass, jstring packPath, jstring outPath) {
    std::vector<uint8_t> pack;
    if (!load(jstr(env, packPath), pack)) return false;
    const auto szs = gcmodel::link_szs(pack);
    return !szs.empty() && save(jstr(env, outPath), szs);
}
