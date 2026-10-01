// Decompile the shaders in a GX2 shader file (.gsh) to Metal Shading Language.
//   shadertest file.gsh [index]
// Loads the file into guest memory, binds each vertex/pixel shader pair into a
// register file exactly as GX2SetVertexShader/GX2SetPixelShader do, and runs
// the vendored Cemu decompiler.
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "gx2/gx2_regs.h"
#include "ppc.h"
#include "util/helpers/StringBuf.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"

std::unique_ptr<Renderer> g_renderer;
void cemu_shim_log(const std::string& msg) { fprintf(stderr, "[decompiler] %s\n", msg.c_str()); }

struct Block {
    uint32 type, addr, size;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: shadertest file.gsh\n");
        return 1;
    }
    mach_vm_address_t base = (mach_vm_address_t)PPC_MEM_BASE;
    if (mach_vm_allocate(mach_task_self(), &base, 0x100000000ull, VM_FLAGS_FIXED) != KERN_SUCCESS) return 1;
    g_renderer = std::make_unique<MetalRenderer>();

    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8> d((std::istreambuf_iterator<char>(f)), {});
    auto be = [&](size_t o) { return (uint32)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]; };

    // copy every block into guest memory at 0x10000000+ (256-byte aligned, as GX2 requires)
    std::vector<Block> blocks;
    uint32 next = 0x10000000;
    size_t off = be(4);
    while (off + 0x20 <= d.size()) {
        uint32 hs = be(off + 4), type = be(off + 16), size = be(off + 20);
        memcpy(PPC_MEM_BASE + next, &d[off + hs], size);
        blocks.push_back({type, next, size});
        next = (next + size + 0xFF) & ~0xFFu;
        off += hs + size;
    }
    // shader headers (types 3/6) are followed by their programs (types 5/7);
    // point the header's shaderPtr at the loaded program
    uint32 vs = 0, ps = 0;
    for (size_t i = 0; i + 1 < blocks.size(); i++) {
        if (blocks[i].type == 3 && blocks[i + 1].type == 5) {
            vs = blocks[i].addr;
            st32(vs + 0xD0, blocks[i + 1].size);
            st32(vs + 0xD4, blocks[i + 1].addr);
        }
        if (blocks[i].type == 6 && blocks[i + 1].type == 7) {
            ps = blocks[i].addr;
            st32(ps + 0xA4, blocks[i + 1].size);
            st32(ps + 0xA8, blocks[i + 1].addr);
        }
    }
    static uint32 regs[gx2::kNumRegs];
    uint32 size;

    if (vs) {
        gx2::bind_vertex_shader_regs(regs, vs);
        // fetch shader: one float4 attribute per vertex shader input, each in its own buffer
        LatteFetchShader fs;
        // attribInfo pointers inside .gsh files are relocatable offsets (0xD06xxxxx) into the header block
        uint32 nattr = ld32(vs + 0x104), attrs = vs + (ld32(vs + 0x108) & 0xFFFFF);
        static LatteParsedFetchShaderAttribute pa[16];
        for (uint32 i = 0; i < nattr && i < 16; i++) {
            // GX2AttribVar: +0 name, +4 type, +8 count, +C location
            uint32 loc = ld32(attrs + i * 16 + 12);
            LatteParsedFetchShaderAttribute& a = pa[i];
            a = {};
            a.attributeBufferIndex = (uint8)i;
            a.semanticId = (uint8)loc;
            a.format = Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT;
            a.fetchType = LatteConst::VertexFetchType2::VERTEX_DATA;
            a.endianSwap = LatteConst::VertexFetchEndianMode::SWAP_U32;
            a.ds[0] = 0; a.ds[1] = 1; a.ds[2] = 2; a.ds[3] = 3;
            LatteParsedFetchShaderBufferGroup g{};
            g.attributeBufferIndex = (uint8)i;
            g.attribCount = 1;
            g.attrib = &a;
            fs.bufferGroups.push_back(g);
            fs.attributeBufferMask |= 1u << i;
            regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7 + 2] = 16 << 11;  // stride
            printf("// attribute %u -> semantic %u\n", i, loc);
        }
        uint32 prog = gx2::vertex_shader_program(vs, &size);
        LatteDecompilerOptions opt;
        LatteDecompilerOutput_t out{};
        LatteDecompiler_DecompileVertexShader(0x1234, regs, PPC_MEM_BASE + prog, size, &fs, opt, &out);
        printf("// ===== vertex shader (%u bytes) =====\n%s\n", size,
               out.shader && out.shader->strBuf_shaderSource ? out.shader->strBuf_shaderSource->c_str() : "(no output)");
        fs.bufferGroups.clear();
    }
    if (ps) {
        gx2::bind_pixel_shader_regs(regs, ps);
        regs[mmCB_COLOR0_INFO] = 0x1A << 2;  // RGBA8 render target
        regs[mmCB_COLOR0_BASE] = 0x100;
        regs[Latte::REGADDR::CB_TARGET_MASK] = 0xF;
        uint32 prog = gx2::pixel_shader_program(ps, &size);
        LatteDecompilerOptions opt;
        LatteDecompilerOutput_t out{};
        LatteDecompiler_DecompilePixelShader(0x5678, regs, PPC_MEM_BASE + prog, size, opt, &out);
        printf("// ===== pixel shader (%u bytes) =====\n%s\n", size,
               out.shader && out.shader->strBuf_shaderSource ? out.shader->strBuf_shaderSource->c_str() : "(no output)");
    }
    return 0;
}
