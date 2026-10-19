// Guest registers in locals.
//
// Every guest function works on a copy of the Cpu struct in its own stack frame (an alloca the
// emitter passes to the ops instead of the Cpu pointer). After the ops are inlined, this pass makes
// the copy agree with the struct where code outside the function can see it:
// - at entry, every register the function uses is read from the struct;
// - before each call that gets the Cpu (game functions, imports, hooks, instruction hooks,
//   ppc_dispatch, the runtime helpers some ops call), every register the function writes goes back
//   to the struct, and the call gets the struct itself; after it, every register is read again
//   (callee-saved ones too: a save state may replace a waiting thread's registers, and the
//   compiler's save/restore helpers change them on purpose);
// - before a tail call or tail dispatch, every register the function writes goes back;
// - at a return (blr), only what the caller can see under the PowerPC EABI: r1-r4 and r13-r31,
//   f1, f2 and f14-f31, the CR fields cr2-cr4, and the special registers other than LR, CTR and XER.
//   The scratch registers (r0, r5-r12, f0, f3-f13, cr0, cr1, cr5-cr7, LR, CTR, XER) are not
//   written back.
// The copy is then an ordinary local that LLVM's SROA keeps in host registers: no store of a value
// that is overwritten before anyone can see it, no reload after a loop head's memory barrier
// (emit.cpp, loop_barrier), which before reloaded every register on every loop iteration.
//
// A function whose copy is used in any other way (a pointer into it passed on, stored, or selected)
// keeps the struct form: the copy is replaced by the struct pointer, which is the code as before.
//
// WWHD_RECOMP_POISON=1 (a checking build, as the PR's WWHD_RECOMP_LEAF_POISON): at a return, the
// scratch GPRs and FPRs the function wrote get 0xDEADBEEF and NaN, so a caller that relied on one
// breaks visibly.
#include "locals.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <bitset>
#include <map>
#include <set>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ppc.h"

namespace recomp {

const LocalsOptions& locals_options() {
    static const LocalsOptions o = [] {
        LocalsOptions r;
        if (const char* e = getenv("WWHD_RECOMP_LOCALS")) r.on = atoi(e) != 0;
        if (const char* e = getenv("WWHD_RECOMP_POISON")) r.poison = atoi(e) != 0;
        return r;
    }();
    return o;
}

// compile.cpp: hot functions ordered by their calls (WWHD_RECOMP_HOT_ORDER=0: by rank only)
bool hot_call_order() {
    static const bool on = !getenv("WWHD_RECOMP_HOT_ORDER") || atoi(getenv("WWHD_RECOMP_HOT_ORDER")) != 0;
    return on;
}

std::string code_switches() {
    const LocalsOptions& o = locals_options();
    // (emit.cpp: WWHD_RECOMP_LEAF_SPLIT=0 keeps the slow entry path inside functions without calls)
    const bool leaf = !getenv("WWHD_RECOMP_LEAF_SPLIT") || atoi(getenv("WWHD_RECOMP_LEAF_SPLIT")) != 0;
    return std::string("locals ") + (o.on ? "1" : "0") + " poison " + (o.poison ? "1" : "0") + (leaf ? "" : " leafsplit 0") +
           (hot_call_order() ? "" : " hotorder 0");
}

namespace {

enum class Poison { None, Gpr, Fpr };

struct Slot {
    uint32_t off, size;
    llvm::Type* ty;
    bool keepAtReturn;
    Poison poison;
};

// the Cpu fields, one slot per register (or CR bit)
std::vector<Slot> cpu_slots(llvm::LLVMContext& C) {
    std::vector<Slot> s;
    auto* i8 = llvm::Type::getInt8Ty(C);
    auto* i32 = llvm::Type::getInt32Ty(C);
    auto* f64 = llvm::Type::getDoubleTy(C);
    auto* ptr = llvm::PointerType::getUnqual(C);
    for (uint32_t r = 0; r < 32; r++) {
        bool keep = r == 1 || r == 2 || r == 3 || r == 4 || r >= 13;
        s.push_back({(uint32_t)(offsetof(Cpu, r) + 4 * r), 4, i32, keep, keep ? Poison::None : Poison::Gpr});
    }
    s.push_back({(uint32_t)offsetof(Cpu, lr), 4, i32, false, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, ctr), 4, i32, false, Poison::None});
    for (uint32_t b = 0; b < 32; b++) s.push_back({(uint32_t)(offsetof(Cpu, cr) + b), 1, i8, b >= 8 && b < 20, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, xer_so), 1, i8, false, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, xer_ov), 1, i8, false, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, xer_ca), 1, i8, false, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, xer_bc), 1, i8, false, Poison::None});
    for (uint32_t f = 0; f < 32; f++) {
        bool keep = f == 1 || f == 2 || f >= 14;
        for (uint32_t k = 0; k < 2; k++)
            s.push_back({(uint32_t)(offsetof(Cpu, f) + 16 * f + 8 * k), 8, f64, keep, keep ? Poison::None : Poison::Fpr});
    }
    s.push_back({(uint32_t)offsetof(Cpu, fpscr), 4, i32, true, Poison::None});
    for (uint32_t g = 0; g < 8; g++) s.push_back({(uint32_t)(offsetof(Cpu, gqr) + 4 * g), 4, i32, true, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, res_addr), 4, i32, true, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, res_val), 4, i32, true, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, pc), 4, i32, false, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, core), 4, i32, true, Poison::None});
    s.push_back({(uint32_t)offsetof(Cpu, preempt), 4, i32, true, Poison::None});  // read at entry only
    s.push_back({(uint32_t)offsetof(Cpu, thread), 8, ptr, true, Poison::None});
    return s;
}

