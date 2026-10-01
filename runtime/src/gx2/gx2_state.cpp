// GX2 fixed-function state: blend, depth/stencil, rasterizer, viewport, etc.
// Register packing follows the GX2 SDK (as documented by Cemu's GX2_State.cpp).
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "runtime.h"

using namespace Latte;

namespace gx2 {
static float fa(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }  // i-th float argument
}  // namespace gx2

using gx2::fa;
using gx2::set_reg;

// ---------------------------------------------------------------- alpha test
HLE(gx2, GX2SetAlphaTest) {
    LATTE_SX_ALPHA_TEST_CONTROL ctrl;
    ctrl.set_ALPHA_FUNC((LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC)arg(c, 1));
    ctrl.set_ALPHA_TEST_ENABLE(arg(c, 0) != 0);
    LATTE_SX_ALPHA_REF ref;
    ref.set_ALPHA_TEST_REF(fa(c, 0));
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, ctrl.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, ref.getRawValue());
}
HLE(gx2, GX2SetAlphaTestReg) {
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, ld32(arg(c, 0)));
    set_reg(REGADDR::SX_ALPHA_REF, ld32(arg(c, 0) + 4));
}
HLE(gx2, GX2SetAlphaToMask) {
    // (enable, GX2AlphaToMaskMode) -> DB_ALPHA_TO_MASK
    uint32 enable = arg(c, 0) & 1, mode = arg(c, 1);
    static const uint32 offsets[] = {0xAA, 0x78, 0xB4, 0x1E, 0x00};  // dither patterns per mode
    uint32 v = enable | ((mode < 5 ? offsets[mode] : 0) << 8);
    set_reg(mmDB_ALPHA_TO_MASK, v);
}

// ---------------------------------------------------------------- color / blend
HLE(gx2, GX2SetColorControl) {
    LATTE_CB_COLOR_CONTROL r;
    r.set_MULTIWRITE_ENABLE(arg(c, 2) != 0);
    r.set_SPECIAL_OP(arg(c, 3) == 0 ? LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE : LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL);
    r.set_BLEND_MASK(arg(c, 1));
    r.set_ROP((LATTE_CB_COLOR_CONTROL::E_LOGICOP)arg(c, 0));
    set_reg(REGADDR::CB_COLOR_CONTROL, r.getRawValue());
}
HLE(gx2, GX2SetColorControlReg) { set_reg(REGADDR::CB_COLOR_CONTROL, ld32(arg(c, 0))); }

HLE(gx2, GX2SetBlendControl) {
    LATTE_CB_BLENDN_CONTROL r;
    using BF = LATTE_CB_BLENDN_CONTROL::E_BLENDFACTOR;
    using CF = LATTE_CB_BLENDN_CONTROL::E_COMBINEFUNC;
    r.set_COLOR_SRCBLEND((BF)arg(c, 1));
    r.set_COLOR_DSTBLEND((BF)arg(c, 2));
    r.set_COLOR_COMB_FCN((CF)arg(c, 3));
    r.set_SEPARATE_ALPHA_BLEND(arg(c, 4) != 0);
    r.set_ALPHA_SRCBLEND((BF)arg(c, 5));
    r.set_ALPHA_DSTBLEND((BF)arg(c, 6));
    r.set_ALPHA_COMB_FCN((CF)arg(c, 7));
    set_reg(REGADDR::CB_BLEND0_CONTROL + (arg(c, 0) & 7), r.getRawValue());
}
HLE(gx2, GX2SetBlendControlReg) { set_reg(REGADDR::CB_BLEND0_CONTROL + (ld32(arg(c, 0)) & 7), ld32(arg(c, 0) + 4)); }

HLE(gx2, GX2SetBlendConstantColor) {
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, gx2::fbits(fa(c, i)));
}
HLE(gx2, GX2SetBlendConstantColorReg) {
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, ld32(arg(c, 0) + 4 * i));
}

HLE(gx2, GX2SetTargetChannelMasks) {
    uint32 m = 0;
    for (int i = 0; i < 8; i++) m |= (arg(c, i) & 0xF) << (4 * i);
    set_reg(REGADDR::CB_TARGET_MASK, m);
}

// ---------------------------------------------------------------- depth / stencil
HLE(gx2, GX2SetDepthStencilControl) {
    // (depthTest, depthWrite, depthFunc, stencilTest, backStencilTest, frontFunc, frontZPass, frontZFail,
    //  frontFail, backFunc, backZPass, backZFail, backFail) - args past r10 are on the stack
    uint32 a[13];
    for (int i = 0; i < 8; i++) a[i] = arg(c, i);
    for (int i = 8; i < 13; i++) a[i] = ld32(c->r[1] + 8 + 4 * (i - 8));
    using SF = LATTE_DB_DEPTH_CONTROL::E_STENCILFUNC;
    using SA = LATTE_DB_DEPTH_CONTROL::E_STENCILACTION;
    LATTE_DB_DEPTH_CONTROL r;
    r.set_Z_ENABLE(a[0] != 0).set_Z_WRITE_ENABLE(a[1] != 0).set_Z_FUNC((LATTE_DB_DEPTH_CONTROL::E_ZFUNC)a[2]);
    r.set_STENCIL_ENABLE(a[3] != 0).set_BACK_STENCIL_ENABLE(a[4] != 0);
    r.set_STENCIL_FUNC_F((SF)a[5]).set_STENCIL_ZPASS_F((SA)a[6]).set_STENCIL_ZFAIL_F((SA)a[7]).set_STENCIL_FAIL_F((SA)a[8]);
    r.set_STENCIL_FUNC_B((SF)a[9]).set_STENCIL_ZPASS_B((SA)a[10]).set_STENCIL_ZFAIL_B((SA)a[11]).set_STENCIL_FAIL_B((SA)a[12]);
    set_reg(REGADDR::DB_DEPTH_CONTROL, r.getRawValue());
}
HLE(gx2, GX2SetDepthStencilControlReg) { set_reg(REGADDR::DB_DEPTH_CONTROL, ld32(arg(c, 0))); }

