// shim: render-target helpers used by the decompiler (implemented in latte_mrt.cpp)
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
class LatteDecompilerShader;
class LatteMRT {
public:
    static uint8 GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const struct LatteContextRegister& lcr);
    static bool GetActiveDepthBufferMask(const struct LatteContextRegister& lcr);
    static Latte::E_GX2SURFFMT GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr);
    static Latte::E_GX2SURFFMT GetDepthBufferFormat(const LatteContextRegister& lcr);
};
