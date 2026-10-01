// Trimmed from Cemu src/Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h (MPL-2.0): only what the MSL emitter uses.
#pragma once
#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"

enum class MetalDataType { NONE, INT, UINT, FLOAT };

// number type bits of E_GX2SURFFMT: FMT_BIT_INT 0x100, FMT_BIT_SIGNED 0x200 -> 0x100 UINT, 0x300 SINT
inline MetalDataType GetColorBufferDataType(const uint32 index, const LatteContextRegister& lcr) {
    uint32 f = (uint32)LatteMRT::GetColorBufferFormat(index, lcr);
    if ((f & 0x300) == 0x100) return MetalDataType::UINT;
    if ((f & 0x300) == 0x300) return MetalDataType::INT;
    return MetalDataType::FLOAT;
}

inline const char* GetDataTypeStr(MetalDataType dataType) {
    switch (dataType) {
    case MetalDataType::INT: return "int4";
    case MetalDataType::UINT: return "uint4";
    case MetalDataType::FLOAT: return "float4";
    default: return "INVALID";
    }
}

MTL::VertexFormat GetMtlVertexFormat(Latte::E_HWFMT format);
uint32 GetMtlVertexFormatSize(Latte::E_HWFMT format);
