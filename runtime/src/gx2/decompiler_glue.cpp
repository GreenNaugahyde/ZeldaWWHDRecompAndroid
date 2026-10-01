// Definitions the vendored Cemu decompiler expects from its host.
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#include "runtime.h"

std::unique_ptr<Renderer> g_renderer = std::make_unique<MetalRenderer>();
void cemu_shim_log(const std::string& msg) { LOG("[decompiler] %s", msg.c_str()); }
