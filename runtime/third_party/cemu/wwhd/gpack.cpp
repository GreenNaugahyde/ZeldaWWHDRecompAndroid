// Cemu graphic packs for the recompiled game (see gpack.h). Adapted from Cemu (MPL-2.0):
// util/IniParser (rules.txt syntax), Cafe/GraphicPack/GraphicPack2.cpp (presets and their variables),
// GraphicPack2PatchesParser.cpp (patch_*.asm syntax), GraphicPack2PatchesApply.cpp (symbol resolution,
// relocations) and Cafe/OS/RPL/rpl.cpp (the module checksum moduleMatches compares).
#include "wwhd/gpack.h"

#include <strings.h>
#include <zlib.h>

#include "Cemu/ExpressionParser/ExpressionParser.h"
#include "Cemu/PPCAssembler/ppcAssembler.h"
#include "util/helpers/StringParser.h"

namespace gpack {
namespace {

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

void trim(std::string_view& s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
}

std::vector<std::string_view> lines_of(std::string_view text) {
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) text.remove_prefix(3);
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i <= text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        std::string_view l = text.substr(i, e - i);
        if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
        out.push_back(l);
        i = e + 1;
    }
    return out;
}

// ---- rules.txt (IniParser)
struct IniSection {
    std::string name;
    std::vector<std::pair<std::string, std::string>> options;
    const std::string* find(std::string_view n) const {
        for (auto& [k, v] : options)
            if (iequals(k, n)) return &v;
        return nullptr;
    }
};

std::vector<IniSection> parse_ini(std::string_view text) {
    std::vector<IniSection> secs;
    for (std::string_view line : lines_of(text)) {
        trim(line);
        if (line.empty()) continue;
        bool quote = false;
        for (size_t i = 0; i < line.size(); i++) {
            if (line[i] == '"') quote = !quote;
            if ((line[i] == '#' || line[i] == ';') && !quote) {
                line = line.substr(0, i);
                break;
            }
        }
        if (line.empty()) continue;
        if (line[0] == '[') {
            size_t e = line.find(']');
            std::string_view n = line.substr(1, e == std::string_view::npos ? std::string_view::npos : e - 1);
            secs.push_back({std::string(n), {}});
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string_view::npos || secs.empty()) continue;
        std::string_view k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k);
        trim(v);
        if (k.empty()) continue;
        if (!v.empty() && v.front() == '"') {
            v.remove_prefix(1);
            if (!v.empty() && v.back() == '"') v.remove_suffix(1);
        }
        secs.back().options.emplace_back(std::string(k), std::string(v));
    }
    return secs;
}

// "$name:int" -> "$name"
std::string var_name(std::string n) {
    size_t c = n.find(':');
    if (c != std::string::npos) n = n.substr(0, c);
    std::string_view v = n;
    trim(v);
    return std::string(v);
}

std::vector<std::pair<std::string, std::string>> vars_of(const IniSection& s) {
    std::vector<std::pair<std::string, std::string>> out;
    for (auto& [k, v] : s.options)
        if (!k.empty() && k[0] == '$') out.emplace_back(var_name(k), v);
    return out;
}

}  // namespace

bool parse_rules(const std::string& text, Rules& out, std::string& err) {
    out = {};
    auto secs = parse_ini(text);
    const IniSection* def = nullptr;
    for (auto& s : secs) {
        out.sections.push_back(s.name);
        if (iequals(s.name, "Definition") && !def) def = &s;
    }
    if (!def) return err = "rules.txt has no [Definition] section", false;
    if (auto* v = def->find("name")) out.name = *v;
    if (auto* v = def->find("path")) out.path = *v;
    if (auto* v = def->find("description")) out.description = *v;
    if (auto* v = def->find("version")) out.version = atoi(v->c_str());
    if (auto* v = def->find("titleIds")) {
        std::string_view s = *v;
        while (!s.empty()) {
            size_t c = s.find(',');
            std::string_view id = s.substr(0, c);
            trim(id);
            if (!id.empty()) out.titleIds.push_back(strtoull(std::string(id).c_str(), nullptr, 16));
            if (c == std::string_view::npos) break;
            s.remove_prefix(c + 1);
        }
    }
    for (auto& s : secs)
        for (auto& [k, v] : s.options)
            if (!k.empty() && k[0] == '$') {
                size_t c = k.find(':');
                if (c != std::string::npos) {
                    std::string_view t = std::string_view(k).substr(c + 1);
                    trim(t);
                    if (t == "int" && std::find(out.intVars.begin(), out.intVars.end(), var_name(k)) == out.intVars.end())
                        out.intVars.push_back(var_name(k));
                }
            }
    for (auto& s : secs) {
        if (iequals(s.name, "Default")) {
            out.defaults = vars_of(s);
        } else if (iequals(s.name, "Preset")) {
            Preset p;
            auto* n = s.find("name");
            if (!n) continue;
            p.name = *n;
            if (auto* c = s.find("category")) p.category = *c;
            if (auto* c = s.find("condition")) p.condition = *c;
            if (auto* d = s.find("default")) p.isDefault = atoi(d->c_str()) != 0 || iequals(*d, "true");
            p.vars = vars_of(s);
            out.presets.push_back(std::move(p));
        }
    }
    return true;
}

