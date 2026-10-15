// Mod pack import (ModPacks.java): unpacking a zip or RAR archive with libarchive into a folder.
#include <archive.h>
#include <archive_entry.h>
#include <jni.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

namespace {

void make_dirs(const std::string& path) {
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

// "" when the entry name is unsafe (absolute, "..", empty parts)
std::string safe_name(std::string n) {
    for (char& c : n)
        if (c == '\\') c = '/';
    if (!n.empty() && n[0] == '/') return "";
    std::string out;
    size_t i = 0;
    while (i <= n.size()) {
        size_t e = n.find('/', i);
        if (e == std::string::npos) e = n.size();
        std::string part = n.substr(i, e - i);
        if (part == "..") return "";
        if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
        i = e + 1;
    }
    return out;
}

// null when done, else why not
std::string extract(int fd, const std::string& dest) {
    archive* a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_7zip(a);  // (stored and deflate entries; LZMA needs liblzma, not built in)
    archive_read_support_filter_all(a);
    std::string err;
    if (archive_read_open_fd(a, fd, 1 << 16) != ARCHIVE_OK) {
        err = std::string("not an archive this app can read (zip, RAR): ") + archive_error_string(a);
        archive_read_free(a);
        return err;
    }
    archive_entry* e;
    char buf[1 << 16];
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        if (archive_entry_is_encrypted(e)) {
            err = "the archive is password protected";
            break;
        }
        if (archive_entry_filetype(e) != AE_IFREG) continue;  // folders come with their files; no links
        std::string name = safe_name(archive_entry_pathname(e) ? archive_entry_pathname(e) : "");
        if (name.empty()) continue;
        std::string path = dest + "/" + name;
        make_dirs(path);
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            err = "cannot write " + path;
            break;
        }
        la_ssize_t n;
        while ((n = archive_read_data(a, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)n, f);
        fclose(f);
        if (n < 0) {
            err = std::string(name) + ": " + archive_error_string(a);
            break;
        }
    }
    if (err.empty() && r != ARCHIVE_EOF) err = std::string("damaged archive: ") + archive_error_string(a);
    archive_read_free(a);
    return err;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_wwhdrecomp_app_Native_extractModArchive(JNIEnv* env, jclass, jint fd, jstring destDir) {
    const char* d = env->GetStringUTFChars(destDir, nullptr);
    std::string dest = d;
    env->ReleaseStringUTFChars(destDir, d);
    mkdir(dest.c_str(), 0755);
    int own = dup(fd);  // libarchive reads it; the caller keeps its own
    std::string err = extract(own, dest);
    close(own);
    return err.empty() ? nullptr : env->NewStringUTF(err.c_str());
}
