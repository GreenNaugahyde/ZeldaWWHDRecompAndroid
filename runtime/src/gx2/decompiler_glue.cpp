// Definitions the vendored Cemu decompiler expects from its host.
#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
std::unique_ptr<Renderer> g_renderer = std::make_unique<MetalRenderer>();
#else
#include "Cafe/HW/Latte/Renderer/Renderer.h"
std::unique_ptr<Renderer> g_renderer = std::make_unique<Renderer>();
#endif
#include "runtime.h"

void cemu_shim_log(const std::string& msg) { LOG("[decompiler] %s", msg.c_str()); }
