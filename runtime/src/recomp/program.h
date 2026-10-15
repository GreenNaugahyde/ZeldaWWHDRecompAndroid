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

// Changes to the executable before analysis: Cemu graphic pack patches (runtime/src/packs.h). Byte
// writes into .text, and new code in a code cave (a second code region below .text) with the entries
// reached only through pointers.
struct CodePatch {
    struct Write {
        uint32_t addr;
        std::vector<uint8_t> bytes;
    };
    std::vector<Write> writes;  // anywhere; those in .text and the cave change the code
    uint32_t caveLo = 0, caveHi = 0;
    std::vector<uint32_t> entries;
    std::string key;  // for the code cache key ("" without patches)
};
// the patch Program::load applies (set once at startup, before the code is compiled or loaded)
void set_code_patch(const CodePatch* p);
const CodePatch* code_patch();

struct Program {
    const CodePatch* patch = code_patch();  // applied by load (nullptr: the executable as it is)

    // loads and analyzes; false and `err` if the file isn't a usable RPX
    bool load(const std::string& path, std::string& err);
    bool load(const std::vector<uint8_t>& file, std::string& err);

    uint32_t entry = 0;  // ELF entry point
    uint32_t textLo = 0, textHi = 0;
    std::vector<uint32_t> words;  // .text, host order
    uint32_t caveLo = 0, caveHi = 0;  // code cave (CodePatch), empty without one
    std::vector<uint32_t> caveWords;
    std::vector<bool> caveIsCode;  // per cave word: reached by control flow (functions end where data starts)
    uint32_t word(uint32_t addr) const { return addr >= textLo ? words[(addr - textLo) >> 2] : caveWords[(addr - caveLo) >> 2]; }
    bool inText(uint32_t a) const { return (a >= textLo && a < textHi) || (a >= caveLo && a < caveHi); }

    std::map<uint32_t, Import> imports;                   // slot address -> import
    std::map<uint32_t, uint32_t> importCalls;             // call site -> slot (REL24 into an import)
    std::set<uint32_t> undefCalls;                        // calls to $UNDEF
    std::map<uint32_t, uint16_t> immOverride;             // instruction -> relocated 16-bit immediate
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> jumpTables;  // bctr address -> (table, count)

    std::vector<uint32_t> entries;  // function entry points, sorted
    uint32_t funcOf(uint32_t a) const;   // entry of the function containing a (0 if none)
    uint32_t funcEnd(uint32_t start) const;  // next entry or the end of its code region
    int fixpointRounds = 0;

private:
    template <typename F>
    void for_each_word(F f) const {  // .text, then the code cave
        for (size_t i = 0; i < words.size(); i++) f(textLo + 4 * (uint32_t)i, words[i]);
        for (size_t i = 0; i < caveWords.size(); i++)
            if (caveIsCode[i]) f(caveLo + 4 * (uint32_t)i, caveWords[i]);
    }
    bool parse(const std::vector<uint8_t>& f, std::string& err);
    void discover();
};

}  // namespace recomp