HLE(gx2, GX2SetStencilMask) {
    LATTE_DB_STENCILREFMASK f;
    f.set_STENCILREF_F(arg(c, 2) & 0xFF).set_STENCILMASK_F(arg(c, 0) & 0xFF).set_STENCILWRITEMASK_F(arg(c, 1) & 0xFF);
    LATTE_DB_STENCILREFMASK_BF b;
    b.set_STENCILREF_B(arg(c, 5) & 0xFF).set_STENCILMASK_B(arg(c, 3) & 0xFF).set_STENCILWRITEMASK_B(arg(c, 4) & 0xFF);
    set_reg(REGADDR::DB_STENCILREFMASK, f.getRawValue());
    set_reg(REGADDR::DB_STENCILREFMASK_BF, b.getRawValue());
}

// ---------------------------------------------------------------- rasterizer
HLE(gx2, GX2SetPolygonControl) {
    LATTE_PA_SU_SC_MODE_CNTL v;
    v.set_FRONT_FACE((LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE)arg(c, 0));
    v.set_CULL_FRONT((arg(c, 1) & 1) != 0);
    v.set_CULL_BACK((arg(c, 2) & 1) != 0);
    v.set_POLYGON_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_POLYGONMODE)arg(c, 3));
    v.set_FRONT_POLY_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE)arg(c, 4));
    v.set_BACK_POLY_MODE((LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE)arg(c, 5));
    v.set_OFFSET_FRONT_ENABLED((arg(c, 6) & 1) != 0);
    v.set_OFFSET_BACK_ENABLED((arg(c, 7) & 1) != 0);
    v.set_OFFSET_PARA_ENABLED((ld32(c->r[1] + 8) & 1) != 0);  // 9th argument
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, v.getRawValue());
}
HLE(gx2, GX2SetPolygonControlReg) { set_reg(REGADDR::PA_SU_SC_MODE_CNTL, ld32(arg(c, 0))); }

HLE(gx2, GX2SetPolygonOffsetReg) {
    uint32 r = arg(c, 0);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE + i, ld32(r + 4 * i));
    set_reg(REGADDR::PA_SU_POLY_OFFSET_CLAMP, ld32(r + 16));
}

HLE(gx2, GX2SetRasterizerClipControl) {
    LATTE_PA_CL_CLIP_CNTL r;
    bool raster = arg(c, 0) & 1, zclip = arg(c, 1) & 1;
    r.set_ZCLIP_NEAR_DISABLE(!zclip).set_ZCLIP_FAR_DISABLE(!zclip);
    r.set_DX_RASTERIZATION_KILL(!raster);
    r.set_DX_CLIP_SPACE_DEF(false);
    r.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, r.getRawValue());
}

HLE(gx2, GX2SetViewport) {
    float x = fa(c, 0), y = fa(c, 1), w = fa(c, 2), h = fa(c, 3), n = fa(c, 4), f = fa(c, 5);
    set_reg(REGADDR::PA_CL_VPORT_XSCALE, gx2::fbits(w * 0.5f));
    set_reg(REGADDR::PA_CL_VPORT_XOFFSET, gx2::fbits(x + w * 0.5f));
    set_reg(REGADDR::PA_CL_VPORT_YSCALE, gx2::fbits(h * -0.5f));
    set_reg(REGADDR::PA_CL_VPORT_YOFFSET, gx2::fbits(y + h * 0.5f));
    set_reg(REGADDR::PA_CL_VPORT_ZSCALE, gx2::fbits((f - n) * 0.5f));
    set_reg(REGADDR::PA_CL_VPORT_ZOFFSET, gx2::fbits((n + f) * 0.5f));
}

HLE(gx2, GX2SetScissor) {
    uint32 x = arg(c, 0), y = arg(c, 1), w = arg(c, 2), h = arg(c, 3);
    uint32 tlx = std::min(x, 8192u), tly = std::min(y, 8192u), brx = std::min(x + w, 8192u), bry = std::min(y + h, 8192u);
    LATTE_PA_SC_GENERIC_SCISSOR_TL tl;
    tl.set_TL_X(tlx).set_TL_Y(tly).set_WINDOW_OFFSET_DISABLE(true);
    LATTE_PA_SC_GENERIC_SCISSOR_BR br;
    br.set_BR_X(brx).set_BR_Y(bry);
    set_reg(REGADDR::PA_SC_GENERIC_SCISSOR_TL, tl.getRawValue());
    set_reg(REGADDR::PA_SC_GENERIC_SCISSOR_BR, br.getRawValue());
}

HLE(gx2, GX2SetPointSize) {
    uint32 w = std::min<uint32>((uint32)(fa(c, 0) * 8.0f), 0xFFFF), h = std::min<uint32>((uint32)(fa(c, 1) * 8.0f), 0xFFFF);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(w).set_HEIGHT(h).getRawValue());
}

HLE(gx2, GX2SetLineWidth) {
    uint32 w = std::min<uint32>((uint32)(fa(c, 0) * 8.0f), 0xFFFF);
    set_reg(mmPA_SU_LINE_CNTL, w);
}

// ---------------------------------------------------------------- shader mode
HLE(gx2, GX2SetShaderModeEx) {
    // (mode, gprsVS, stackVS, gprsGS, stackGS, gprsPS, stackPS)
    uint32 mode = arg(c, 0);
    bool geometry = mode == 2;
    if (!geometry) set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(mode == 0).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
}
