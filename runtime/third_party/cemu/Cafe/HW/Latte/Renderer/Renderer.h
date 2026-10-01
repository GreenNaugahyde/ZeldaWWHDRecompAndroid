// shim: the decompiler only asks which API it targets
#pragma once
#include <memory>
enum class RendererAPI { OpenGL, Vulkan, Metal };
class Renderer {
public:
    enum class INDEX_TYPE { NONE, U16, U32 };
    virtual ~Renderer() = default;
    RendererAPI GetType() const { return RendererAPI::Metal; }
};
extern std::unique_ptr<Renderer> g_renderer;
