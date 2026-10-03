// shim: the decompiler only asks which API it targets
#pragma once
#include <memory>
enum class RendererAPI { OpenGL, Vulkan, Metal };
class Renderer {
public:
    enum class INDEX_TYPE { NONE, U16, U32 };
    virtual ~Renderer() = default;
    explicit Renderer(RendererAPI api = RendererAPI::Metal) : m_api(api) {}
    RendererAPI GetType() const { return m_api; }
private:
    RendererAPI m_api;
};
extern std::unique_ptr<Renderer> g_renderer;
