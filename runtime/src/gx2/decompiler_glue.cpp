// Definitions the vendored Cemu decompiler expects from its host.
#ifdef WWHD_HAS_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#endif
#ifdef WWHD_HAS_METAL
#include "Cafe/HW/Latte/Renderer/Metal/MetalRenderer.h"
#endif
#include "gfx/renderer.h"
#include "runtime.h"

// The decompiler emits MSL or GLSL depending on g_renderer's type: set once at start-up (and again if
// Vulkan cannot start and the game falls back to Metal), before any shader is translated.
#ifdef WWHD_HAS_METAL
std::unique_ptr<Renderer> g_renderer = std::make_unique<MetalRenderer>();
#else
std::unique_ptr<Renderer> g_renderer = std::make_unique<VulkanRenderer>();
#endif
void select_decompiler_api(render::Api api) {
#ifdef WWHD_HAS_VULKAN
    if (api == render::Api::Vulkan) {
        g_renderer = std::make_unique<VulkanRenderer>();
        return;
    }
#endif
#ifdef WWHD_HAS_METAL
    g_renderer = std::make_unique<MetalRenderer>();
#endif
}
void cemu_shim_log(const std::string& msg) { LOG("[decompiler] %s", msg.c_str()); }
