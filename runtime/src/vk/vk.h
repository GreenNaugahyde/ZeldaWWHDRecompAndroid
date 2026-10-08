// Vulkan renderer internals (Android). Mirrors the Metal renderer (runtime/src/gfx/metal.h):
// the same surface model and caches, on Vulkan objects.
#pragma once
#include <volk.h>  // Vulkan through function pointers (no prototypes; see create_device)

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vk_formats.h"
#include "vk_mem_alloc.h"
#include "vk_record.h"

namespace gfx {

#define VK_CHECK(x)                                                                     \
    do {                                                                                \
        VkResult r_ = (x);                                                              \
        if (r_ != VK_SUCCESS) fatal("%s failed: VkResult %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); \
    } while (0)

// what an image is about to be used for; decides its layout and the barrier before that use
enum class Use : uint8_t { NONE, SAMPLED, COLOR, DEPTH, COPY_SRC, COPY_DST };

// A Vulkan image with its tracked layout (one layout for all subresources).
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;  // natural view type
    uint32_t width = 0, height = 0, depth = 1, layers = 1, mips = 1;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Use use = Use::NONE;
    bool cube = false;  // created cube compatible
    std::vector<VkImageView> attachViews;  // per layer, for rendering
};

// A host image backing a guest surface (render target, depth buffer or sampled texture).
struct Surface {
    Image img;
    uint32_t addr = 0, mipAddr = 0;
    uint32_t width = 0, height = 0, slices = 1, pitch = 0, mips = 1;
    uint32_t format = 0;       // E_GX2SURFFMT
    uint32_t dim = 1;          // E_DIM
    uint32_t tileMode = 0;     // E_HWTILEMODE
    uint32_t swizzle = 0;
    bool isDepth = false;
    bool gpuWritten = false;   // contents produced by the GPU; never reload from guest memory
    uint64_t writeSeq = 0;     // when the GPU last wrote it (several surfaces can alias one address)
    uint64_t contentHash = 0;  // hash of guest data at last upload
    uint64_t lastCheckedFrame = ~0ull;
    uint64_t sparseHash = 0;   // cheap per-frame change check (a few hundred samples)
    uint32_t dataSize = 0;     // base level size in guest memory
    bool dirty = true;         // new, or invalidated by the game: do a full check
    FormatInfo fmt;
    float rscale = 1.0f;              // resolution scale: image size / guest size (render targets only)
    float ax = 1.0f, ay = 1.0f;       // aspect ratio widening (taller) of TV-shaped targets, on top of rscale
    Surface* feedbackCopy = nullptr;  // copy sampled while this surface is a bound attachment
    Surface* mipChain = nullptr;      // a rendered picture with coarser levels, for sampling with mips
    uint64_t mipChainSeq = ~0ull;     // writeSeq of the source when mipChain was last built
    Surface* volume = nullptr;        // a render target with slices, as the 3D texture the game samples
    uint64_t volumeSeq = ~0ull;       // writeSeq of the source when volume was last copied
    bool aliased = false;             // another colour surface of the same texel bits shares the address
    uint64_t writtenBackSeq = 0;      // linear render targets: writeSeq when last written to guest memory
    uint64_t lastDrawFrame = ~0ull;   // render targets: the last frame drawn into,
    uint32_t drawStreak = 0;          // and in how many consecutive frames up to it
    bool firstDrawFrame = false;      // lastDrawFrame is the first frame it was drawn into
};

// A display output: the TV or the GamePad screen, composed into the Android surface.
struct Screen {
    Image img;                          // last image copied to the scan buffer
    VkImageView view = VK_NULL_HANDLE;  // for presentation
    std::atomic<bool> srgb{false};      // scan buffer is sRGB: presentation applies the encoding
};

uint64_t next_write_seq();
uint64_t write_seq();  // the latest; changes whenever a surface is written, uploaded or invalidated
inline void mark_gpu_written(Surface* s) { s->gpuWritten = true; s->writeSeq = next_write_seq(); }

// Transient GPU-visible memory for one draw's data (vertices, indices, uniforms, uploads).
struct Upload {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;  // mapped
};

struct Renderer {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VmaAllocator vma = nullptr;
    VkPhysicalDeviceProperties props{};
    std::string driverInfo;  // the running driver's name and version
    VkDriverId driverID = (VkDriverId)0;  // VkPhysicalDeviceDriverProperties::driverID (0: unknown)
    VkPhysicalDeviceFeatures features{};  // enabled features
    bool mirrorClampToEdge = false;
    bool uploadCached = false;   // transient upload memory is CPU-cached (copy_deduped compares in it)
    bool shaderFloat16 = false;  // 16-bit float arithmetic in shaders (frame generation)
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::mutex queueMutex;  // vkQueueSubmit/Present from the render thread, waits from the UI thread

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // recording, or null
    uint64_t cmdSerial = 0;                // increments with every command buffer begun
    // open render pass
    VkRenderPass pass = VK_NULL_HANDLE;
    Surface* passColor[8] = {};
    Surface* passDepth = nullptr;
    uint32_t mainDepthAddr = 0;  // guest address of the last TV-sized (1280x720) depth buffer drawn with (peek_z)
    uint32_t passColorSlice[8] = {}, passDepthSlice = 0;
    uint32_t passWidth = 0, passHeight = 0;

    Screen tv, drc;
    VkSampler linearClamp = VK_NULL_HANDLE;

    // surfaces keyed by guest address (several may share an address with different shapes)
    std::unordered_multimap<uint32_t, std::unique_ptr<Surface>> surfaces;

    uint64_t frame = 0;
    uint64_t drawCount = 0;
};
extern Renderer R;

// ---- command recording (render thread)
VkCommandBuffer command_buffer();  // begins one if needed
void end_pass();                   // close the open render pass, if any
// run `fn` once the GPU finished everything recorded so far (on the render thread, during a later poll)
void on_complete(std::function<void()> fn);
// descriptor sets live until the command buffer they are used in has completed
VkDescriptorSet alloc_descriptor_set(VkDescriptorSetLayout layout);
uint32_t& draws_since_commit();  // draws recorded since the last submission
// make an image ready for `use` (records a barrier; ends the render pass if needed)
void prepare(Image& img, Use use);

// a surface was added or its data size changed (invalidate's address index)
void surfaces_changed();

// ---- GPU profiling (vk_profile.cpp; WWHD_GPU_PROFILE=1)
void prof_cmd_begin();
void prof_cmd_end();
void prof_pass_begin(uint32_t w, uint32_t h, const VkFormat* colors, uint32_t nColors, VkFormat depth);
void prof_pass_end();
void prof_draw(uint32_t ps);
void prof_frame();

// ---- transient memory
Upload upload_alloc(VkDeviceSize size, VkDeviceSize align = 256);
Upload upload(const void* data, VkDeviceSize size, VkDeviceSize align = 256);

// ---- images
bool create_image(Image& img, VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t w, uint32_t h,
                  uint32_t depth, uint32_t layers, uint32_t mips, VkImageUsageFlags usage, bool cube, bool isDepth, bool stencil);
VkImageView attachment_view(Image& img, uint32_t layer);
VkImageView make_view(Image& img, VkImageViewType type, uint32_t baseLayer, uint32_t layers, VkComponentMapping sw,
                      VkImageAspectFlags aspect);

// ---- surfaces
struct SurfaceDesc {
    uint32_t addr = 0, mipAddr = 0, width = 0, height = 0, slices = 1, pitch = 0, mips = 1;
    uint32_t format = 0, dim = 1, tileMode = 0, swizzle = 0;
    bool isDepth = false;
};
Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering);
Surface* color_target(const uint32_t* regs, int index, uint32_t* slice = nullptr);  // from CB_COLOR* registers
Surface* depth_target(const uint32_t* regs, uint32_t* slice = nullptr);             // from DB_DEPTH_* registers
Surface* surface_from_color_buffer(uint32_t gx2ColorBuffer, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* surface_from_depth_buffer(uint32_t gx2DepthBuffer, uint32_t* firstSlice = nullptr, uint32_t* numSlices = nullptr);
Surface* sampled_texture(const uint32_t* texWords, bool isDepthSampler);  // from SQ_TEX_RESOURCE words
void write_back_linear();  // GX2DrawDone: render results the CPU reads, to guest memory
void upload_surface(Surface* s);
// a private image like `like` (render targets the game doesn't know about)
bool create_surface_image(Surface* s, bool forRendering);
// resolution scale for screen-sized render targets (WWHD_RES_SCALE at startup, then the app's
// setting); a change requested with set_resolution_scale() takes effect at the next frame
float resolution_scale();
void latch_resolution_scale();  // frame boundary (swap)
// a render target made at another scale: reallocate it at the current one, keeping its contents
Surface* rescaled(Surface* s);
// forget everything that refers to `old` and free it once the GPU is done with it
void retire_image(const Image& old);

// ---- shaders (vk_shader.cpp)
void shader_compiler_init();
// GLSL (Cemu decompiler output, Vulkan flavour) -> SPIR-V; false and a log message on errors
bool compile_glsl(const char* src, bool vertex, std::vector<uint32_t>& spirv, std::string& log);
bool compile_glsl_compute(const char* src, std::vector<uint32_t>& spirv, std::string& log);

// ---- debugging (vk_device.cpp)
void dump_texture(Image& img, const char* name, bool async, bool srgbEncode);
bool log_this_frame();

}  // namespace gfx
