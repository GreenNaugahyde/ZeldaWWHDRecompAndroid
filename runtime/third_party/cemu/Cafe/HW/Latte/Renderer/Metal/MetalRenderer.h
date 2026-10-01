// shim: capabilities queried by the MSL emitter
#pragma once
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/Metal/MetalCommon.h"
class MetalRenderer : public Renderer {
public:
    bool SupportsFramebufferFetch() const { return true; }  // all Apple silicon GPUs
};
