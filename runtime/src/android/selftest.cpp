// Renderer self-test without game files (WWHD_SELFTEST=1): drives the GX2-level renderer interface
// the way the game does, with hand-assembled Latte shaders. Draws two textured quads into a
// 1280x720 color buffer (left: RGBA8 gradient, right: BC1 blocks, which most mobile GPUs can only
// show through the CPU decoder) and presents it as the TV and GamePad images.
#include <chrono>
#include <thread>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "gx2/gx2.h"
#include "gx2/gx2_cmd.h"
#include "gx2/gx2_regs.h"
#include "gx2_texture_regs.h"
#include "runtime.h"

extern "C" uint64_t g_shader_state_gen;

using namespace Latte;

namespace {
constexpr uint32_t kW = 1280, kH = 720;
uint32_t g_regs[gx2::kNumRegs];

void put_le(uint32_t addr, std::initializer_list<uint32_t> words) {  // shader microcode is little-endian
    for (uint32_t w : words) {
        memcpy(mem::ptr(addr), &w, 4);
        addr += 4;
    }
}

uint32_t alloc(uint32_t size) { return mem::runtime_alloc(size, 0x100); }

// a linear-aligned texture; `texel(x, y, dst)` writes one element (texel or 4x4 block) in guest layout
template <class F>
uint32_t make_texture(uint32_t fmt, uint32_t w, uint32_t h, uint32_t bytesPerElement, uint32_t blockDim, uint32_t* pitchOut, F texel) {
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((E_GX2SURFFMT)fmt, w, h, 1, E_DIM::DIM_2D, E_GX2TILEMODE::TM_LINEAR_ALIGNED, 0, 0, &info);
    uint32_t addr = alloc((uint32_t)info.surfSize);
    for (uint32_t y = 0; y < h / blockDim; y++)
        for (uint32_t x = 0; x < w / blockDim; x++) {
            uint32_t off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, 0, 0, bytesPerElement * 8, info.pitch, info.height, 1);
            texel(x, y, mem::ptr(addr + off));
        }
    *pitchOut = info.pitch * blockDim;  // in texels
    return addr;
}

void set_texture(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitchTexels) {
    uint32_t* t = &g_regs[REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS];
    LATTE_SQ_TEX_RESOURCE_WORD0_N w0{};
    w0.set_DIM(E_DIM::DIM_2D).set_TILE_MODE(E_HWTILEMODE::TM_LINEAR_ALIGNED).set_PITCH(pitchTexels / 8 - 1).set_WIDTH(w - 1);
    LATTE_SQ_TEX_RESOURCE_WORD1_N w1{};
    w1.set_HEIGHT(h - 1).set_DATA_FORMAT((E_HWFMT)fmt);
    LATTE_SQ_TEX_RESOURCE_WORD4_N w4{};
    w4.set_DST_SEL_X(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL::SEL_X).set_DST_SEL_Y(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL::SEL_Y)
        .set_DST_SEL_Z(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL::SEL_Z).set_DST_SEL_W(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL::SEL_W);
    t[0] = w0.getRawValue();
    t[1] = w1.getRawValue();
    t[2] = addr >> 8;
    t[3] = 0;
    t[4] = w4.getRawValue();
    t[5] = 0;
    t[6] = 0;
    g_shader_state_gen++;  // as gx2_core.cpp does for shader-relevant register writes
}

void stf(uint32_t addr, float f) { st32(addr, gx2::fbits(f)); }
}  // namespace

