// The app's converter on the PC: gcmodel_host Link.arc permanent_3d.pack out.pack
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "gcmodel/gcmodel.h"

static std::vector<uint8_t> load(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: gcmodel_host Link.arc permanent_3d.pack out.pack\n");
        return 2;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> out;
    auto r = gcmodel::convert(load(argv[1]), load(argv[2]), out, [](int pc, const std::string& s) { fprintf(stderr, "\r%3d%% %-40s", pc, s.c_str()); });
    fprintf(stderr, "\n");
    printf("%s", r.report.c_str());
    if (!r.error.empty()) {
        fprintf(stderr, "ERROR: %s\n", r.error.c_str());
        return 1;
    }
    std::string err;
    const auto pack = gcmodel::assemble(load(argv[2]), out, err);
    if (pack.empty()) {
        fprintf(stderr, "ERROR: %s\n", err.c_str());
        return 1;
    }
    out = pack;
    std::ofstream(argv[3], std::ios::binary).write((const char*)out.data(), out.size());
    printf("written %zu bytes in %.1f s\n", out.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