constexpr size_t kMaxSlots = 160;
using Slots = std::bitset<kMaxSlots>;

// what an instruction does to the copy
struct Effect {
    Slots read, write;
};

thread_local std::string why_text;  // debug (WWHD_RECOMP_LOCALS_LOG): why a function keeps the struct form
bool fail(llvm::Value* v) {
    why_text.clear();
    llvm::raw_string_ostream os(why_text);
    v->print(os);
    return false;
}

// where a pointer into the copy points: byte `off` exactly (span 0), anywhere in [off, off + span)
// (an array indexed by a variable), or unknown (off -1)
struct Where {
    int64_t off = 0, span = 0;
};

// a GEP from `w`: constant indices move the offset; a variable index into an array makes the whole
// array the range (c->cr[i] in mfcr's loop, say); a variable index on the pointer: unknown
Where gep_where(llvm::GetElementPtrInst* g, Where w, const llvm::DataLayout& DL) {
    if (w.off < 0 || w.span) return w;  // unknown, or within the array already
    llvm::Type* cur = g->getSourceElementType();
    int64_t off = w.off;
    bool first = true;
    for (llvm::Value* idx : g->indices()) {
        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(idx);
        if (first) {
            if (!ci) return {-1, 0};
            off += ci->getSExtValue() * (int64_t)DL.getTypeAllocSize(cur);
            first = false;
            continue;
        }
        if (auto* st = llvm::dyn_cast<llvm::StructType>(cur)) {
            off += (int64_t)DL.getStructLayout(st)->getElementOffset((unsigned)ci->getZExtValue());
            cur = st->getElementType((unsigned)ci->getZExtValue());
        } else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(cur)) {
            if (!ci) return {off, (int64_t)DL.getTypeAllocSize(at)};
            off += ci->getSExtValue() * (int64_t)DL.getTypeAllocSize(at->getElementType());
            cur = at->getElementType();
        } else {
            return {-1, 0};
        }
    }
    return {off, 0};
}

