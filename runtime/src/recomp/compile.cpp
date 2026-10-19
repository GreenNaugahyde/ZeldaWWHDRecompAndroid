// LLVM compile pipeline for the recompiled game (see compile.h).
#include "compile.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

#include <algorithm>
#include <functional>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "../disc/crypto.h"
#include "../release.h"
#include "emit.h"
#include "locals.h"
#include "program.h"

namespace recomp {

namespace {
// Paired-single and single multiplies round their second operand to 25 bits (round25 in ppc.h: the
// double's bits masked and rounded at bit 27). A value widened from a float has its low 29 mantissa
// bits clear, so for it the masks are no-ops; LLVM can't see through fpext to the bits. Here the
// masks on such values fold away (round25(x) = x), as do other ands that keep or clear only those bits.
struct SingleBitsPass : llvm::PassInfoMixin<SingleBitsPass> {
    static bool single(llvm::Value* v, llvm::SmallPtrSetImpl<llvm::Value*>& seen, int depth) {
        using namespace llvm;
        if (auto* e = dyn_cast<FPExtInst>(v)) return e->getSrcTy()->getScalarType()->isFloatTy();
        if (auto* c = dyn_cast<ConstantFP>(v)) return (c->getValueAPF().bitcastToAPInt().getZExtValue() & 0x1FFFFFFFull) == 0;
        if (auto* c = dyn_cast<Constant>(v); c && c->getType()->isVectorTy()) {
            auto* vt = cast<FixedVectorType>(c->getType());
            for (unsigned k = 0; k < vt->getNumElements(); k++) {
                Constant* el = c->getAggregateElement(k);
                if (!el || !(isa<UndefValue>(el) || (isa<ConstantFP>(el) && (cast<ConstantFP>(el)->getValueAPF().bitcastToAPInt().getZExtValue() & 0x1FFFFFFFull) == 0)))
                    return false;
            }
            return true;
        }
        if (depth > 6) return false;
        if (!seen.insert(v).second) return true;  // a cycle of phis: single if everything entering it is
        if (auto* p = dyn_cast<PHINode>(v)) {
            for (Value* in : p->incoming_values())
                if (!single(in, seen, depth + 1)) return false;
            return true;
        }
        if (auto* s = dyn_cast<SelectInst>(v)) return single(s->getTrueValue(), seen, depth + 1) && single(s->getFalseValue(), seen, depth + 1);
        if (auto* u = dyn_cast<UnaryOperator>(v)) return u->getOpcode() == Instruction::FNeg && single(u->getOperand(0), seen, depth + 1);
        if (auto* c = dyn_cast<CallInst>(v))
            if (c->getIntrinsicID() == Intrinsic::fabs) return single(c->getArgOperand(0), seen, depth + 1);
        // a lane of a vector of such values (paired-single loads widen both floats at once)
        if (auto* e = dyn_cast<ExtractElementInst>(v)) return single(e->getVectorOperand(), seen, depth + 1);
        if (auto* e = dyn_cast<InsertElementInst>(v))
            return single(e->getOperand(0), seen, depth + 1) && single(e->getOperand(1), seen, depth + 1);
        if (auto* sv = dyn_cast<ShuffleVectorInst>(v))
            return single(sv->getOperand(0), seen, depth + 1) && single(sv->getOperand(1), seen, depth + 1);
        if (isa<PoisonValue>(v) || isa<UndefValue>(v)) return true;  // (lanes nobody reads)
        return false;
    }
    llvm::PreservedAnalyses run(llvm::Function& f, llvm::FunctionAnalysisManager&) {
        using namespace llvm;
        constexpr uint64_t kLow = 0x1FFFFFFFull;
        bool changed = false;
        for (BasicBlock& b : f)
            for (Instruction& i : make_early_inc_range(b)) {
                auto* bc = dyn_cast<BitCastInst>(&i);
                if (!bc || !bc->getSrcTy()->isDoubleTy() || !bc->getDestTy()->isIntegerTy(64)) continue;
                SmallPtrSet<Value*, 16> seen;
                if (!single(bc->getOperand(0), seen, 0)) continue;
                for (User* u : make_early_inc_range(bc->users())) {
                    auto* a = dyn_cast<BinaryOperator>(u);
                    ConstantInt* c = nullptr;
                    if (!a || a->getOpcode() != Instruction::And || !(c = dyn_cast<ConstantInt>(a->getOperand(1)))) continue;
                    uint64_t m = c->getZExtValue();
                    if ((m | kLow) == ~0ull) a->replaceAllUsesWith(bc);
                    else if ((m & ~kLow) == 0) a->replaceAllUsesWith(ConstantInt::get(a->getType(), 0));
                    else continue;
                    changed = true;
                }
            }
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }
};
}  // namespace

// Code for the CPU we run on: generic AArch64 plus only the extensions the kernel reports (a CPU
// model would imply some the firmware disables, e.g. SVE on Snapdragons); the model itself is used
// for scheduling only ("tune-cpu"). SVE/SVE2 are off: code compiled with them never ran on a
// tested device, and a crash on a Dimensity 8350 points at it (issue #24); WWHD_SVE=1 uses them where
// the kernel enables them (MediaTek Dimensity, Google Tensor; the code stays vector-length
// agnostic). SME stays off: streaming mode isn't something compiled game code can use.
std::string host_features() {
    std::string f;
#if defined(__aarch64__) && defined(__linux__)
    unsigned long hw = getauxval(AT_HWCAP);
    if (hw & (1UL << 8)) f += "+lse,";        // HWCAP_ATOMICS
    if (hw & (1UL << 9)) f += "+fullfp16,";   // HWCAP_FPHP
    if (hw & (1UL << 20)) f += "+dotprod,";   // HWCAP_ASIMDDP
    if (hw & (1UL << 15)) f += "+rcpc,";      // HWCAP_LRCPC
    if (hw & (1UL << 12)) f += "+rdm,";       // HWCAP_ASIMDRDM (bit 7 is CRC32)
    unsigned long hw2 = getauxval(AT_HWCAP2);
    static const bool sveOn = getenv("WWHD_SVE") && atoi(getenv("WWHD_SVE")) != 0;
    const bool sve = sveOn && (hw & (1UL << 22));  // HWCAP_SVE
    const bool sve2 = sve && (hw2 & (1UL << 1));  // HWCAP2_SVE2
    f += sve ? "+sve," : "-sve,";
    f += sve2 ? "+sve2," : "-sve2,";
    f += "-sme";
#else
    f += "-sve,-sve2,-sme";
#endif
    return f;
}

size_t module_count(size_t functions, size_t perModule) { return (functions + perModule - 1) / perModule; }

bool compile_game(const CompileOptions& opt, CompileResult& res, std::string& err) {
    auto t0 = std::chrono::steady_clock::now();
    Program prog;
    if (!prog.load(opt.rpxPath, err)) return false;
    if (!release::select(prog.entry)) {  // before the hooks: their USA addresses are translated
        err = "this game version is not supported";
        return false;
    }
    Hooks hooks;
    hooks.parse(opt.hooksText);

    static std::once_flag init;
    std::call_once(init, [] {
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
    });
    const std::string triple = "aarch64-unknown-linux-android30";
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, err);
    if (!target) return false;
    std::string cpu = opt.cpu.empty() ? "generic" : opt.cpu;
    std::string features = opt.cpu.empty() ? host_features() : "";
    std::string tune = opt.cpu.empty() ? llvm::sys::getHostCPUName().str() : "";

