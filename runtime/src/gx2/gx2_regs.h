// GX2 state is kept as a GPU register file indexed like the hardware. Register
// addresses and bit layouts come from the vendored Cemu definitions
// (third_party/cemu/Cafe/HW/Latte/ISA/{RegDefines,LatteReg}.h).
// GX2 API functions write register values; the Metal backend and the shader
// decompiler read them. Files including this header are compiled with
// third_party/cemu/cemu_shim.h force-included.
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"

namespace gx2 {

constexpr uint32 kNumRegs = 0x10000;

// shader binding helpers (write into an explicit register array)
void bind_vertex_shader_regs(uint32* regs, uint32 shader);    // GX2VertexShader* (guest address)
void bind_pixel_shader_regs(uint32* regs, uint32 shader);     // GX2PixelShader*
void bind_geometry_shader_regs(uint32* regs, uint32 shader);  // GX2GeometryShader*
uint32 vertex_shader_program(uint32 shader, uint32* size);
uint32 pixel_shader_program(uint32 shader, uint32* size);

}  // namespace gx2
