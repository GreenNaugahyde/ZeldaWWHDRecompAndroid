// LLVM IR emission for recompiled guest functions (see emit.h). Control flow follows
// tools/recomp/recomp.py's Ctx callbacks and ppc2c.py's branch translation.
#include "emit.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/MemoryBuffer.h>

#include <cstddef>
#include <cstdio>
#include <map>
#include <sstream>

#include "decode.h"
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
        if (line[0] == '@') sites.insert((uint32_t)strtoul(line.c_str() + 1, nullptr, 16));
        else functions.insert((uint32_t)strtoul(line.c_str(), nullptr, 16));
    }
}

namespace {

int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

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
            llvm::Function* w = guest("f_" + hex8(start));
            auto* bb = llvm::BasicBlock::Create(C, "", w);
            B.SetInsertPoint(bb);
            B.CreateCall(external("hook_" + hex8(start)), {w->getArg(0)});
            B.CreateRetVoid();
        }
        F = guest(hooked ? "f_" + hex8(start) + "_orig" : "f_" + hex8(start));
        c = F->getArg(0);
        auto* entry = llvm::BasicBlock::Create(C, "entry", F);  // first: the function's entry block
        collect_labels();
        B.SetInsertPoint(entry);
        call_op("enter", 0, start);
        for (uint32_t a = start; a < end; a += 4) {
            auto it = blocks.find(a);
            if (it != blocks.end()) {
                if (!B.GetInsertBlock()->getTerminator()) B.CreateBr(it->second);
                B.SetInsertPoint(it->second);
                if (loopHeads.count(a)) loop_barrier();
            } else if (B.GetInsertBlock()->getTerminator()) {
                B.SetInsertPoint(llvm::BasicBlock::Create(C, "", F));  // unreachable code after a jump
            }
            if (H.sites.count(a)) B.CreateCall(external("site_" + hex8(a)), {c});
            instruction(a, P.word(a));
            S.instructions++;
        }
        if (!B.GetInsertBlock()->getTerminator()) {  // falls through into the next function
            if (end < P.textHi) {
                std::string next = "f_" + hex8(end) + (H.functions.count(end) ? "_orig" : "");
                tail(guest(next));
            } else {
                call_unimplemented(end, 0);
                B.CreateRetVoid();
            }
        }
        S.functions++;
    }

private:
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
    llvm::Value* c = nullptr;
    uint32_t cur = 0, end = 0;
    std::map<uint32_t, llvm::BasicBlock*> blocks;
    std::set<uint32_t> loopHeads;

    // ---- symbols
    llvm::Function* guest(const std::string& name) {
        llvm::Function* f = M.getFunction(name);
        if (!f) {
            f = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, M);
            f->addParamAttr(0, llvm::Attribute::NoAlias);  // guest memory never aliases the register file
        }
        return f;
    }
    llvm::Function* external(const std::string& name) {
        llvm::Function* f = M.getFunction(name);
        return f ? f : llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, M);
    }
    std::string import_name(uint32_t slot) { return import_symbol(P.imports.at(slot)); }

    // ---- Cpu fields
    llvm::Value* field(size_t off) { return B.CreateConstInBoundsGEP1_64(i8, c, off); }
    llvm::Value* load32(size_t off) { return B.CreateLoad(i32, field(off)); }
    void store32(size_t off, llvm::Value* v) { B.CreateStore(v, field(off)); }
    void store32(size_t off, uint32_t v) { store32(off, B.getInt32(v)); }
    llvm::Value* cr(uint32_t bit) { return B.CreateLoad(i8, field(offsetof(Cpu, cr) + bit)); }

    // ---- calls
    void call_op(const char* name, uint32_t w, uint32_t addr) {
        llvm::Function* f = M.getFunction(std::string("ppcop_") + name);
        B.CreateCall(f, {c, B.getInt32(w), B.getInt32(addr)});
    }
    void call_unimplemented(uint32_t addr, uint32_t w) { call_op("unimplemented", w, addr); }
    void tail(llvm::Function* f) {
        llvm::CallInst* ci = B.CreateCall(f, {c});
        ci->setTailCallKind(llvm::CallInst::TCK_MustTail);
        B.CreateRetVoid();
    }
    void dispatch(bool isTail) {
        llvm::Function* d = external("ppc_dispatch");
        if (isTail) tail(d);
        else B.CreateCall(d, {c});
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

    // ---- control flow (recomp.py's Ctx callbacks)
    void branch(uint32_t addr, uint32_t tgt) {
        auto ic = P.importCalls.find(addr);
        if (ic != P.importCalls.end()) return tail(external(import_name(ic->second)));
        if (tgt >= cur && tgt < end) {
            B.CreateBr(blocks.at(tgt));
            return;
        }
        if (std::binary_search(P.entries.begin(), P.entries.end(), tgt)) return tail(guest("f_" + hex8(tgt)));
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
            B.CreateCall(guest("f_" + hex8(tgt)), {c});
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

    void instruction(uint32_t a, uint32_t w) {
        Decoded d = decode(w);
        uint32_t bo = (w >> 21) & 31, bi = (w >> 16) & 31;
        switch (d.kind) {
        case Kind::Op: {
            auto ov = P.immOverride.find(a);  // relocated immediate of a data import
            if (ov != P.immOverride.end()) w = (w & 0xFFFF0000u) | ov->second;
            call_op(d.op, w, a);
            return;
        }
        case Kind::Unhandled:
            S.unhandled++;
            call_unimplemented(a, w);
            return;
        case Kind::Branch: {
            uint32_t tgt = (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a));
            if (w & 1) {
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
                if (w & 1) {  // blrl / bctrl: call through the register
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
    return m;
}

}  // namespace recomp
