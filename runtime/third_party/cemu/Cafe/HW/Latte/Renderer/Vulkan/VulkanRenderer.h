// Decompiler API selection shim; Vulkan device ownership stays in gfx/vulkan.
#pragma once
#include "Cafe/HW/Latte/Renderer/Renderer.h"
class VulkanRenderer : public Renderer {
public:
    VulkanRenderer() : Renderer(RendererAPI::Vulkan) {}
};
