// The game executable (RPX) as the recompiler sees it: code, symbols, relocations, imports, and
// the function boundaries found by analysis. A port of tools/rpx.py, tools/recomp/analyze.py and
// the analysis part of tools/recomp/recomp.py, for recompiling on the device.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

struct Import {
    std::string lib, name;  // e.g. "coreinit", "OSCreateThread"
    bool function;          // else data
    uint32_t dataAddr = 0;  // data imports: runtime-owned storage
};

struct Program {
    // loads and analyzes; false and `err` if the file isn't a usable RPX
    bool load(const std::string& path, std::string& err);
    bool load(const std::vector<uint8_t>& file, std::string& err);

    uint32_t entry = 0;  // ELF entry point
    uint32_t textLo = 0, textHi = 0;
    std::vector<uint32_t> words;  // .text, host order
    uint32_t word(uint32_t addr) const { return words[(addr - textLo) >> 2]; }
    bool inText(uint32_t a) const { return a >= textLo && a < textHi; }

    std::map<uint32_t, Import> imports;                   // slot address -> import
    std::map<uint32_t, uint32_t> importCalls;             // call site -> slot (REL24 into an import)
    std::set<uint32_t> undefCalls;                        // calls to $UNDEF
    std::map<uint32_t, uint16_t> immOverride;             // instruction -> relocated 16-bit immediate
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> jumpTables;  // bctr address -> (table, count)

    std::vector<uint32_t> entries;  // function entry points, sorted
    uint32_t funcOf(uint32_t a) const;   // entry of the function containing a (0 if none)
    uint32_t funcEnd(uint32_t start) const;  // next entry or the end of .text
    int fixpointRounds = 0;

private:
    bool parse(const std::vector<uint8_t>& f, std::string& err);
    void discover();
};

}  // namespace recomp