class Localizer {
public:
    Localizer(llvm::Function& f, llvm::AllocaInst* a, const LocalsOptions& o, const std::vector<Slot>& s)
        : F(f), A(a), c(f.getArg(0)), opt(o), slots(s), DL(f.getParent()->getDataLayout()) {
        byteSlot.assign(sizeof(Cpu), -1);
        for (size_t i = 0; i < slots.size(); i++)
            for (uint32_t b = slots[i].off; b < slots[i].off + slots[i].size; b++) byteSlot[b] = (int)i;
        // mod packs' code (emit.cpp marks it "wwhd-noeabi") doesn't follow the EABI: it returns values in
        // any register and passes the CR to its own functions (Cemu runs it exactly), so it keeps every
        // register at returns and writes everything back before calls
        noEabi = f.hasFnAttribute("wwhd-noeabi");
        for (size_t i = 0; i < slots.size(); i++) (slots[i].keepAtReturn || noEabi ? keepAtReturn : scratch).set(i);
        // the CR bits (but bit 6, the vararg float flag) and XER: not read by an EABI callee (writeback)
        auto mark = [&](uint32_t off) {
            for (size_t i = 0; i < slots.size(); i++)
                if (slots[i].off == off) passThrough.set(i);
        };
        for (uint32_t b = 0; b < 32; b++)
            if (b != 6) mark((uint32_t)(offsetof(Cpu, cr) + b));
        for (uint32_t x : {offsetof(Cpu, xer_so), offsetof(Cpu, xer_ov), offsetof(Cpu, xer_ca), offsetof(Cpu, xer_bc)}) mark((uint32_t)x);
    }

