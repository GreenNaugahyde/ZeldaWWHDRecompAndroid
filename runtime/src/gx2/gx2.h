// Internal interfaces between the GX2 layer and the renderer.
#pragma once
#include <cstdint>

struct LatteFetchShader;
namespace GX2 {
struct GX2ColorBuffer;
struct GX2DepthBuffer;
}

namespace gx2 {
uint32_t color_buffer_address(const GX2::GX2ColorBuffer* cb);
LatteFetchShader* build_fetch_shader(uint32_t program);  // from our encoded fetch "program"
}  // namespace gx2

// The renderer backend (Metal on macOS, Vulkan on Android). All calls come from the thread executing GX2
// commands, in submission order. Guest structures are passed by guest address.
namespace gx2 {
constexpr uint32_t kDepthSlicesReg = 0xA002;  // our convention (unused register): depth buffer array size
}

namespace gfx {
const char* backend_name();      // "Metal" or "Vulkan"
void init();                     // create device; call on the main thread before the game starts
void run_main_loop();            // window/event loop; runs on the main thread, never returns
void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr,
          uint32_t baseVertex, uint32_t instances);
void clear_color(const uint32_t* regs, uint32_t colorBuffer, const float rgba[4]);
void clear_depth_stencil(const uint32_t* regs, uint32_t depthBuffer, float depth, uint32_t stencil, uint32_t flags);
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice);
void copy_to_scan(uint32_t colorBuffer, uint32_t target);  // target: 1 = TV, 4 = DRC (GamePad)
void swap();                     // present the TV scan buffer
uint64_t frames_completed();     // swaps whose GPU work has finished
void with_autorelease_pool(void (*fn)());  // render thread: drain Objective-C temporaries per batch
void set_tv_format(uint32_t gx2Format, bool tv);  // GX2SetTVBuffer / GX2SetDRCBuffer
void invalidate(uint32_t flags, uint32_t addr, uint32_t size);
void flush();                    // submit queued GPU work
void wait_idle();                // block until the GPU finished
void draw_done();                // GX2DrawDone: what the game may rely on once it returns (see the renderers)
}  // namespace gfx