void run_selftest() {
    LOG("[selftest] start: renderer %s", gfx::backend_name());
    // ---- shaders (see the host test in the commit that added this file)
    uint32_t vs = alloc(64), ps = alloc(64), fsProg = alloc(64);
    put_le(vs, {0, 0x13u << 23,                                                                       // CALL_FS
                60 | 1u << 13 | 1u << 15, 0 | 1 << 3 | 2 << 6 | 3 << 9 | 0x27u << 23,                  // EXPORT POS0 <- R1
                0 | 2u << 13 | 2u << 15, 0 | 1 << 3 | 2 << 6 | 3 << 9 | 1u << 21 | 0x28u << 23});      // EXPORT_DONE PARAM0 <- R2
    put_le(ps, {4, 0x01u << 23,                                                                       // TEX clause at word 4
                0, 0 | 1 << 3 | 2 << 6 | 3 << 9 | 1u << 21 | 0x28u << 23,                             // EXPORT_DONE PIXEL0 <- R0
                0, 0, 0, 0,
                0x10, 0 | 1 << 12 | 2 << 15 | 3 << 18 | 0xFu << 28, 0 | 1 << 23 | 2 << 26 | 3u << 29, 0});  // R0 = SAMPLE(t0, R0)
    // fetch shader in the runtime's compact encoding (gx2_resources.cpp): two float2 attributes in buffer 0
    st32(fsProg, 0x57574653);
    st32(fsProg + 4, 2);
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t e = fsProg + 16 + i * 16;
        st32(e, i | 0u << 8 | 0u << 16 | 3u << 24);  // location i, buffer 0, per vertex, default endian
        st32(e + 4, i * 8);                          // offset
        st32(e + 8, 0x80D);                          // GX2_ATTRIB_FORMAT_FLOAT_32_32
        st32(e + 12, 0x00010405);                    // x, y, 0, 1
    }
    // ---- vertex data: per quad 4 vertices of (x, y, u, v), big-endian floats
    uint32_t vb = alloc(2 * 4 * 16);
    auto quad = [&](uint32_t base, float x0, float x1) {
        const float v[4][4] = {{x0, -0.8f, 0, 0}, {x1, -0.8f, 1, 0}, {x1, 0.8f, 1, 1}, {x0, 0.8f, 0, 1}};
        for (int i = 0; i < 4; i++)
            for (int k = 0; k < 4; k++) stf(base + i * 16 + k * 4, v[i][k]);
    };
    quad(vb, -0.95f, -0.05f);
    quad(vb + 64, 0.05f, 0.95f);
    // ---- textures
    uint32_t rgbaPitch, bcPitch;
    uint32_t rgba = make_texture(0x1A, 256, 256, 4, 1, &rgbaPitch, [](uint32_t x, uint32_t y, uint8_t* p) {
        p[0] = (uint8_t)x;
        p[1] = (uint8_t)y;
        p[2] = ((x / 32 + y / 32) & 1) ? 255 : 64;
        p[3] = 255;
    });
    uint32_t bc1 = make_texture(0x31, 64, 64, 8, 4, &bcPitch, [](uint32_t bx, uint32_t by, uint8_t* p) {
        // solid block: color0 = RGB565 from the block position, all indices 0
        uint16_t r = (uint16_t)(bx * 2), g = (uint16_t)(by * 4), b = (uint16_t)(((bx ^ by) & 1) * 31);
        uint16_t c0 = (uint16_t)(r << 11 | g << 5 | b);
        memset(p, 0, 8);
        p[0] = (uint8_t)c0;
        p[1] = (uint8_t)(c0 >> 8);
    });
    // ---- color buffer (GX2ColorBuffer struct, as GX2InitColorBuffer leaves it) and its registers
    uint32_t cbMem = alloc(kW * kH * 4), cbStruct = alloc(sizeof(GX2::GX2ColorBuffer));
    auto* cb = (GX2::GX2ColorBuffer*)mem::ptr(cbStruct);
    cb->surface.dim = E_DIM::DIM_2D;
    cb->surface.width = kW;
    cb->surface.height = kH;
    cb->surface.depth = 1;
    cb->surface.numLevels = 1;
    cb->surface.format = (E_GX2SURFFMT)0x1A;
    cb->surface.tileMode = E_GX2TILEMODE::TM_LINEAR_ALIGNED;
    cb->surface.imagePtr = cbMem;
    cb->surface.pitch = kW;
    cb->viewNumSlices = 1;
    uint32_t* r = g_regs;
    r[mmCB_COLOR0_BASE] = cbMem;
    r[mmCB_COLOR0_SIZE] = (kW / 8 - 1) | ((kW * kH / 64 - 1) << 10);
    r[mmCB_COLOR0_INFO] = 0x1A << 2;
    r[mmCB_COLOR0_TILE] = kW | 1u << 16;  // the renderer's convention: width | slices << 16
    r[mmCB_COLOR0_FRAG] = kH;
    r[REGADDR::CB_TARGET_MASK] = 0xF;
    r[mmCB_SHADER_MASK] = 0xF;
    r[REGADDR::CB_COLOR_CONTROL] = 0x00CC0000;  // ROP copy, no blending
    // ---- shader and rasterizer state
    r[mmSQ_PGM_START_VS] = vs >> 8;
    r[mmSQ_PGM_START_VS + 1] = 24 >> 3;
    r[mmSQ_PGM_START_PS] = ps >> 8;
    r[mmSQ_PGM_START_PS + 1] = 48 >> 3;
    r[mmSQ_PGM_START_FS] = fsProg >> 8;
    r[mmSQ_PGM_START_FS + 1] = 48 >> 3;
    for (int i = 0; i < 32; i++) r[mmSQ_VTX_SEMANTIC_0 + i] = i < 2 ? i : 0xFF;  // R1 <- attribute 0, R2 <- attribute 1
    r[mmSPI_VS_OUT_ID_0] = 0xFFFFFF00;  // param 0: semantic 0
    r[mmSPI_PS_IN_CONTROL_0] = 1;
    r[mmSPI_PS_INPUT_CNTL_0] = 0;
    r[REGADDR::PA_CL_VTE_CNTL] = 0x43F;
    r[REGADDR::PA_CL_VPORT_XSCALE] = gx2::fbits(kW / 2.0f);
    r[REGADDR::PA_CL_VPORT_XOFFSET] = gx2::fbits(kW / 2.0f);
    r[REGADDR::PA_CL_VPORT_YSCALE] = gx2::fbits(-(kH / 2.0f));  // GL-style: +y up
    r[REGADDR::PA_CL_VPORT_YOFFSET] = gx2::fbits(kH / 2.0f);
    r[REGADDR::PA_CL_VPORT_ZSCALE] = gx2::fbits(0.5f);
    r[REGADDR::PA_CL_VPORT_ZOFFSET] = gx2::fbits(0.5f);
    r[REGADDR::PA_SC_GENERIC_SCISSOR_TL] = 0x80000000;  // window offset disabled
    r[REGADDR::PA_SC_GENERIC_SCISSOR_BR] = kW | kH << 16;
    r[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + SAMPLER_BASE_INDEX_PIXEL * 3] = 1u << 9 | 1u << 12;  // bilinear
    r[mmSQ_VTX_ATTRIBUTE_BLOCK_START] = vb;
    r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 1] = 2 * 4 * 16 - 1;
    r[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 2] = 16 << 11;  // stride
    gfx::set_tv_format(0x1A, true);
    gfx::set_tv_format(0x1A, false);

    for (uint64_t frame = 0;; frame++) {
        auto t0 = std::chrono::steady_clock::now();
        float pulse = 0.5f + 0.5f * sinf(frame * 0.05f);
        const float clear[4] = {0.1f * pulse, 0.15f, 0.3f, 1};
        gfx::clear_color(r, cbStruct, clear);
        set_texture(rgba, 0x1A, 256, 256, rgbaPitch);
        gfx::draw(r, 0x13, 4, 0, 0, 0, 1);  // quads, 4 vertices from vertex 0
        set_texture(bc1, 0x31, 64, 64, bcPitch);
        gfx::draw(r, 0x13, 4, 0, 0, 4, 1);
        gfx::copy_to_scan(cbStruct, 1);
        gfx::copy_to_scan(cbStruct, 4);
        gfx::swap();
        if (frame % 300 == 0) LOG("[selftest] frame %llu, %llu completed", (unsigned long long)frame, (unsigned long long)gfx::frames_completed());
        std::this_thread::sleep_until(t0 + std::chrono::milliseconds(33));
    }
}
