// LLVM IR emission for recompiled guest functions (see emit.h). Control flow follows
// tools/recomp/recomp.py's Ctx callbacks and ppc2c.py's branch translation.
#include "emit.h"

#include "../release.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/MemoryBuffer.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <map>
#include <sstream>

#include "decode.h"
#include "locals.h"
#include "ppc.h"

namespace recomp {

void Hooks::parse(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t b = line.find_first_not_of(" \t\r"), e = line.find_last_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, e - b + 1);
        // USA addresses: the running release's (release.h); none if the code differs between them
        bool site = line[0] == '@';
        uint32_t a = release::code((uint32_t)strtoul(line.c_str() + (site ? 1 : 0), nullptr, 16));
        if (a) (site ? sites : functions).insert(a);
    }
}

namespace {

int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

// the name of guest code at `a`: its USA address (the runtime names game functions by those;
// release.h), or for code without one (other releases only) E + its own address
std::string sym(uint32_t a) {
    uint32_t u = release::usa_code(a);
    char b[10];
    snprintf(b, sizeof b, u ? "%08X" : "E%08X", u ? u : a);
    return b;
}

std::string hex8(uint32_t v) {
    char b[9];
    snprintf(b, sizeof b, "%08X", v);
    return b;
}

std::string c_ident(const std::string& s) {
    std::string o = s;
    for (char& ch : o)
        if (!isalnum((unsigned char)ch) && ch != '_') ch = '_';
    return o;
}

class FunctionEmitter {
public:
    FunctionEmitter(llvm::Module& m, const Program& p, const Hooks& h, EmitStats& st)
        : M(m), C(m.getContext()), P(p), H(h), S(st), B(C) {
        ptrTy = llvm::PointerType::getUnqual(C);
        fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(C), {ptrTy}, false);
        i8 = llvm::Type::getInt8Ty(C);
        i32 = llvm::Type::getInt32Ty(C);
    }

    void emit(uint32_t start) {
        cur = start;
        end = P.funcEnd(start);
        bool hooked = H.functions.count(start) != 0;
        if (hooked) {  // callers reach hook_X, which may call the game's code (f_X_orig)
            llvm::Function* w = guest("f_" + sym(start));
            auto* bb = llvm::BasicBlock::Create(C, "", w);
            B.SetInsertPoint(bb);
            B.CreateCall(external("hook_" + sym(start)), {w->getArg(0)});
            B.CreateRetVoid();
        }
        llvm::Function* fn = guest(hooked ? "f_" + sym(start) + "_orig" : "f_" + sym(start));
        // A function without calls gets its slow entry path (ppc_entry) in a copy of its own (f_X_s, the
        // whole function as below), reached by a tail call: the fast path then needs no stack frame for
        // the return address (the call in the rejoining slow path kept LLVM from shrink-wrapping it;
        // saving and restoring it was ~3% of the game thread on a Helio G85)
        if (leaf()) {
            llvm::Function* slow = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage, fn->getName() + "_s", M);
            slow->addParamAttr(0, llvm::Attribute::NoAlias);
            emit_function(slow, nullptr);
            emit_function(fn, slow);
        } else {
            emit_function(fn, nullptr);
        }
        S.functions++;
    }

