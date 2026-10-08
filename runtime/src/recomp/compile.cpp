// LLVM compile pipeline for the recompiled game (see compile.h).
#include "compile.h"

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

#if defined(__aarch64__) && defined(__linux__)
#include <sys/auxv.h>
#endif

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "../release.h"
#include "emit.h"
#include "program.h"

namespace recomp {

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
    if (hw & (1UL << 7)) f += "+rdm,";        // HWCAP_ASIMDRDM
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

    std::vector<std::vector<uint32_t>> modules;
    for (size_t i = 0; i < prog.entries.size(); i += opt.perModule)
        modules.emplace_back(prog.entries.begin() + i, prog.entries.begin() + std::min(prog.entries.size(), i + opt.perModule));
    llvm::sys::fs::create_directories(opt.outDir);
    if (opt.modulesTotal) *opt.modulesTotal = modules.size();
    std::atomic<size_t> next{0}, instructions{0}, unhandled{0};
    std::atomic<bool> failed{false};
    std::mutex errm;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(errm);
        if (!failed.exchange(true)) err = why;
    };
    auto worker = [&] {
        for (size_t i; !failed && (i = next++) < modules.size();) {
            if (opt.cancel && *opt.cancel) return fail("cancelled");
            llvm::LLVMContext ctx;
            EmitStats st;
            std::string e;
            char name[32];
            snprintf(name, sizeof name, "code_%03zu", i);
            if (opt.keepExisting && llvm::sys::fs::exists(opt.outDir + "/" + name + ".o")) {
                if (opt.modulesDone) ++*opt.modulesDone;
                continue;
            }
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
            pb.registerModuleAnalyses(mam);
            pb.registerCGSCCAnalyses(cgam);
            pb.registerFunctionAnalyses(fam);
            pb.registerLoopAnalyses(lam);
            pb.crossRegisterProxies(lam, fam, cgam, mam);
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
    res.instructions = instructions;
    res.unhandled = unhandled;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return !failed;
}

}  // namespace recomp
