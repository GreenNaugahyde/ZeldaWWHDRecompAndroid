// Instruction classification (see decode.h); the same cases as tools/recomp/ppc2c.py.
#include "decode.h"

namespace recomp {
namespace {
Decoded op(const char* n) { return {Kind::Op, n}; }
Decoded unhandled() { return {Kind::Unhandled, "unimplemented"}; }

Decoded decode31(uint32_t w) {
    uint32_t xo = (w >> 1) & 0x3FF, xo9 = (w >> 1) & 0x1FF;
    switch (xo9) {  // XO-form arithmetic (OE in bit 10)
    case 266: return op("add");
    case 40: return op("subf");
    case 10: return op("addc");
    case 138: return op("adde");
    case 8: return op("subfc");
    case 136: return op("subfe");
    case 234: return op("addme");
    case 202: return op("addze");
    case 232: return op("subfme");
    case 200: return op("subfze");
    case 104: return op("neg");
    case 235: return op("mullw");
    case 75: return op("mulhw");
    case 11: return op("mulhwu");
    case 491: return op("divw");
    case 459: return op("divwu");
    default: break;
    }
    switch (xo) {
    case 28: return op("and");
    case 444: return op("or");
    case 316: return op("xor");
    case 60: return op("andc");
    case 412: return op("orc");
    case 476: return op("nand");
    case 124: return op("nor");
    case 284: return op("eqv");
    case 24: return op("slw");
    case 536: return op("srw");
    case 792: return op("sraw");
    case 824: return op("srawi");
    case 26: return op("cntlzw");
    case 954: return op("extsb");
    case 922: return op("extsh");
    case 0: return op("cmp");
    case 32: return op("cmpl");
    case 23: return op("lwzx");
    case 87: return op("lbzx");
    case 279: return op("lhzx");
    case 343: return op("lhax");
    case 55: return op("lwzux");
    case 119: return op("lbzux");
    case 311: return op("lhzux");
    case 375: return op("lhaux");
    case 151: return op("stwx");
    case 215: return op("stbx");
    case 407: return op("sthx");
    case 183: return op("stwux");
    case 247: return op("stbux");
    case 439: return op("sthux");
    case 534: return op("lwbrx");
    case 790: return op("lhbrx");
    case 662: return op("stwbrx");
    case 918: return op("sthbrx");
    case 20: return op("lwarx");
    case 150: return op("stwcx");
    case 597: return op("lswi");
    case 725: return op("stswi");
    case 535: return op("lfsx");
    case 567: return op("lfsux");
    case 599: return op("lfdx");
    case 631: return op("lfdux");
    case 663: return op("stfsx");
    case 695: return op("stfsux");
    case 727: return op("stfdx");
    case 759: return op("stfdux");
    case 983: return op("stfiwx");
    case 1014: return op("dcbz");
    case 598: case 854: return op("sync");
    case 86: case 54: case 278: case 246: case 470: case 982: return op("nop");
    case 339: return op("mfspr");
    case 467: return op("mtspr");
    case 371: return op("mftb");
    case 19: return op("mfcr");
    case 144: return op("mtcrf");
    case 512: return op("mcrxr");
    case 4: return op("tw");
    default: return unhandled();
    }
}

Decoded decode59(uint32_t w) {
    switch ((w >> 1) & 31) {
    case 18: return op("fdivs");
    case 20: return op("fsubs");
    case 21: return op("fadds");
    case 25: return op("fmuls");
    case 28: return op("fmsubs");
    case 29: return op("fmadds");
    case 30: return op("fnmsubs");
    case 31: return op("fnmadds");
    case 24: return op("fres");
    default: return unhandled();
    }
}

Decoded decode63(uint32_t w) {
    switch ((w >> 1) & 31) {
    case 18: return op("fdiv");
    case 20: return op("fsub");
    case 21: return op("fadd");
    case 22: return op("fsqrt");
    case 23: return op("fsel");
    case 25: return op("fmul");
    case 26: return op("frsqrte");
    case 28: return op("fmsub");
    case 29: return op("fmadd");
    case 30: return op("fnmsub");
    case 31: return op("fnmadd");
    default: break;
    }
    switch ((w >> 1) & 0x3FF) {
    case 0: case 32: return op("fcmp");
    case 12: return op("frsp");
    case 15: return op("fctiwz");
    case 14: return op("fctiw");
    case 72: return op("fmr");
    case 40: return op("fneg");
    case 264: return op("fabs");
    case 136: return op("fnabs");
    case 583: return op("mffs");
    case 711: return op("mtfsf");
    case 134: return op("mtfsfi");
    case 38: return op("mtfsb1");
    case 70: return op("mtfsb0");
    case 64: return op("mcrfs");
    default: return unhandled();
    }
}

Decoded decode4(uint32_t w) {
    switch ((w >> 1) & 31) {
    case 10: return op("ps_sum0");
    case 11: return op("ps_sum1");
    case 12: return op("ps_muls0");
    case 13: return op("ps_muls1");
    case 14: return op("ps_madds0");
    case 15: return op("ps_madds1");
    case 18: return op("ps_div");
    case 20: return op("ps_sub");
    case 21: return op("ps_add");
    case 23: return op("ps_sel");
    case 24: return op("ps_res");
    case 25: return op("ps_mul");
    case 26: return op("ps_rsqrte");
    case 28: return op("ps_msub");
    case 29: return op("ps_madd");
    case 30: return op("ps_nmsub");
    case 31: return op("ps_nmadd");
    case 6: case 7: {
        uint32_t xo6 = (w >> 1) & 0x3F;
        bool upd = xo6 == 38 || xo6 == 39;
        if (((w >> 1) & 31) == 6) return op(upd ? "psq_lux" : "psq_lx");
        return op(upd ? "psq_stux" : "psq_stx");
    }
    default: break;
    }
    switch ((w >> 1) & 0x3FF) {
    case 0: case 32: return op("ps_cmp0");
    case 64: case 96: return op("ps_cmp1");
    case 40: return op("ps_neg");
    case 72: return op("ps_mr");
    case 136: return op("ps_nabs");
    case 264: return op("ps_abs");
    case 528: return op("ps_merge00");
    case 560: return op("ps_merge01");
    case 592: return op("ps_merge10");
    case 624: return op("ps_merge11");
    case 1014: return op("dcbz_l");
    default: return unhandled();
    }
}
}  // namespace

Decoded decode(uint32_t w) {
    switch (w >> 26) {
    case 18: return {Kind::Branch, nullptr};
    case 16: return {Kind::BranchCond, nullptr};
    case 19:
        switch ((w >> 1) & 0x3FF) {
        case 16: return {Kind::BranchLr, nullptr};
        case 528:
            if (!(((w >> 21) & 31) & 0x04)) return unhandled();  // bcctr with ctr decrement
            return {Kind::BranchCtr, nullptr};
        case 257: return op("crand");
        case 449: return op("cror");
        case 193: return op("crxor");
        case 225: return op("crnand");
        case 33: return op("crnor");
        case 289: return op("creqv");
        case 129: return op("crandc");
        case 417: return op("crorc");
        case 0: return op("mcrf");
        case 150: return op("isync");
        default: return unhandled();
        }
    case 14: return op("addi");
    case 15: return op("addis");
    case 12: case 13: return op("addic");
    case 8: return op("subfic");
    case 7: return op("mulli");
    case 10: return op("cmpli");
    case 11: return op("cmpi");
    case 24: return op("ori");
    case 25: return op("oris");
    case 26: return op("xori");
    case 27: return op("xoris");
    case 28: return op("andi");
    case 29: return op("andis");
    case 21: return op("rlwinm");
    case 20: return op("rlwimi");
    case 23: return op("rlwnm");
    case 32: return op("lwz");
    case 33: return op("lwzu");
    case 34: return op("lbz");
    case 35: return op("lbzu");
    case 40: return op("lhz");
    case 41: return op("lhzu");
    case 42: return op("lha");
    case 43: return op("lhau");
    case 36: return op("stw");
    case 37: return op("stwu");
    case 38: return op("stb");
    case 39: return op("stbu");
    case 44: return op("sth");
    case 45: return op("sthu");
    case 46: return op("lmw");
    case 47: return op("stmw");
    case 48: return op("lfs");
    case 49: return op("lfsu");
    case 50: return op("lfd");
    case 51: return op("lfdu");
    case 52: return op("stfs");
    case 53: return op("stfsu");
    case 54: return op("stfd");
    case 55: return op("stfdu");
    case 56: return op("psq_l");
    case 57: return op("psq_lu");
    case 60: return op("psq_st");
    case 61: return op("psq_stu");
    case 31: return decode31(w);
    case 59: return decode59(w);
    case 63: return decode63(w);
    case 4: return decode4(w);
    case 3: return op("twi");
    case 17: return op("unimplemented");  // sc
    default: return unhandled();
    }
}

}  // namespace recomp