private:
    // no calls (and no instruction hooks or traces, which call): see emit
    bool leaf() const {
        static const bool on = !getenv("WWHD_RECOMP_LEAF_SPLIT") || atoi(getenv("WWHD_RECOMP_LEAF_SPLIT")) != 0;
        if (!on || (cur >= P.caveLo && cur < P.caveHi)) return false;
        for (uint32_t a = cur; a < end; a += 4) {
            if (H.sites.count(a)) return false;
            Decoded d = decode(P.word(a));
            uint32_t w = P.word(a);
            if ((d.kind == Kind::Branch || d.kind == Kind::BranchCond || d.kind == Kind::BranchLr || d.kind == Kind::BranchCtr) && (w & 1))
                return false;
        }
        return true;
    }
    // the function's code into F; fastTo: F only checks the entry flag and goes to that copy when it is
    // set (which has the slow path), instead of the slow path itself
    void emit_function(llvm::Function* fn, llvm::Function* fastTo) {
        F = fn;
        const uint32_t start = cur;
        if (start >= P.caveLo && start < P.caveHi) F->addFnAttr("wwhd-noeabi");  // mod pack code (locals.cpp)
        c = F->getArg(0);
        auto* entry = llvm::BasicBlock::Create(C, "entry", F);  // first: the function's entry block
        collect_labels();
        B.SetInsertPoint(entry);
        // the ops work on the function's register copy; calls get the struct (locals.cpp)
        auto* copy = B.CreateAlloca(llvm::ArrayType::get(i8, sizeof(Cpu)), nullptr, kLocalCpuName);
        copy->setAlignment(llvm::Align(16));
        regs = copy;
        // the entry check (trace, preemption); its slow path may also say where to continue: at once
        // back to the caller, or after one of the function's calls (a save state's call chain is
        // rebuilt: the thread enters each function of the chain at its call, see threads.cpp)
        resumes.clear();
        llvm::Value* where = nullptr;
        llvm::BasicBlock* resume = nullptr;
        auto* body = llvm::BasicBlock::Create(C, "body", F);
        if (fastTo) {
            auto* slow = llvm::BasicBlock::Create(C, "slow", F);
            llvm::Value* flag = load32(offsetof(Cpu, preempt));  // as ppcop_enter
            B.CreateCondBr(B.CreateICmpNE(flag, B.getInt32(0)), slow, body, llvm::MDBuilder(C).createBranchWeights(1, 1 << 20));
            B.SetInsertPoint(slow);
            tail(fastTo);
        } else {
            where = B.CreateCall(M.getFunction("ppcop_enter"), {regs, B.getInt32(end - start), B.getInt32(start)});
            resume = llvm::BasicBlock::Create(C, "resume", F);
            // (rare: save states rebuilding a call chain, preemption; weighted so the resume switch is laid
            // out after the function's code instead of between its entry and body)
            B.CreateCondBr(B.CreateICmpNE(where, B.getInt32(0)), resume, body, llvm::MDBuilder(C).createBranchWeights(1, 1 << 20));
        }
        B.SetInsertPoint(body);
        for (uint32_t a = start; a < end; a += 4) {
            auto it = blocks.find(a);
            if (it != blocks.end()) {
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(it->second);
                B.SetInsertPoint(it->second);
                if (loopHeads.count(a)) loop_barrier();
                trace(a);
            } else if (a == start) {
                trace(a);
            } else if (B.GetInsertBlock()->getTerminator()) {
                B.SetInsertPoint(llvm::BasicBlock::Create(C, "", F));  // unreachable code after a jump
            }
            if (H.sites.count(a)) B.CreateCall(external("site_" + sym(a)), {c});
            instruction(a, P.word(a));
            if (!fastTo) S.instructions++;  // (a function split in two: counted once)
        }
        if (!B.GetInsertBlock()->getTerminator()) {  // falls through into the next function
            if (std::binary_search(P.entries.begin(), P.entries.end(), end)) {  // (a code cave's data: no)
                std::string next = "f_" + sym(end) + (H.functions.count(end) ? "_orig" : "");
                tail(guest(next));
            } else {
                call_unimplemented(end, 0);
                B.CreateRetVoid();
            }
        }
        if (!resume) return;
        // where: 1 returns, a call's return address goes to that call (its callee continues the
        // chain); anything else is a chain that doesn't fit this function
        B.SetInsertPoint(resume);
        auto* ret = llvm::BasicBlock::Create(C, "", F);
        auto* bad = llvm::BasicBlock::Create(C, "", F);
        llvm::SwitchInst* sw = B.CreateSwitch(where, bad, (unsigned)resumes.size() + 1);
        sw->addCase(B.getInt32(1), ret);
        for (auto& [ra, bb] : resumes) sw->addCase(B.getInt32(ra), bb);
        B.SetInsertPoint(ret);
        B.CreateRetVoid();
        B.SetInsertPoint(bad);
        B.CreateCall(external("ppc_resume_failed"), {c});
        B.CreateRetVoid();
    }

    llvm::Module& M;
    llvm::LLVMContext& C;
    const Program& P;
    const Hooks& H;
    EmitStats& S;
    llvm::IRBuilder<> B;
    llvm::PointerType* ptrTy;
    llvm::FunctionType* fnTy;
    llvm::Type *i8, *i32;
    llvm::Function* F = nullptr;
    llvm::Value* c = nullptr;     // the Cpu struct: passed to calls
    llvm::Value* regs = nullptr;  // the registers the ops and branches use: the function's copy
    uint32_t cur = 0, end = 0;
    std::map<uint32_t, llvm::BasicBlock*> blocks;
    std::set<uint32_t> loopHeads;
    std::map<uint32_t, llvm::BasicBlock*> resumes;  // a call's return address -> the call (see emit)

    // ---- symbols
    llvm::Function* guest(const std::string& name) {
        llvm::Function* f = M.getFunction(name);
        if (!f) {
            f = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, M);
            f->addParamAttr(0, llvm::Attribute::NoAlias);  // guest memory never aliases the register file
        }
        return f;
    }
    // WWHD_PACK_TRACE=1 (code cave modules, compile.cpp): each block of a mod pack's code logs its first
    // run (packs.cpp, wwhd_pack_trace)
    void trace(uint32_t a) {
        static const bool on = getenv("WWHD_PACK_TRACE") && atoi(getenv("WWHD_PACK_TRACE")) != 0;
        if (!on || a < P.caveLo || a >= P.caveHi) return;
        llvm::FunctionCallee f = M.getOrInsertFunction("wwhd_pack_trace", llvm::FunctionType::get(llvm::Type::getVoidTy(C), {ptrTy, i32}, false));
        B.CreateCall(f, {c, B.getInt32(a)});
    }
    llvm::Function* external(const std::string& name) {
        llvm::Function* f = M.getFunction(name);
        return f ? f : llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, M);
    }
    std::string import_name(uint32_t slot) { return import_symbol(P.imports.at(slot)); }

    // ---- Cpu fields
    llvm::Value* field(size_t off) { return B.CreateConstInBoundsGEP1_64(i8, regs, off); }
    llvm::Value* load32(size_t off) { return B.CreateLoad(i32, field(off)); }
    void store32(size_t off, llvm::Value* v) { B.CreateStore(v, field(off)); }
    void store32(size_t off, uint32_t v) { store32(off, B.getInt32(v)); }
    llvm::Value* cr(uint32_t bit) { return B.CreateLoad(i8, field(offsetof(Cpu, cr) + bit)); }

    // ---- calls
    void call_op(const char* name, uint32_t w, uint32_t addr) {
        llvm::Function* f = M.getFunction(std::string("ppcop_") + name);
        B.CreateCall(f, {regs, B.getInt32(w), B.getInt32(addr)});
    }
    void call_unimplemented(uint32_t addr, uint32_t w) { call_op("unimplemented", w, addr); }
    void tail(llvm::Function* f) {
        llvm::CallInst* ci = B.CreateCall(f, {c});
        ci->setTailCallKind(llvm::CallInst::TCK_MustTail);
        B.CreateRetVoid();
    }
    // an indirect call or jump to the address in pc: game code is looked up in g_ppc_text_table
    // right here (one range check and load); anything else, or an address without a function, goes
    // through ppc_dispatch (imports, HLE, host functions, the error for an unknown address)
    void dispatch(bool isTail) {
        llvm::Function* d = external("ppc_dispatch");
        llvm::Value* off = B.CreateSub(load32(offsetof(Cpu, pc)), B.getInt32(0x02000000));
        llvm::Value* inText = B.CreateICmpULT(off, B.getInt32(0x01000000));
        llvm::Value* idx = B.CreateSelect(inText, B.CreateLShr(off, 2), B.getInt32(0));
        llvm::GlobalVariable* table = M.getGlobalVariable("g_ppc_text_table");
        if (!table)
            table = new llvm::GlobalVariable(M, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, "g_ppc_text_table");
        llvm::Value* base = B.CreateLoad(ptrTy, table);
        llvm::Value* fn = B.CreateLoad(ptrTy, B.CreateInBoundsGEP(ptrTy, base, B.CreateZExt(idx, B.getInt64Ty())));
        llvm::Value* found = B.CreateAnd(inText, B.CreateICmpNE(fn, llvm::ConstantPointerNull::get(ptrTy)));
        llvm::Value* target = B.CreateSelect(found, fn, d);
        llvm::CallInst* ci = B.CreateCall(fnTy, target, {c});
        if (isTail) {
            ci->setTailCallKind(llvm::CallInst::TCK_MustTail);
            B.CreateRetVoid();
        }
    }
    void loop_barrier() {
        // guest loops may wait for another core's store: memory is re-read every iteration
        auto* asmTy = llvm::FunctionType::get(llvm::Type::getVoidTy(C), false);
        B.CreateCall(llvm::InlineAsm::get(asmTy, "", "~{memory}", true));
    }

    // ---- labels: branch targets inside the function and jump table entries
    void collect_labels() {
        blocks.clear();
        loopHeads.clear();
        for (uint32_t a = cur; a < end; a += 4) {
            uint32_t w = P.word(a), op = w >> 26;
            uint32_t t = 0xFFFFFFFFu;
            if (op == 18 && !(w & 1) && !P.importCalls.count(a)) t = (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a));
            if (op == 16 && !(w & 1)) t = (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)a));
            if (t >= cur && t < end) {
                label(t);
                if (t <= a) loopHeads.insert(t);
            }
            if (op == 19 && ((w >> 1) & 0x3FF) == 528 && !(w & 1)) {  // bcctr: jump table?
                auto jt = P.jumpTables.find(a);
                if (jt != P.jumpTables.end())
                    for (uint32_t i = 0; i < jt->second.second; i++) {
                        uint32_t slot = jt->second.first + 4 * i;
                        if (slot >= cur && slot < end) {
                            label(slot);
                            if (slot <= a) loopHeads.insert(slot);
                        }
                    }
            }
        }
    }
    void label(uint32_t a) {
        if (!blocks.count(a)) blocks[a] = llvm::BasicBlock::Create(C, "L_" + hex8(a), F);
    }

    // a call at `a` starts a block of its own: resuming at its return address enters the call again
    void resume_point(uint32_t a) {
        auto* bb = llvm::BasicBlock::Create(C, "R_" + hex8(a), F);
        B.CreateBr(bb);
        B.SetInsertPoint(bb);
        resumes[a + 4] = bb;
    }

    // ---- control flow (recomp.py's Ctx callbacks)
    void branch(uint32_t addr, uint32_t tgt) {
        auto ic = P.importCalls.find(addr);
        if (ic != P.importCalls.end()) return tail(external(import_name(ic->second)));
        if (tgt >= cur && tgt < end) {
            B.CreateBr(blocks.at(tgt));
            return;
        }
        if (std::binary_search(P.entries.begin(), P.entries.end(), tgt)) return tail(guest("f_" + sym(tgt)));
        store32(offsetof(Cpu, pc), tgt);
        dispatch(true);
    }
    void call(uint32_t addr, uint32_t tgt) {
        auto ic = P.importCalls.find(addr);
        if (ic != P.importCalls.end()) {
            B.CreateCall(external(import_name(ic->second)), {c});
            return;
        }
        if (P.undefCalls.count(addr)) {
            call_unimplemented(addr, 0);
            return;
        }
        if (std::binary_search(P.entries.begin(), P.entries.end(), tgt)) {
            B.CreateCall(guest("f_" + sym(tgt)), {c});
            return;
        }
        store32(offsetof(Cpu, pc), tgt);
        dispatch(false);
    }
    void indirect_jump(uint32_t addr) {
        llvm::Value* ctr = load32(offsetof(Cpu, ctr));
        auto* fallback = llvm::BasicBlock::Create(C, "", F);
        auto jt = P.jumpTables.find(addr);
        std::vector<uint32_t> slots;
        if (jt != P.jumpTables.end())
            for (uint32_t i = 0; i < jt->second.second; i++) {
                uint32_t slot = jt->second.first + 4 * i;
                if (slot >= cur && slot < end) slots.push_back(slot);
            }
        if (!slots.empty()) {
            llvm::SwitchInst* sw = B.CreateSwitch(ctr, fallback, (unsigned)slots.size());
            std::set<uint32_t> seen;
            for (uint32_t s : slots)
                if (seen.insert(s).second) sw->addCase(B.getInt32(s), blocks.at(s));
        } else {
            B.CreateBr(fallback);
        }
        B.SetInsertPoint(fallback);
        store32(offsetof(Cpu, pc), ctr);
        dispatch(true);
    }

    // bc-family condition (nullptr: always); the ctr decrement is emitted before
    llvm::Value* condition(uint32_t bo, uint32_t bi) {
        llvm::Value* cond = nullptr;
        if (!(bo & 0x04)) {
            llvm::Value* ctr = load32(offsetof(Cpu, ctr));
            cond = (bo & 0x02) ? B.CreateICmpEQ(ctr, B.getInt32(0)) : B.CreateICmpNE(ctr, B.getInt32(0));
        }
        if (!(bo & 0x10)) {
            llvm::Value* bit = B.CreateICmpNE(cr(bi), B.getInt8(0));
            if (!(bo & 0x08)) bit = B.CreateNot(bit);
            cond = cond ? B.CreateAnd(cond, bit) : bit;
        }
        return cond;
    }
    void ctr_decrement(uint32_t bo) {
        if (bo & 0x04) return;
        store32(offsetof(Cpu, ctr), B.CreateSub(load32(offsetof(Cpu, ctr)), B.getInt32(1)));
    }
    // `body` runs if `cond` holds (always if null); code continues after it either way
    template <typename Body> void guarded(llvm::Value* cond, Body body) {
        if (!cond) {
            body();
            return;
        }
        auto* then = llvm::BasicBlock::Create(C, "", F);
        auto* after = llvm::BasicBlock::Create(C, "", F);
        B.CreateCondBr(cond, then, after);
        B.SetInsertPoint(then);
        body();
        if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(after);
        B.SetInsertPoint(after);
    }

    // lmw/stmw (function prologues and epilogues) as one lwz/stw per register: the same accesses, and
    // each register a constant (locals.cpp keeps them apart; the op's loop indexes the registers).
    // Not where lmw loads its own base register (an invalid form) or an offset leaves 16 bits.
    bool split_multiple(uint32_t w, uint32_t a) {
        uint32_t op = w >> 26, rd = (w >> 21) & 31, ra = (w >> 16) & 31;
        int32_t d = sext(w & 0xFFFF, 16);
        if (op != 46 && op != 47) return false;
        if (op == 46 && ra != 0 && ra >= rd) return false;
        if (d + 4 * (31 - (int32_t)rd) > 0x7FFF) return false;
        for (uint32_t r = rd; r < 32; r++) {
            uint32_t one = (op == 46 ? 32u : 36u) << 26 | r << 21 | ra << 16 | ((uint32_t)(d + 4 * (int32_t)(r - rd)) & 0xFFFF);
            call_op(op == 46 ? "lwz" : "stw", one, a);
        }
        return true;
    }

    void instruction(uint32_t a, uint32_t w) {
        Decoded d = decode(w);
        uint32_t bo = (w >> 21) & 31, bi = (w >> 16) & 31;
        switch (d.kind) {
        case Kind::Op: {
            auto ov = P.immOverride.find(a);  // relocated immediate of a data import
            if (ov != P.immOverride.end()) w = (w & 0xFFFF0000u) | ov->second;
            else if (split_multiple(w, a)) return;
            call_op(d.op, w, a);
            return;
        }
        case Kind::Unhandled:
            if (F->hasExternalLinkage() || !F->getName().ends_with("_s")) S.unhandled++;
            call_unimplemented(a, w);
            return;
        case Kind::Branch: {
            uint32_t tgt = (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a));
            if (w & 1) {
                resume_point(a);
                store32(offsetof(Cpu, lr), a + 4);
                call(a, tgt);
            } else {
                branch(a, tgt);
            }
            return;
        }
        case Kind::BranchCond: {
            uint32_t tgt = (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)a));
            ctr_decrement(bo);
            guarded(condition(bo, bi), [&] {
                if (w & 1) {
                    resume_point(a);
                    store32(offsetof(Cpu, lr), a + 4);
                    call(a, tgt);
                } else {
                    branch(a, tgt);
                }
            });
            return;
        }
        case Kind::BranchLr:
        case Kind::BranchCtr: {
            bool lr = d.kind == Kind::BranchLr;
            ctr_decrement(bo);
            guarded(condition(bo, bi), [&] {
                if (w & 1) {  // blrl / bctrl: call through the register (resuming: set to the callee)
                    resume_point(a);
                    llvm::Value* t = load32(lr ? offsetof(Cpu, lr) : offsetof(Cpu, ctr));
                    store32(offsetof(Cpu, lr), a + 4);
                    store32(offsetof(Cpu, pc), t);
                    dispatch(false);
                } else if (lr) {
                    B.CreateRetVoid();
                } else {
                    indirect_jump(a);
                }
            });
            return;
        }
        }
    }
};

}  // namespace