std::vector<std::string> categories(const Rules& r) {
    std::vector<std::string> out;
    for (auto& p : r.presets)
        if (std::find(out.begin(), out.end(), p.category) == out.end()) out.push_back(p.category);
    return out;
}

Resolved resolve(const Rules& r, const std::map<std::string, std::string>& selection) {
    Resolved res;
    auto evaluate = [](const std::string& expr, const std::map<std::string, double>& consts, double& v) {
        try {
            ExpressionParser ep;
            for (auto& [k, x] : consts) ep.AddConstant(k, x);
            v = ep.Evaluate(expr);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    std::map<std::string, double> defaults;
    for (auto& [k, e] : r.defaults) {
        double v = 0;
        evaluate(e, defaults, v);
        defaults[k] = v;
    }
    std::vector<std::string> cats = categories(r);
    std::map<std::string, std::string> active;
    // the selection (or the defaults) first, then visibility by the conditions with those values,
    // then each category's active preset re-checked against its visible ones (Cemu's
    // ValidatePresetSelections; two rounds settle the usual "aspect ratio -> resolution" chains)
    for (auto& c : cats) {
        auto it = selection.find(c);
        const Preset* pick = nullptr;
        for (auto& p : r.presets)
            if (p.category == c && it != selection.end() && p.name == it->second) pick = &p;
        if (!pick)
            for (auto& p : r.presets)
                if (p.category == c && (p.isDefault || !pick)) {
                    if (p.isDefault || !pick) pick = &p;
                    if (p.isDefault) break;
                }
        if (pick) active[c] = pick->name;
    }
    auto presetOf = [&](const std::string& c, const std::string& n) -> const Preset* {
        for (auto& p : r.presets)
            if (p.category == c && p.name == n) return &p;
        return nullptr;
    };
    auto constants = [&]() {
        std::map<std::string, double> m = defaults;
        for (auto& [c, n] : active)
            if (const Preset* p = presetOf(c, n))
                for (auto& [k, e] : p->vars) {
                    double v = 0;
                    if (evaluate(e, m, v)) m[k] = v;
                }
        return m;
    };
    auto visible = [&](const Preset& p, const std::map<std::string, double>& m) {
        if (p.condition.empty() || r.version < 5) return true;
        double v = 0;
        return evaluate(p.condition, m, v) && v != 0;
    };
    for (int round = 0; round < 2; round++) {
        auto m = constants();
        for (auto& c : cats) {
            const Preset* cur = presetOf(c, active[c]);
            if (cur && visible(*cur, m)) continue;
            const Preset* pick = nullptr;
            for (auto& p : r.presets)
                if (p.category == c && visible(p, m) && (!pick || (p.isDefault && !pick->isDefault))) pick = &p;
            if (pick) active[c] = pick->name;
        }
    }
    auto m = constants();
    for (auto& c : cats) {
        std::vector<std::string> names;
        for (auto& p : r.presets)
            if (p.category == c && visible(p, m)) names.push_back(p.name);
        res.visible.emplace_back(c, names);
    }
    // variables: active visible presets, then active hidden ones, then [Default]
    res.vars = defaults;
    for (int pass = 1; pass >= 0; pass--)
        for (auto& [c, n] : active)
            if (const Preset* p = presetOf(c, n))
                if (visible(*p, m) == (pass == 0))
                    for (auto& [k, e] : p->vars) {
                        double v = 0;
                        if (evaluate(e, res.vars, v)) res.vars[k] = v;
                    }
    res.active = active;
    return res;
}

std::vector<TextureRule> texture_rules(const std::string& text, const std::map<std::string, double>& vars) {
    std::vector<TextureRule> out;
    auto eval = [&](const std::string& expr, double& v) {
        try {
            ExpressionParser ep;
            for (auto& [k, x] : vars) ep.AddConstant(k, x);
            v = ep.Evaluate(expr);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    for (auto& s : parse_ini(text)) {
        if (!iequals(s.name, "TextureRedefine")) continue;
        TextureRule r;
        auto num = [&](const char* name, int& out, double scale = 1.0) {
            double v;
            if (const std::string* o = s.find(name); o && eval(*o, v)) {
                out = (int)(v * scale);
                return true;
            }
            return false;
        };
        auto list = [&](const char* name, std::vector<int>& out) {
            if (const std::string* o = s.find(name)) {
                std::string_view l = *o;
                while (!l.empty()) {
                    size_t c = l.find(',');
                    std::string_view item = l.substr(0, c);
                    trim(item);
                    double v;
                    if (!item.empty() && eval(std::string(item), v)) out.push_back((int)v);
                    if (c == std::string_view::npos) break;
                    l.remove_prefix(c + 1);
                }
            }
        };
        num("width", r.width);
        num("height", r.height);
        num("depth", r.depth);
        if (const std::string* o = s.find("inMEM1")) r.inMem1 = (iequals(*o, "true") || *o == "1") ? 1 : 2;
        list("formats", r.formats);
        list("formatsExcluded", r.formatsExcluded);
        list("tilemodes", r.tileModes);
        list("tilemodesExcluded", r.tileModesExcluded);
        num("overwriteWidth", r.overwriteWidth);
        num("overwriteHeight", r.overwriteHeight);
        num("overwriteDepth", r.overwriteDepth);
        num("overwriteFormat", r.overwriteFormat);
        num("overwriteLodBias", r.lodBias, 64.0);
        num("overwriteRelativeLodBias", r.relativeLodBias, 64.0);
        int aniso;
        if (num("overwriteAnisotropy", aniso))  // only 1, 2, 4, 8, 16 (Cemu)
            r.anisotropy = aniso == 1 ? 0 : aniso == 2 ? 1 : aniso == 4 ? 2 : aniso == 8 ? 3 : aniso == 16 ? 4 : -1;
        out.push_back(std::move(r));
    }
    return out;
}

// ---- the module checksum (rpl.cpp, RPLLoader_BeginCemuhookCRC): little-endian fields as Cemu hashes them
uint32_t module_crc(const std::vector<uint8_t>& d) {
    auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
    auto be16 = [&](size_t o) { return (uint16_t)(d[o] << 8 | d[o + 1]); };
    if (d.size() < 0x34) return 0;
    uint32_t shoff = be32(0x20);
    uint16_t shentsize = be16(0x2E), shnum = be16(0x30), shstrndx = be16(0x32);
    int32_t symtab = -1, strtab = -1;
    for (int i = 0; i < shnum; i++) {
        uint32_t type = be32(shoff + i * shentsize + 4);
        if (type == 2) symtab = i;
        if (type == 3 && i != shstrndx && strtab == -1) strtab = i;
    }
    uLong c = crc32(0, (const Bytef*)"\x7FRPX", 4);
    auto add = [&](uint32_t v) { c = crc32(c, (const Bytef*)&v, 4); };  // host order (little endian), as Cemu
    add(shnum);
    add((uint32_t)symtab);
    add((uint32_t)strtab);
    add(shstrndx);
    for (int i = 0; i < shnum; i++) {
        size_t o = shoff + i * shentsize;
        uint32_t name = be32(o), type = be32(o + 4), flags = be32(o + 8), addr = be32(o + 12), off = be32(o + 16), size = be32(o + 20),
                 align = be32(o + 32);
        std::vector<uint8_t> raw;
        uint32_t rawSize = size;
        const uint8_t* data = nullptr;
        if (type == 8) {
        } else if (flags & 0x08000000) {
            uLongf n = be32(off);
            raw.resize(n);
            if (uncompress(raw.data(), &n, &d[off + 4], size - 4) != Z_OK) continue;
            rawSize = (uint32_t)n;
            data = raw.data();
        } else {
            data = &d[off];
        }
        add(name), add(type), add(flags), add(addr), add(rawSize), add(align);
        if (data && rawSize) c = crc32(c, data, rawSize);
    }
    return (uint32_t)c;
}

Module module_info(const std::vector<uint8_t>& d) {
    Module m;
    m.crc = module_crc(d);
    auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
    if (d.size() < 0x34) return m;
    uint32_t shoff = be32(0x20);
    uint16_t shentsize = (uint16_t)(d[0x2E] << 8 | d[0x2F]), shnum = (uint16_t)(d[0x30] << 8 | d[0x31]);
    for (int i = 0; i < shnum; i++) {
        size_t o = shoff + i * shentsize;
        uint32_t type = be32(o + 4), flags = be32(o + 8), addr = be32(o + 12), size = be32(o + 20);
        if (!(flags & 2) || !addr) continue;  // allocated sections
        if (type != 8 && (flags & 0x08000000)) size = be32(be32(o + 16));
        m.sections.push_back({addr, size});
    }
    return m;
}

// ---- patch files (GraphicPack2PatchesParser.cpp, ParseCemuPatchesTxtInternal) and their resolution
// (GraphicPack2PatchesApply.cpp)
namespace {

constexpr uint32_t kCodeCaveMaxOffset = 8 * 1024 * 1024;
constexpr uint32_t kInvalidOrigin = 0xFFFFFFFF;

struct Entry {
    enum Kind { Label, Variable, Instruction, CemuhookSymbol } kind;  // CemuhookSymbol: a relocated address
    int line;
    std::string name, expr;   // Label / Variable
    uint32_t addr = 0;        // Label, Instruction: unrelocated (code cave offset or module address)
    std::vector<uint8_t> data;  // Instruction
    bool code = false;          // Label: an instruction follows (not a data directive)
    std::vector<PPCAssemblerReloc> relocs;
    uint32_t relocated = 0;
    bool addrDone = false;
};

struct Group {
    std::string name;
    std::vector<uint32_t> matches;
    bool rpxOnly = false;
    std::vector<Entry> entries;
    std::vector<std::string> callbacks;
    uint32_t caveSize = 0, caveBase = 0;
};

int comment_start(std::string_view s) {
    bool q = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '"') q = !q;
        else if ((s[i] == '#' || s[i] == ';') && !q) return (int)i;
    }
    return (int)s.size();
}

// Cemuhook's patches.txt (ParseCemuhookPatchesTxtInternal): groups, moduleMatches, "0xADDR = <one
// instruction or data directive>" and "<symbol> = <address expression>"; no .origin, labels or callbacks
bool parse_cemuhook_file(std::string_view text, const std::string& file, std::vector<Group>& groups, std::vector<std::string>& errors) {
    Group* g = nullptr;
    auto fail = [&](int line, const std::string& msg) {
        errors.push_back(fmt::format("{} line {}: {}", file, line, msg));
        return false;
    };
    auto finishGroup = [&]() {
        if (!g) return;
        uint32_t maxAddr = 0;
        for (auto& e : g->entries)
            if (e.kind == Entry::Instruction && e.addr < kCodeCaveMaxOffset) maxAddr = std::max(maxAddr, e.addr + (uint32_t)e.data.size());
        g->caveSize = maxAddr;
    };
    int lineNo = 0;
    for (std::string_view raw : lines_of(text)) {
        lineNo++;
        std::string lineStr(raw.substr(0, comment_start(raw)));
        StringTokenParser parser(lineStr.data(), (sint32)lineStr.size());
        parser.skipWhitespaces();
        if (parser.isEndOfString()) continue;
        if (parser.compareCharacter(0, '[')) {
            parser.skipCharacters(1);
            const char* n = parser.getCurrentPtr();
            sint32 len = parser.skipToCharacter(']');
            if (len < 0) return fail(lineNo, "Expected ']'");
            finishGroup();
            groups.push_back({});
            g = &groups.back();
            g->name.assign(n, len);
        } else if (parser.compareCharacter(0, '0') && parser.compareCharacterI(1, 'x')) {
            uint32 a;
            if (!parser.parseU32(a)) return fail(lineNo, "Malformed address");
            if (!parser.matchWordI("=")) return fail(lineNo, "Expected '=' after address");
            if (!g) return fail(lineNo, "Line outside of a group");
            parser.skipWhitespaces();
            parser.trimWhitespaces();
            std::string instr(parser.getCurrentPtr(), parser.getCurrentLen());
            PPCAssemblerInOut ctx{};
            ctx.virtualAddress = a;
            if (!ppcAssembler_assembleSingleInstruction(instr.c_str(), &ctx)) return fail(lineNo, "Assembler: " + ctx.errorMsg);
            Entry e{Entry::Instruction, lineNo};
            e.addr = a;
            e.data.assign(ctx.outputData.begin(), ctx.outputData.end());
            e.relocs = ctx.list_relocs;
            g->entries.push_back(std::move(e));
        } else if (parser.matchWordI("moduleMatches")) {
            if (!g) return fail(lineNo, "moduleMatches outside of a group");
            if (!parser.matchWordI("=")) return fail(lineNo, "Expected '=' after moduleMatches");
            for (;;) {
                uint32 crc = 0;
                if (!parser.parseU32(crc)) return fail(lineNo, "Invalid value for moduleMatches");
                g->matches.push_back(crc);
                if (!parser.matchWordI(",")) break;
            }
        } else {
            const char* sym;
            sint32 symLen;
            if (!g) return fail(lineNo, "Line outside of a group");
            if (!(parser.parseSymbolName(sym, symLen) && parser.matchWordI("="))) return fail(lineNo, "Invalid syntax");
            parser.skipWhitespaces();
            parser.trimWhitespaces();
            Entry e{Entry::CemuhookSymbol, lineNo, std::string(sym, symLen), std::string(parser.getCurrentPtr(), parser.getCurrentLen())};
            g->entries.push_back(std::move(e));
        }
    }
    finishGroup();
    return true;
}

bool parse_patch_file(std::string_view text, const std::string& file, std::vector<Group>& groups, std::vector<std::string>& errors) {
    Group* g = nullptr;
    uint32_t origin = kInvalidOrigin, caveOrigin = 0;
    std::vector<Entry> scheduledLabels;
    auto fail = [&](int line, const std::string& msg) {
        errors.push_back(fmt::format("{} line {}: {}", file, line, msg));
        return false;
    };
    auto flushLabels = [&](uint32_t va, bool code = false) {
        for (auto& l : scheduledLabels) {
            l.addr = va;
            l.code = code;
            g->entries.push_back(std::move(l));
        }
        scheduledLabels.clear();
    };
    auto finishGroup = [&]() {
        if (!g) return;
        flushLabels(origin);
        uint32_t maxAddr = 0;
        for (auto& e : g->entries)
            if (e.kind == Entry::Instruction && e.addr < kCodeCaveMaxOffset) maxAddr = std::max(maxAddr, e.addr + (uint32_t)e.data.size());
        g->caveSize = maxAddr;
    };
    int lineNo = 0;
    for (std::string_view raw : lines_of(text)) {
        lineNo++;
        std::string lineStr(raw.substr(0, comment_start(raw)));
        StringTokenParser parser(lineStr.data(), (sint32)lineStr.size());
        parser.skipWhitespaces();
        if (parser.isEndOfString()) continue;
        if (parser.compareCharacter(0, '[')) {
            parser.skipCharacters(1);
            const char* n = parser.getCurrentPtr();
            sint32 len = parser.skipToCharacter(']');
            if (len < 0) return fail(lineNo, "Expected ']'");
            finishGroup();
            groups.push_back({});
            g = &groups.back();
            g->name.assign(n, len);
            origin = kInvalidOrigin;
            caveOrigin = 0;
            continue;
        }
        if (parser.matchWordI("moduleMatches")) {
            if (!g) return fail(lineNo, "moduleMatches outside of a group");
            if (!parser.matchWordI("=")) return fail(lineNo, "Expected '=' after moduleMatches");
            for (;;) {
                if (parser.matchWordI("rpx")) {
                    g->rpxOnly = true;
                    break;
                }
                uint32 crc = 0;
                if (!parser.parseU32(crc)) return fail(lineNo, "Invalid value for moduleMatches");
                g->matches.push_back(crc);
                if (!parser.matchWordI(",")) break;
            }
            continue;
        }
        if (!g) return fail(lineNo, "Line outside of a group");
        uint32_t overwrite = kInvalidOrigin;
        if (parser.compareCharacter(0, '0') && parser.compareCharacterI(1, 'x')) {
            uint32 a;
            if (!parser.parseU32(a)) return fail(lineNo, "Malformed address");
            if (!parser.matchWordI("=")) return fail(lineNo, "Expected '=' after address");
            parser.skipWhitespaces();
            parser.trimWhitespaces();
            overwrite = a;
        }
        if (parser.matchWordI(".origin")) {
            if (!parser.matchWordI("=")) return fail(lineNo, "Missing '=' after .origin");
            uint32 a;
            if (parser.matchWordI("codecave")) origin = caveOrigin;
            else if (parser.parseU32(a)) origin = a;
            else return fail(lineNo, ".origin must be followed by codecave or an address");
            continue;
        }
        if (parser.matchWordI(".callback")) {
            const char* s;
            sint32 len;
            if (!parser.matchWordI("entry")) return fail(lineNo, "Unrecognized type for .callback");
            if (!parser.parseSymbolName(s, len)) return fail(lineNo, ".callback must name a symbol");
            g->callbacks.emplace_back(s, len);
            continue;
        }
        StringTokenParser bak;
        const char* sym;
        sint32 symLen;
        parser.storeParserState(&bak);
        if (parser.parseSymbolName(sym, symLen) && parser.matchWordI(":")) {
            Entry e{Entry::Label, lineNo, std::string(sym, symLen)};
            if (overwrite != kInvalidOrigin) {
                e.code = true;  // a label at a fixed address: game code
                e.addr = overwrite;
                g->entries.push_back(std::move(e));
            } else {
                if (origin == kInvalidOrigin) return fail(lineNo, "Label without an address (no .origin)");
                scheduledLabels.push_back(std::move(e));  // sticks to the next data / instruction
            }
            continue;
        }
        parser.restoreParserState(&bak);
        if (parser.parseSymbolName(sym, symLen) && parser.matchWordI("=")) {
            parser.skipWhitespaces();
            parser.trimWhitespaces();
            Entry e{Entry::Variable, lineNo, std::string(sym, symLen), std::string(parser.getCurrentPtr(), parser.getCurrentLen())};
            g->entries.push_back(std::move(e));
            continue;
        }
        parser.restoreParserState(&bak);
        std::string instr(parser.getCurrentPtr(), parser.getCurrentLen());
        PPCAssemblerInOut ctx{};
        ctx.forceNoAlignment = overwrite != kInvalidOrigin;
        if (overwrite != kInvalidOrigin) ctx.virtualAddress = overwrite;
        else if (origin != kInvalidOrigin) ctx.virtualAddress = origin;
        else return fail(lineNo, "Instruction without an address (declare .origin or prefix the line with <address> =)");
        if (!ppcAssembler_assembleSingleInstruction(instr.c_str(), &ctx)) return fail(lineNo, "Assembler: " + ctx.errorMsg);
        if (overwrite == kInvalidOrigin) {
            origin += ctx.alignmentPaddingSize + (uint32_t)ctx.outputData.size();
            if (origin <= 32 * 1024 * 1024) caveOrigin = std::max(caveOrigin, origin);
        }
        flushLabels(ctx.virtualAddressAligned, !instr.empty() && instr[0] != '.');
        Entry e{Entry::Instruction, lineNo};
        e.addr = ctx.virtualAddressAligned;
        e.data.assign(ctx.outputData.begin(), ctx.outputData.end());
        e.relocs = ctx.list_relocs;
        g->entries.push_back(std::move(e));
    }
    finishGroup();
    return true;
}

enum class R { Resolved, Unknown, Error };

struct Resolver {
    const Module& m;
    const std::map<std::string, double>& presetVars;
    std::unordered_map<std::string, uint32_t> values;
    Group* group = nullptr;
    bool unknown = false;
    std::set<std::string>* missing = nullptr;  // last pass: unresolved names
    std::vector<std::string>& errors;

    bool relocate(uint32_t a, uint32_t& out) const {
        if (a <= kCodeCaveMaxOffset) {
            out = group->caveBase + a;
            return true;
        }
        for (auto& [lo, size] : m.sections)
            if (a >= lo && a < lo + size) {
                out = a;  // sections are loaded at their link addresses
                return true;
            }
        return false;
    }

    static inline thread_local Resolver* current = nullptr;  // the parser takes plain function pointers

    template <typename T>
    R evaluate(const std::string& expr, T& result) {
        ExpressionParser ep;
        unknown = false;
        current = this;
        ep.AddConstantCallback([](std::string_view name) -> double { return current->constant(name); });
        ep.SetFunctionCallback([](std::string_view fn, double in) -> double { return current->function(fn, in); });
        try {
            result = (T)ep.Evaluate(expr);
        } catch (const std::exception&) {
            errors.push_back(fmt::format("[{}] bad expression \"{}\"", group->name, expr));
            return R::Error;
        }
        return unknown ? R::Unknown : R::Resolved;
    }

    double constant(std::string_view name) {
        {
            std::string n(name);
            if (!n.empty() && n[0] == '$') {
                auto it = presetVars.find(n);
                if (it != presetVars.end()) return it->second;
            } else {
                auto it = values.find(n);
                if (it != values.end()) return it->second;
            }
            unknown = true;
            if (missing) missing->insert(n);
            return 0.0;
        }
    }

    double function(std::string_view fn, double in) {
        {
            std::string f(fn);
            std::transform(f.begin(), f.end(), f.begin(), ::tolower);
            if (f == "ha" || f == "ha16") return TExpressionParser<double>::ExpressionFuncHA(in);
            if (f == "hi" || f == "hi16") return TExpressionParser<double>::ExpressionFuncHI(in);
            if (f == "lo" || f == "lo16") return TExpressionParser<double>::ExpressionFuncLO(in);
            if (f == "reloc") {
                uint32_t out = 0;
                if (!relocate((uint32_t)in, out)) unknown = true;
                return out;
            }
            unknown = true;
            if (missing) missing->insert(f + "()");
            return 0.0;
        }
    }

    bool define(const std::string& name, uint32_t v, int line) {
        if (values.count(name)) {
            errors.push_back(fmt::format("[{}] line {}: {} is already defined", group->name, line, name));
            return false;
        }
        values[name] = v;
        return true;
    }

    template <typename T>
    static void put_be(uint8_t* p, T v) {
        if constexpr (sizeof(T) == 8) {
            uint64_t u;
            memcpy(&u, &v, 8);
            u = __builtin_bswap64(u);
            memcpy(p, &u, 8);
        } else if constexpr (sizeof(T) == 4) {
            uint32_t u;
            memcpy(&u, &v, 4);
            u = __builtin_bswap32(u);
            memcpy(p, &u, 4);
        } else if constexpr (sizeof(T) == 2) {
            uint16_t u = __builtin_bswap16((uint16_t)v);
            memcpy(p, &u, 2);
        } else {
            *p = (uint8_t)v;
        }
    }
    static uint32_t get_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

    R reloc(Entry& e, PPCAssemblerReloc& r) {
        uint8_t* p = e.data.data() + r.m_byteOffset;
        if (r.m_relocType == PPCASM_RELOC::FLOAT) {
            float v;
            R x = evaluate(r.m_expression, v);
            if (x == R::Resolved) put_be(p, v);
            return x;
        }
        if (r.m_relocType == PPCASM_RELOC::DOUBLE) {
            double v;
            R x = evaluate(r.m_expression, v);
            if (x == R::Resolved) put_be(p, v);
            return x;
        }
        uint32_t v;
        R x = evaluate(r.m_expression, v);
        if (x != R::Resolved) return x;
        switch (r.m_relocType) {
        case PPCASM_RELOC::U32: put_be(p, v); return R::Resolved;
        case PPCASM_RELOC::U16: put_be(p, (uint16_t)v); return R::Resolved;
        case PPCASM_RELOC::U8: *p = (uint8_t)v; return R::Resolved;
        case PPCASM_RELOC::U32_MASKED_IMM: {
            uint32_t op = get_be32(p), mask = (0xFFFFFFFFu >> (32 - r.m_bitCount)) << r.m_bitOffset;
            op = (op & ~mask) | ((v << r.m_bitOffset) & mask);
            put_be(p, op);
            return R::Resolved;
        }
        case PPCASM_RELOC::BRANCH_S26:
        case PPCASM_RELOC::BRANCH_S16: {
            const bool s26 = r.m_relocType == PPCASM_RELOC::BRANCH_S26;
            uint32_t op = get_be32(p), field = s26 ? 0x3FFFFFC : 0xFFFC;
            if (s26 && (op & 2)) {  // absolute
                if (v >= 0x3FFFFFC) return errors.push_back(fmt::format("[{}] line {}: branch target out of range", group->name, e.line)), R::Error;
                op = (op & ~field) | (v & field);
            } else {
                uint32_t at = e.relocated + r.m_byteOffset;
                int64_t d = (int64_t)v - (int64_t)at;
                int64_t lim = s26 ? 0x1FFFFFF : 0x8000;
                if (d > lim - 1 || d < -lim) return errors.push_back(fmt::format("[{}] line {}: branch target \"{}\" out of range", group->name, e.line, r.m_expression)), R::Error;
                op = (op & ~field) | ((uint32_t)d & field);
            }
            put_be(p, op);
            return R::Resolved;
        }
        default: return R::Error;
        }
    }

    R resolve(Entry& e) {
        switch (e.kind) {
        case Entry::Label: {
            uint32_t a;
            if (!relocate(e.addr, a)) return errors.push_back(fmt::format("[{}] line {}: label {} is outside the module and the code cave", group->name, e.line, e.name)), R::Error;
            return define(e.name, a, e.line) ? R::Resolved : R::Error;
        }
        case Entry::Variable: {
            uint32_t v;
            R x = evaluate(e.expr, v);
            if (x != R::Resolved) return x;
            return define(e.name, v, e.line) ? R::Resolved : R::Error;
        }
        case Entry::CemuhookSymbol: {  // PatchEntryCemuhookSymbolValue: the value is an address (code cave offset or module)
            uint32_t v, a;
            R x = evaluate(e.expr, v);
            if (x != R::Resolved) return x;
            if (!relocate(v, a)) return errors.push_back(fmt::format("[{}] line {}: {} is outside the module and the code cave", group->name, e.line, e.name)), R::Error;
            return define(e.name, a, e.line) ? R::Resolved : R::Error;
        }
        case Entry::Instruction: {
            if (!e.addrDone) {
                if (!relocate(e.addr, e.relocated)) return errors.push_back(fmt::format("[{}] line {}: address {:08x} is outside the module and the code cave", group->name, e.line, e.addr)), R::Error;
                e.addrDone = true;
            }
            for (auto& r : e.relocs) {
                if (r.isApplied()) continue;
                R x = reloc(e, r);
                if (x != R::Resolved) return x;
                r.setApplied();
            }
            return R::Resolved;
        }
        }
        return R::Error;
    }
};

}  // namespace

bool apply_patches(const std::vector<std::pair<std::string, std::string>>& files, const std::map<std::string, double>& vars, const Module& m,
                   uint32_t caveBase, PatchResult& out) {
    out.caveBase = out.caveEnd = caveBase;
    std::vector<Group> all;
    // Cemu: patch_*.asm files (any order: the directory's); a file that fails to parse disables the pack
    for (auto& [name, text] : files) {
        const bool cemuhook = name.size() >= 11 && !strcasecmp(name.c_str() + name.size() - 11, "patches.txt");
        if (!(cemuhook ? parse_cemuhook_file(text, name, all, out.errors) : parse_patch_file(text, name, all, out.errors))) return false;
    }
    std::vector<Group*> groups;
    for (auto& g : all) {
        if (g.matches.empty() && !g.rpxOnly) {
            out.errors.push_back(fmt::format("Group [{}] has no moduleMatches", g.name));
            return false;
        }
        if (g.rpxOnly || std::find(g.matches.begin(), g.matches.end(), m.crc) != g.matches.end()) groups.push_back(&g);
        else out.groupsOther++;
    }
    uint32_t cave = caveBase;
    for (Group* g : groups) {
        if (g->caveSize) {
            cave = (cave + 255) & ~255u;
            g->caveBase = cave;
            cave += g->caveSize;
        }
    }
    out.caveEnd = cave;
    Resolver rs{m, vars, {}, nullptr, false, nullptr, out.errors};
    std::vector<std::vector<Entry*>> open(groups.size());
    for (size_t i = 0; i < groups.size(); i++)
        for (auto& e : groups[i]->entries) open[i].push_back(&e);
    auto remaining = [&] {
        size_t n = 0;
        for (auto& o : open) n += o.size();
        return n;
    };
    for (int pass = 0; pass < 30 && remaining(); pass++) {
        size_t done = 0;
        for (size_t i = 0; i < groups.size(); i++) {
            rs.group = groups[i];
            std::erase_if(open[i], [&](Entry* e) {
                R r = rs.resolve(*e);
                if (r == R::Resolved) done++;
                return r == R::Resolved;
            });
        }
        if (!out.errors.empty()) return false;
        if (!done) break;
    }
    if (remaining()) {
        std::set<std::string> missing;
        rs.missing = &missing;
        for (size_t i = 0; i < groups.size(); i++) {
            rs.group = groups[i];
            for (Entry* e : open[i]) rs.resolve(*e);
        }
        for (auto& n : missing) out.errors.push_back("Unresolved symbol: " + n);
        if (out.errors.empty()) out.errors.push_back("Some symbols could not be resolved");
        return false;
    }
    for (Group* g : groups) {
        for (auto& e : g->entries)
            if (e.kind == Entry::Instruction && !e.data.empty()) out.writes.push_back({e.relocated, e.data});
        for (auto& cb : g->callbacks) {
            auto it = rs.values.find(cb);
            if (it == rs.values.end()) {
                out.errors.push_back("Unknown .callback symbol " + cb);
                return false;
            }
            out.entryCallbacks.push_back(it->second);
        }
    }
    // code labels whose address is taken (data words, lis/addi pairs, li + mtctr): reached through
    // pointers, so the recompiler needs them as function entries
    auto names_in = [](const std::string& expr, std::set<std::string>& out) {
        for (size_t i = 0; i < expr.size();) {
            if (isalpha((unsigned char)expr[i]) || expr[i] == '_' || expr[i] == '.') {
                size_t j = i;
                while (j < expr.size() && (isalnum((unsigned char)expr[j]) || expr[j] == '_' || expr[j] == '.')) j++;
                out.insert(expr.substr(i, j - i));
                i = j;
            } else {
                i++;
            }
        }
    };
    std::set<std::string> taken;
    for (Group* g : groups)
        for (auto& e : g->entries) {
            if (e.kind == Entry::Variable) names_in(e.expr, taken);
            for (auto& r : e.relocs)
                if (r.m_relocType != PPCASM_RELOC::BRANCH_S26 && r.m_relocType != PPCASM_RELOC::BRANCH_S16) names_in(r.m_expression, taken);
        }
    for (Group* g : groups)
        for (auto& e : g->entries)
            if (e.kind == Entry::Label && e.code && taken.count(e.name)) out.codePointers.push_back(rs.values[e.name]);
    for (uint32_t a : out.entryCallbacks) out.codePointers.push_back(a);
    out.groups = (int)groups.size();
    out.symbols.insert(rs.values.begin(), rs.values.end());
    return true;
}

}  // namespace gpack