    // Modules are cut at every perModule-th entry of the unpatched executable, so mod pack patches
    // (CodePatch) change only the modules that hold patched code; code caves get modules of their own.
    // Each module's object file is named by a hash of everything its code depends on: the cache keeps
    // the unchanged ones, and switching packs back finds the earlier objects.
    std::vector<uint32_t> cuts;
    {
        Program base;
        const Program* b = &prog;
        if (code_patch()) {
            base.patch = nullptr;
            if (!base.load(opt.rpxPath, err)) return false;
            b = &base;
        }
        for (size_t i = 0; i < b->entries.size(); i += opt.perModule) cuts.push_back(b->entries[i]);
    }
    // The hot functions (hot_functions.txt, hottest first) go into modules of their own, first in the
    // load order: the code every frame runs then lies together (instruction cache, TLB), and a module's
    // small hot callees can be inlined into their callers. The rest stays cut by address.
    std::vector<uint32_t> hot;
    std::set<uint32_t> hotSet;
    {
        std::istringstream in(opt.hotText);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            uint32_t a = release::code((uint32_t)strtoul(line.c_str(), nullptr, 16));  // (USA address)
            if (!a || (a >= prog.caveLo && a < prog.caveHi) || !std::binary_search(prog.entries.begin(), prog.entries.end(), a))
                continue;
            if (hotSet.insert(a).second) hot.push_back(a);
        }
    }
    // Hot callees right after their callers (static calls, bl): the hottest function first, then its
    // hot callees, hottest first, a few levels deep, then the next hot function not placed yet. Code
    // that runs together then shares cache lines, pages and modules (inlining) instead of lying wherever
    // its rank puts it. WWHD_RECOMP_HOT_ORDER=0: by rank only (in the code cache key).
    if (hot_call_order()) {
        std::map<uint32_t, size_t> rank;
        for (size_t i = 0; i < hot.size(); i++) rank[hot[i]] = i;
        std::map<uint32_t, std::vector<uint32_t>> callees;
        for (uint32_t f : hot) {
            std::vector<uint32_t>& cs = callees[f];
            for (uint32_t a = f, end = prog.funcEnd(f); a < end; a += 4) {
                uint32_t w = prog.word(a);
                if ((w >> 26) != 18 || (w & 3) != 1) continue;  // bl
                uint32_t t = a + (uint32_t)((int32_t)(w << 6) >> 6 & ~3);
                if (t != f && rank.count(t) && std::find(cs.begin(), cs.end(), t) == cs.end()) cs.push_back(t);
            }
            std::sort(cs.begin(), cs.end(), [&](uint32_t x, uint32_t y) { return rank[x] < rank[y]; });
        }
        std::vector<uint32_t> order;
        std::set<uint32_t> placed;
        std::function<void(uint32_t, int)> place = [&](uint32_t f, int depth) {
            if (!placed.insert(f).second) return;
            order.push_back(f);
            if (depth == 0) return;
            for (uint32_t c : callees[f]) place(c, depth - 1);
        };
        for (uint32_t f : hot) place(f, 3);
        hot.swap(order);
    }
    constexpr size_t kHotPerModule = 600;
    std::vector<std::vector<uint32_t>> modules;
    for (size_t i = 0; i < hot.size(); i += kHotPerModule)
        modules.emplace_back(hot.begin() + i, hot.begin() + std::min(hot.size(), i + kHotPerModule));
    const size_t firstCut = modules.size();
    modules.resize(firstCut + cuts.size());
    std::vector<uint32_t> caveFuncs;
    for (uint32_t a : prog.entries) {
        if (hotSet.count(a)) continue;
        if (a >= prog.caveLo && a < prog.caveHi) {
            caveFuncs.push_back(a);
            continue;
        }
        auto it = std::upper_bound(cuts.begin(), cuts.end(), a);
        modules[firstCut + (it == cuts.begin() ? 0 : it - cuts.begin() - 1)].push_back(a);
    }
    for (size_t i = 0; i < caveFuncs.size(); i += opt.perModule)
        modules.emplace_back(caveFuncs.begin() + i, caveFuncs.begin() + std::min(caveFuncs.size(), i + opt.perModule));
    modules.erase(std::remove_if(modules.begin(), modules.end(), [](auto& m) { return m.empty(); }), modules.end());
    std::vector<std::string> names(modules.size());
    for (size_t i = 0; i < modules.size(); i++) {
        std::string d = opt.baseKey + "\ncpu " + cpu + " " + features + " " + tune + " -O" + std::to_string(opt.optLevel) + "\n";
        auto put = [&](uint32_t v) { d.append((const char*)&v, 4); };
        auto isEntry = [&](uint32_t a) { return std::binary_search(prog.entries.begin(), prog.entries.end(), a); };
        if (getenv("WWHD_PACK_TRACE") && atoi(getenv("WWHD_PACK_TRACE")) && modules[i][0] >= prog.caveLo && modules[i][0] < prog.caveHi)
            d += "pack trace\n";  // emit.cpp, trace()
        for (uint32_t f : modules[i]) {
            uint32_t end = prog.funcEnd(f);
            put(f), put(end), put(prog.inText(end) && isEntry(end));
            for (uint32_t a = f; a < end; a += 4) {
                uint32_t w = prog.word(a), op = w >> 26;
                put(w);
                // targets outside the function: a call / tail call when they are entries, else a dispatch
                uint32_t t = 0xFFFFFFFFu;
                if (op == 18) t = (uint32_t)(((int32_t)((w & 0x03FFFFFC) << 6) >> 6) + (w & 2 ? 0 : (int32_t)a));
                else if (op == 16) t = (uint32_t)((int32_t)(int16_t)(w & 0xFFFC) + (w & 2 ? 0 : (int32_t)a));
                if (t != 0xFFFFFFFFu && (t < f || t >= end)) put(t), put(isEntry(t));
            }
        }
        uint8_t h[20];
        disc::sha1((const uint8_t*)d.data(), d.size(), h);
        char hex[48] = "m_";
        for (int k = 0; k < 20; k++) snprintf(hex + 2 + 2 * k, 3, "%02x", h[k]);
        names[i] = hex;
    }
    llvm::sys::fs::create_directories(opt.outDir);
    std::vector<size_t> todo;
    for (size_t i = 0; i < modules.size(); i++)
        if (!llvm::sys::fs::exists(opt.outDir + "/" + names[i] + ".o")) todo.push_back(i);
    if (opt.modulesTotal) *opt.modulesTotal = todo.size();
    std::atomic<size_t> next{0}, instructions{0}, unhandled{0}, localized{0}, localFallback{0};
    std::atomic<bool> failed{false};
    std::mutex errm;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(errm);
        if (!failed.exchange(true)) err = why;
    };
    auto worker = [&] {
        for (size_t n; !failed && (n = next++) < todo.size();) {
            if (opt.cancel && *opt.cancel) return fail("cancelled");
            const size_t i = todo[n];
            llvm::LLVMContext ctx;
            EmitStats st;
            std::string e;
            const char* name = names[i].c_str();
            auto m = emit_module(ctx, prog, hooks, modules[i], opt.opsBitcode, name, st, e);
            if (!m) return fail(std::string(name) + ": " + e);
            m->setTargetTriple(triple);
            llvm::TargetOptions to;
            to.AllowFPOpFusion = llvm::FPOpFusion::Strict;  // exact FP: no contraction into FMA
            std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
                triple, cpu, features, to, llvm::Reloc::PIC_, std::nullopt,
                opt.optLevel >= 3 ? llvm::CodeGenOptLevel::Aggressive : llvm::CodeGenOptLevel::Default));
            m->setDataLayout(tm->createDataLayout());
            for (llvm::Function& f : *m) {
                if (f.isDeclaration()) continue;
                if (!tune.empty() && tune != "generic") f.addFnAttr("tune-cpu", tune);
                // unwind tables (.eh_frame, registered by the loader): native backtraces go through the
                // game code, so crash logs name the game functions of the call chain
                f.setUWTableKind(llvm::UWTableKind::Async);
            }
            std::string verr;
            llvm::raw_string_ostream vs(verr);
            if (llvm::verifyModule(*m, &vs)) return fail(std::string(name) + ": invalid IR: " + verr.substr(0, 500));
            // inlines the ops and folds their decoding
            llvm::LoopAnalysisManager lam;
            llvm::FunctionAnalysisManager fam;
            llvm::CGSCCAnalysisManager cgam;
            llvm::ModuleAnalysisManager mam;
            llvm::PassBuilder pb(tm.get());
            pb.registerPeepholeEPCallback([](llvm::FunctionPassManager& fpm, llvm::OptimizationLevel) { fpm.addPass(SingleBitsPass()); });
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
            {  // the ops inlined and their decoding folded first: the register copy's accesses are then known
                llvm::ModulePassManager mpm;
                mpm.addPass(llvm::AlwaysInlinerPass());
                mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::InstSimplifyPass()));
                mpm.run(*m, mam);
            }
            LocalsStats ls;
            localize_registers(*m, locals_options(), ls);
            localized += ls.functions;
            localFallback += ls.fallback;
            pb.buildPerModuleDefaultPipeline(opt.optLevel >= 3 ? llvm::OptimizationLevel::O3 : llvm::OptimizationLevel::O2).run(*m, mam);
            std::string path = opt.outDir + "/" + name + ".o", tmp = path + ".part";
            {
                std::error_code ec;
                llvm::raw_fd_ostream out(tmp, ec, llvm::sys::fs::OF_None);
                llvm::legacy::PassManager cg;
                if (ec || tm->addPassesToEmitFile(cg, out, nullptr, llvm::CodeGenFileType::ObjectFile))
                    return fail(std::string("cannot write ") + path);
                cg.run(*m);
            }
            if (llvm::sys::fs::rename(tmp, path)) return fail(std::string("cannot write ") + path);
            instructions += st.instructions;
            unhandled += st.unhandled;
            if (opt.modulesDone) ++*opt.modulesDone;
        }
    };
    unsigned jobs = opt.jobs ? opt.jobs : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> pool;
    for (unsigned j = 0; j < jobs; j++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    res.functions = prog.entries.size();
    res.modules = modules.size();
    res.compiled = todo.size();
    for (auto& n : names) res.objects.push_back(n + ".o");
    res.instructions = instructions;
    res.unhandled = unhandled;
    res.localized = localized;
    res.localFallback = localFallback;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return !failed;
}

}  // namespace recomp
