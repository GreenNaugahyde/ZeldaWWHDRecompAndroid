// shim: the decompiler only asks which API it targets (MSL for the Metal build, Vulkan GLSL otherwise)
#pragma once
#include <memory>
enum class RendererAPI { OpenGL, Vulkan, Metal };
class Renderer {
public:
    enum class INDEX_TYPE { NONE, U16, U32 };
    virtual ~Renderer() = default;
#ifdef ENABLE_METAL
    RendererAPI GetType() const { return RendererAPI::Metal; }
#else
    RendererAPI GetType() const { return RendererAPI::Vulkan; }
#endif
};
extern std::unique_ptr<Renderer> g_renderer;