    // false: the copy was replaced by the struct (the function keeps the struct form)
    bool run() {
        if (!walk(A, Where{}) || !struct_only_in_calls()) {
            static const bool why = getenv("WWHD_RECOMP_LOCALS_LOG") != nullptr;
            if (why) fprintf(stderr, "fallback %s: %s\n", F.getName().str().c_str(), why_text.c_str());
            fallback();
            return false;
        }
        for (llvm::Use& cu : c->uses())
            if (auto* cb = llvm::dyn_cast<llvm::CallBase>(cu.getUser())) calls.insert(cb);
        dirty_analysis();
        live_analysis();
        if (getenv("WWHD_RECOMP_LOCALS_LOG")) log_size();
        // the copies, placed after both analyses (they look at the original instructions)
        llvm::IRBuilder<> B(F.getContext());
        B.SetInsertPoint(&*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
        copy(B, liveIn.at(&F.getEntryBlock()), true);
        for (llvm::CallBase* cb : calls) {
            B.SetInsertPoint(cb);
            copy(B, writeback(cb, liveAfter.at(cb)), false);  // the struct current for the callee
            for (llvm::Use& arg : cb->args())
                if (arg.get() == A) arg.set(c);
            if (cb->isMustTailCall()) continue;  // a ret follows
            B.SetInsertPoint(cb->getNextNode());
            copy(B, liveAfter.at(cb), true);  // what the function reads before writing it again
        }
        for (auto& [r, d] : dirtyAtRet) {
            B.SetInsertPoint(r);
            copy(B, d & keepAtReturn, false);
            if (opt.poison) poison(B, d & scratch);
        }
        return true;
    }

private:
    llvm::Function& F;
    llvm::AllocaInst* A;
    llvm::Value* c;
    const LocalsOptions& opt;
    const std::vector<Slot>& slots;
    const llvm::DataLayout& DL;
    std::vector<int> byteSlot;
    Slots keepAtReturn, scratch;
    std::map<llvm::Instruction*, Effect> effects;  // accesses to the copy
    std::set<llvm::CallBase*> calls;               // sync points: calls that get the Cpu
    std::map<llvm::CallBase*, Slots> dirtyAt, liveAfter;
    Slots passThrough;  // CR bits and XER: see writeback
    // A call into game code (f_*), an import, a hook or ppc_dispatch follows the PowerPC EABI: the
    // callee reads the CR only for bit 6 (the vararg float flag), and XER not at all, while the bits
    // the caller reads after the call pass through it unchanged (the original project's PR #83,
    // "condition-register liveness"). So the CR bits and XER are written back before such a call
    // only if read after it (they are read back then); compares mostly set bits nobody reads after
    // the next branch. GPRs and FPRs are all written back: the compiler's register save helpers
    // (_savegpr_N: the frame in r11, LR kept in r0 across the call) don't follow the EABI.
    // Instruction hooks (site_*), the runtime's helpers and tail calls get every changed register.
    bool noEabi = false;
    bool eabi_call(llvm::CallBase* cb) const {
        if (noEabi || cb->isMustTailCall()) return false;
        llvm::Function* f = cb->getCalledFunction();
        if (!f) return !cb->isInlineAsm();  // the emitter's inline dispatch (a game function or ppc_dispatch)
        llvm::StringRef n = f->getName();
        return n.starts_with("f_") || n.starts_with("imp_") || n.starts_with("hook_") || n == "ppc_dispatch";
    }
    Slots writeback(llvm::CallBase* cb, const Slots& liveAfterCall) {
        return eabi_call(cb) ? dirtyAt.at(cb) & ~(passThrough & ~liveAfterCall) : dirtyAt.at(cb);
    }
    std::vector<std::pair<llvm::ReturnInst*, Slots>> dirtyAtRet;
    std::map<llvm::BasicBlock*, Slots> liveIn;

    Slots range(Where w, int64_t size) {
        Slots s;
        int64_t lo = w.off, hi = w.span ? w.off + w.span : w.off + size;
        if (w.off < 0 || size < 0 || hi > (int64_t)byteSlot.size()) return s.set();  // unknown: all
        for (int64_t b = lo; b < hi; b++)
            if (byteSlot[b] >= 0) s.set(byteSlot[b]);
        return s;
    }
    // the slots an access fully overwrites (a partial or ranged write also reads: the rest stays)
    Slots covered(Where w, int64_t size) {
        Slots s;
        if (w.off < 0 || w.span || size < 0) return s;
        for (size_t i = 0; i < slots.size(); i++)
            if (slots[i].off >= w.off && slots[i].off + slots[i].size <= w.off + size) s.set(i);
        return s;
    }
    void access(llvm::Instruction* i, Where w, int64_t size, bool isRead, bool isWrite) {
        Effect& e = effects[i];
        Slots r = range(w, size);
        if (isRead) e.read |= r;
        if (isWrite) {
            Slots full = covered(w, size);
            e.write |= r;
            e.read |= r & ~full;  // the bytes it leaves keep their old value
        }
    }
    // the uses of `p` (pointing at `w` in the copy); false if the copy escapes
    bool walk(llvm::Value* p, Where w) {
        for (llvm::User* user : p->users()) {
            if (auto* g = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                if (g->getPointerOperand() != p) return fail(g);
                if (!walk(g, gep_where(g, w, DL))) return false;
            } else if (auto* l = llvm::dyn_cast<llvm::LoadInst>(user)) {
                if (l->isVolatile() || l->isAtomic()) return fail(l);
                access(l, w, (int64_t)DL.getTypeStoreSize(l->getType()), true, false);
            } else if (auto* s = llvm::dyn_cast<llvm::StoreInst>(user)) {
                if (s->getValueOperand() == p || s->isVolatile() || s->isAtomic()) return fail(s);
                access(s, w, (int64_t)DL.getTypeStoreSize(s->getValueOperand()->getType()), false, true);
            } else if (auto* mi = llvm::dyn_cast<llvm::MemIntrinsic>(user)) {
                auto* len = llvm::dyn_cast<llvm::ConstantInt>(mi->getLength());
                int64_t n = len ? (int64_t)len->getZExtValue() : -1;
                if (auto* mt = llvm::dyn_cast<llvm::MemTransferInst>(mi))
                    if (mt->getRawSource() == p) access(mi, w, n, true, false);
                if (mi->getRawDest() == p) access(mi, w, n, false, true);
            } else if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
                if (!ii->isLifetimeStartOrEnd()) return fail(ii);
            } else if (auto* cb = llvm::dyn_cast<llvm::CallBase>(user)) {
                // a call gets the struct instead: only the copy itself, not a pointer into it
                if (p != A || cb->isInlineAsm() || cb->getCalledOperand() == p) return fail(cb);
                calls.insert(cb);
            } else {
                return fail(user);
            }
        }
        return true;
    }
    // the Cpu pointer itself may only be passed to calls (the emitter's calls); any other access
    // would miss the copy's newer values
    bool struct_only_in_calls() {
        for (llvm::User* user : c->users()) {
            auto* cb = llvm::dyn_cast<llvm::CallBase>(user);
            if (!cb || cb->isInlineAsm() || cb->getCalledOperand() == c) return fail(user);
        }
        return true;
    }
    bool is_sync(llvm::Instruction& i) {
        auto* cb = llvm::dyn_cast<llvm::CallBase>(&i);
        return cb && calls.count(cb);
    }