std::string import_symbol(const Import& i) {
    std::string lib = i.lib;
    size_t rpl = lib.find(".rpl");
    if (rpl != std::string::npos) lib.erase(rpl, 4);
    return "imp_" + c_ident(lib) + "_" + c_ident(i.name);
}

std::unique_ptr<llvm::Module> emit_module(llvm::LLVMContext& ctx, const Program& prog, const Hooks& hooks,
                                          const std::vector<uint32_t>& functions, const std::string& opsBitcode,
                                          const std::string& name, EmitStats& stats, std::string& err) {
    auto buf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(opsBitcode.data(), opsBitcode.size()), "ops.bc", false);
    auto ops = llvm::parseBitcodeFile(buf->getMemBufferRef(), ctx);
    if (!ops) {
        err = "cannot read the ops bitcode: " + llvm::toString(ops.takeError());
        return nullptr;
    }
    std::unique_ptr<llvm::Module> m = std::move(*ops);
    m->setModuleIdentifier(name);
    // the ops are inlined into every module; keep them out of the symbol table
    for (llvm::Function& f : *m)
        if (!f.isDeclaration() && f.getName().starts_with("ppcop_")) f.setLinkage(llvm::GlobalValue::InternalLinkage);
    FunctionEmitter fe(*m, prog, hooks, stats);
    for (uint32_t f : functions) fe.emit(f);
    // the slow entry copies (emit) after all the module's functions: rarely run, they stay out of the
    // way of the code that runs (functions are laid out in this order)
    std::vector<llvm::Function*> slow;
    for (llvm::Function& f : *m)
        if (f.hasInternalLinkage() && f.getName().starts_with("f_") && f.getName().ends_with("_s")) slow.push_back(&f);
    for (llvm::Function* f : slow) {
        f->removeFromParent();
        m->getFunctionList().push_back(f);
    }
    return m;
}

}  // namespace recomp
