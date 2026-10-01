// shim: decompiler settings
#pragma once
class ActiveSettings {
public:
    static bool DumpShadersEnabled() { return false; }
    static bool ShaderPreventInfiniteLoopsEnabled() { return false; }
    static bool ForceSamplerRoundToPrecision() { return false; }
};