    // forward: the slots written since the struct was last made current ("dirty"). A call makes the
    // struct current (before it) and leaves the copy current (after it).
    void dirty_analysis() {
        std::map<llvm::BasicBlock*, Slots> in;
        std::vector<llvm::BasicBlock*> work{&F.getEntryBlock()};
        in[&F.getEntryBlock()] = Slots();
        std::set<llvm::BasicBlock*> queued{&F.getEntryBlock()};
        auto through = [&](llvm::BasicBlock* bb, bool record) {
            Slots d = in[bb];
            for (llvm::Instruction& i : *bb) {
                if (is_sync(i)) {
                    auto* cb = llvm::cast<llvm::CallBase>(&i);
                    if (record) dirtyAt[cb] = d;
                    // the CR bits and XER an EABI call may not get (writeback) stay changed: a later
                    // tail call or return still writes them back
                    if (eabi_call(cb)) d &= passThrough;
                    else d.reset();
                    continue;
                }
                auto e = effects.find(&i);
                if (e != effects.end()) d |= e->second.write;
                if (record)
                    if (auto* r = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
                        auto* prev = llvm::dyn_cast_or_null<llvm::CallInst>(r->getPrevNode());
                        if (!prev || !prev->isMustTailCall()) dirtyAtRet.push_back({r, d});
                    }
            }
            return d;
        };
        while (!work.empty()) {
            llvm::BasicBlock* bb = work.back();
            work.pop_back();
            queued.erase(bb);
            Slots out = through(bb, false);
            for (llvm::BasicBlock* s : llvm::successors(bb)) {
                auto it = in.find(s);
                if (it != in.end() && (out & ~it->second).none()) continue;
                in[s] |= out;
                if (queued.insert(s).second) work.push_back(s);
            }
        }
        for (llvm::BasicBlock& bb : F)
            if (in.count(&bb)) through(&bb, true);
            else for (llvm::Instruction& i : bb)  // unreachable: everything
                if (is_sync(i)) dirtyAt[llvm::cast<llvm::CallBase>(&i)] = Slots().set();
                else if (auto* r = llvm::dyn_cast<llvm::ReturnInst>(&i)) dirtyAtRet.push_back({r, Slots().set()});
    }

    // backward: the slots read before being written again ("live"). A call reads the dirty slots
    // (written back before it) and the copy is reloaded after it with what is live there; a return
    // reads what it writes back.
    void live_analysis() {
        std::map<llvm::ReturnInst*, Slots> retReads;
        for (auto& [r, d] : dirtyAtRet) retReads[r] = d & keepAtReturn;  // (poison stores constants)
        std::map<llvm::BasicBlock*, Slots> out;
        auto through = [&](llvm::BasicBlock* bb, bool record) {
            Slots l = out[bb];
            for (auto it = bb->rbegin(); it != bb->rend(); ++it) {
                llvm::Instruction& i = *it;
                if (auto* r = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
                    auto rr = retReads.find(r);
                    l = rr != retReads.end() ? rr->second : Slots();
                    continue;
                }
                if (is_sync(i)) {
                    auto* cb = llvm::cast<llvm::CallBase>(&i);
                    const Slots after = cb->isMustTailCall() ? Slots() : l;
                    if (record) liveAfter[cb] = after;
                    l = writeback(cb, after);
                    continue;
                }
                auto e = effects.find(&i);
                if (e != effects.end()) l = (l & ~(e->second.write & ~e->second.read)) | e->second.read;
            }
            return l;
        };
        std::vector<llvm::BasicBlock*> work;
        std::set<llvm::BasicBlock*> queued;
        for (llvm::BasicBlock& bb : F) {
            work.push_back(&bb);
            queued.insert(&bb);
        }
        while (!work.empty()) {
            llvm::BasicBlock* bb = work.back();
            work.pop_back();
            queued.erase(bb);
            Slots l = through(bb, false);
            Slots& old = liveIn[bb];
            if ((l & ~old).none() && liveInDone.count(bb)) continue;
            old |= l;
            liveInDone.insert(bb);
            for (llvm::BasicBlock* p : llvm::predecessors(bb)) {
                out[p] |= old;
                if (queued.insert(p).second) work.push_back(p);
            }
        }
        for (llvm::BasicBlock& bb : F) through(&bb, true);
    }
    std::set<llvm::BasicBlock*> liveInDone;

    void log_size() {
        size_t insts = 0, wb = 0, rl = 0;
        for (llvm::BasicBlock& bb : F) insts += bb.size();
        for (auto& [cb, d] : dirtyAt) wb += writeback(cb, liveAfter.at(cb)).count();
        for (auto& [cb, l] : liveAfter) rl += l.count();
        fprintf(stderr, "locals %s insts %zu calls %zu writebacks %zu reloads %zu\n", F.getName().str().c_str(), insts, calls.size(), wb, rl);
    }
    // checking build: garbage into the struct's GPRs and FPRs among `which`
    void poison(llvm::IRBuilder<>& B, const Slots& which) {
        for (size_t i = 0; i < slots.size(); i++) {
            if (!which.test(i)) continue;
            const Slot& s = slots[i];
            llvm::Value* v = s.poison == Poison::Gpr   ? (llvm::Value*)B.getInt32(0xDEADBEEFu)
                             : s.poison == Poison::Fpr ? llvm::ConstantFP::getNaN(s.ty)
                                                       : nullptr;
            if (v) B.CreateStore(v, at(B, c, s.off));
        }
    }
    llvm::Value* at(llvm::IRBuilder<>& B, llvm::Value* p, uint32_t off) {
        return off ? B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), p, off) : p;
    }
    // toCopy: struct -> copy; else copy -> struct
    void copy(llvm::IRBuilder<>& B, const Slots& which, bool toCopy) {
        for (size_t i = 0; i < slots.size(); i++) {
            if (!which.test(i)) continue;
            const Slot& s = slots[i];
            llvm::Value* from = at(B, toCopy ? c : (llvm::Value*)A, s.off);
            llvm::Value* to = at(B, toCopy ? (llvm::Value*)A : c, s.off);
            B.CreateStore(B.CreateLoad(s.ty, from), to);
        }
    }
    void fallback() {
        A->replaceAllUsesWith(c);
        A->eraseFromParent();
    }
};

}  // namespace

void localize_registers(llvm::Module& m, const LocalsOptions& opt, LocalsStats& st) {
    std::vector<Slot> slots = cpu_slots(m.getContext());
    for (llvm::Function& f : m) {
        if (f.isDeclaration() || f.arg_empty()) continue;
        llvm::AllocaInst* a = nullptr;  // among the entry block's allocas (the inlined ops add theirs before it)
        for (llvm::Instruction& i : f.getEntryBlock()) {
            auto* ai = llvm::dyn_cast<llvm::AllocaInst>(&i);
            if (!ai) break;
            if (ai->getName() == kLocalCpuName) a = ai;
        }
        if (!a) continue;
        if (!opt.on) {
            a->replaceAllUsesWith(f.getArg(0));
            a->eraseFromParent();
            continue;
        }
        Localizer l(f, a, opt, slots);
        if (l.run()) st.functions++;
        else st.fallback++;
    }
}

}  // namespace recomp
