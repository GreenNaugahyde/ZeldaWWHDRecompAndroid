#define WWHD_VK_RECORD_IMPLEMENTATION
#include "vk_record.h"
#include "runtime.h"
#include "vk.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>

namespace gfx::rec {
namespace {

enum class Op : uint16_t {
    BeginRenderPass, EndRenderPass, BindPipeline, BindDescriptorSets, BindVertexBuffers, BindIndexBuffer,
    SetViewport, SetScissor, SetBlendConstants, SetDepthBias, SetStencilCompareMask, SetStencilReference,
    SetStencilWriteMask, PushConstants, Draw, DrawIndexed, Dispatch, PipelineBarrier, CopyImage, CopyImage2,
    BlitImage, CopyBufferToImage, CopyImageToBuffer, ClearColorImage, ClearDepthStencilImage, ResetQueryPool,
    WriteTimestamp, ClearAttachments,
};

struct Header { Op op; uint16_t reserved; uint32_t size; };
static_assert(sizeof(Header) == 8);
// Only the GX2 render thread ever records to the virtual command buffer. Keeping its current
// stream non-TLS avoids Android's dynamic TLS resolver on every vkCmd wrapper (thousands/frame).
Stream g_stream;
std::mutex g_free_mutex;
std::vector<Stream> g_free_streams;

size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

struct Writer {
    size_t start;
    size_t cursor;
    std::vector<uint8_t>& data;
    explicit Writer(Op op, size_t payloadCapacity = 0)
        : start(g_stream.data.size()), cursor(start), data(g_stream.data) {
        // Grow once per record. The default covers all fixed commands and common small arrays;
        // variable-sized wrappers pass their exact payload size below.
        data.resize(align_up(start + sizeof(Header) + payloadCapacity + 16, alignof(uint64_t)));
        Header h{op, 0, 0};
        put(h);
    }
    void ensure(size_t end) {
        if (end > data.size()) data.resize(align_up(end + 32, alignof(uint64_t)));
    }
    template <class T> void put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        size_t p = align_up(cursor, alignof(T));
        ensure(p + sizeof(T));
        memcpy(data.data() + p, &v, sizeof(T));
        cursor = p + sizeof(T);
    }
    template <class T> void array(const T* v, uint32_t n) {
        if (!n) return;
        size_t p = align_up(cursor, alignof(T));
        ensure(p + sizeof(T) * n);
        memcpy(data.data() + p, v, sizeof(T) * n);
        cursor = p + sizeof(T) * n;
    }
    void bytes(const void* v, uint32_t n) {
        if (!n) return;
        size_t p = cursor;
        ensure(p + n);
        memcpy(data.data() + p, v, n);
        cursor = p + n;
    }
    ~Writer() {
        size_t end = align_up(cursor, alignof(uint64_t));
        data.resize(end);
        reinterpret_cast<Header*>(data.data() + start)->size = static_cast<uint32_t>(end - start);
    }
};

struct Reader {
    const uint8_t* base;
    size_t pos;
    template <class T> T get() {
        pos = align_up(pos, alignof(T));
        T v;
        memcpy(&v, base + pos, sizeof(T));
        pos += sizeof(T);
        return v;
    }
    template <class T> const T* array(uint32_t n) {
        pos = align_up(pos, alignof(T));
        auto* v = reinterpret_cast<const T*>(base + pos);
        pos += sizeof(T) * n;
        return v;
    }
    const void* bytes(uint32_t n) { const void* v = base + pos; pos += n; return v; }
};

bool deferred(VkCommandBuffer cmd) { return enabled() && cmd == virtual_command_buffer(); }

struct BindPipeline { VkPipelineBindPoint point; VkPipeline pipeline; };
struct BindDescriptorSets { VkPipelineBindPoint point; VkPipelineLayout layout; uint32_t first, sets, offsets; };
struct BindVertexBuffers { uint32_t first, count; };
struct BindIndexBuffer { VkBuffer buffer; VkDeviceSize offset; VkIndexType type; };
struct Range { uint32_t first, count; };
struct DepthBias { float constant, clamp, slope; };
struct Stencil { VkStencilFaceFlags face; uint32_t value; };
struct PushConstants { VkPipelineLayout layout; VkShaderStageFlags stages; uint32_t offset, size; };
struct Draw { uint32_t vertices, instances, firstVertex, firstInstance; };
struct DrawIndexed { uint32_t indices, instances, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
struct Dispatch { uint32_t x, y, z; };
struct PipelineBarrier { VkPipelineStageFlags src, dst; VkDependencyFlags deps; uint32_t mem, buf, img; };
struct CopyImage { VkImage src; VkImageLayout srcLayout; VkImage dst; VkImageLayout dstLayout; uint32_t count; };
struct BlitImage { VkImage src; VkImageLayout srcLayout; VkImage dst; VkImageLayout dstLayout; uint32_t count; VkFilter filter; };
struct CopyBufferImage { VkBuffer buffer; VkImage image; VkImageLayout layout; uint32_t count; };
struct CopyImageBuffer { VkImage image; VkImageLayout layout; VkBuffer buffer; uint32_t count; };
struct ClearColor { VkImage image; VkImageLayout layout; VkClearColorValue color; uint32_t count; };
struct ClearDepth { VkImage image; VkImageLayout layout; VkClearDepthStencilValue value; uint32_t count; };
struct ResetQuery { VkQueryPool pool; uint32_t first, count; };
struct Timestamp { VkPipelineStageFlagBits stage; VkQueryPool pool; uint32_t query; };

template <class T> void clear_pnext(T* v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) v[i].pNext = nullptr;
}

}  // namespace

// On with every driver. With Qualcomm's own driver on a Snapdragon 7+ Gen 3 (Adreno 732, driver v762) the render
// thread went from 21.5 to 14.6 ms a frame (the record thread taking 6.4 ms beside it). Without it
// the render thread lags further behind the game, and the light in Beetle's shop flashed for single
// frames with Turnip (issue #16: 3-8 per 15 s on the Adreno 732, 0 in 35 s with the thread), most likely
// uniform blocks the game rewrites without a flush being read too late; that race doesn't depend on
// the GPU. To be measured again: the Snapdragon 855 (Adreno 640, Turnip; an earlier test, before several
// fixes, had it slower with black flicker now and then) and Mali (Helio G85 / Mali-G52 MC2). WWHD_RECORD_THREAD=0
// turns it off. Decided once the device exists.
bool enabled() {
    static int on = -1;
    if (on >= 0) return on != 0;
    if (const char* e = getenv("WWHD_RECORD_THREAD")) return (on = strcmp(e, "0") != 0);
    if (!R.device) return false;
    on = 1;
    LOG("[vk] record thread %s (%s)", on ? "on" : "off", R.driverInfo.c_str());
    return on != 0;
}

VkCommandBuffer virtual_command_buffer() { return reinterpret_cast<VkCommandBuffer>(uintptr_t{1}); }
void begin() {
    std::lock_guard<std::mutex> lk(g_free_mutex);
    if (!g_free_streams.empty()) {
        g_stream = std::move(g_free_streams.back());
        g_free_streams.pop_back();
    }
    g_stream.data.clear();
    if (g_stream.data.capacity() < 256 * 1024) g_stream.data.reserve(256 * 1024);
}
Stream finish() { Stream out = std::move(g_stream); g_stream = {}; return out; }
void recycle(Stream&& stream) {
    stream.data.clear();
    std::lock_guard<std::mutex> lk(g_free_mutex);
    g_free_streams.push_back(std::move(stream));
}

void CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    if (!deferred(cmd)) return vkCmdBeginRenderPass(cmd, info, contents);
    VkRenderPassBeginInfo copy = *info;
    copy.pNext = nullptr; copy.pClearValues = nullptr;
    Writer w(Op::BeginRenderPass, sizeof(copy) + sizeof(contents) + sizeof(VkClearValue) * info->clearValueCount);
    w.put(copy); w.put(contents); w.array(info->pClearValues, info->clearValueCount);
}
void CmdEndRenderPass(VkCommandBuffer cmd) {
    if (!deferred(cmd)) return vkCmdEndRenderPass(cmd);
    Writer w(Op::EndRenderPass);
}
void CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipeline pipeline) {
    if (!deferred(cmd)) return vkCmdBindPipeline(cmd, point, pipeline);
    Writer w(Op::BindPipeline, sizeof(BindPipeline)); w.put(BindPipeline{point, pipeline});
}
void CmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first,
                           uint32_t setCount, const VkDescriptorSet* sets, uint32_t offsetCount, const uint32_t* offsets) {
    if (!deferred(cmd)) return vkCmdBindDescriptorSets(cmd, point, layout, first, setCount, sets, offsetCount, offsets);
    Writer w(Op::BindDescriptorSets, sizeof(BindDescriptorSets) + sizeof(VkDescriptorSet) * setCount + sizeof(uint32_t) * offsetCount);
    w.put(BindDescriptorSets{point, layout, first, setCount, offsetCount});
    w.array(sets, setCount); w.array(offsets, offsetCount);
}
void CmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
    if (!deferred(cmd)) return vkCmdBindVertexBuffers(cmd, first, count, buffers, offsets);
    Writer w(Op::BindVertexBuffers, sizeof(BindVertexBuffers) + (sizeof(VkBuffer) + sizeof(VkDeviceSize)) * count);
    w.put(BindVertexBuffers{first, count}); w.array(buffers, count); w.array(offsets, count);
}
void CmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    if (!deferred(cmd)) return vkCmdBindIndexBuffer(cmd, buffer, offset, type);
    Writer w(Op::BindIndexBuffer, sizeof(BindIndexBuffer)); w.put(BindIndexBuffer{buffer, offset, type});
}
void CmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkViewport* values) {
    if (!deferred(cmd)) return vkCmdSetViewport(cmd, first, count, values);
    Writer w(Op::SetViewport, sizeof(Range) + sizeof(VkViewport) * count); w.put(Range{first, count}); w.array(values, count);
}
void CmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkRect2D* values) {
    if (!deferred(cmd)) return vkCmdSetScissor(cmd, first, count, values);
    Writer w(Op::SetScissor, sizeof(Range) + sizeof(VkRect2D) * count); w.put(Range{first, count}); w.array(values, count);
}
void CmdSetBlendConstants(VkCommandBuffer cmd, const float values[4]) {
    if (!deferred(cmd)) return vkCmdSetBlendConstants(cmd, values);
    Writer w(Op::SetBlendConstants, sizeof(float) * 4); w.array(values, 4);
}
void CmdSetDepthBias(VkCommandBuffer cmd, float constant, float clamp, float slope) {
    if (!deferred(cmd)) return vkCmdSetDepthBias(cmd, constant, clamp, slope);
    Writer w(Op::SetDepthBias, sizeof(DepthBias)); w.put(DepthBias{constant, clamp, slope});
}
void CmdSetStencilCompareMask(VkCommandBuffer cmd, VkStencilFaceFlags face, uint32_t value) {
    if (!deferred(cmd)) return vkCmdSetStencilCompareMask(cmd, face, value);
    Writer w(Op::SetStencilCompareMask, sizeof(Stencil)); w.put(Stencil{face, value});
}
void CmdSetStencilReference(VkCommandBuffer cmd, VkStencilFaceFlags face, uint32_t value) {
    if (!deferred(cmd)) return vkCmdSetStencilReference(cmd, face, value);
    Writer w(Op::SetStencilReference, sizeof(Stencil)); w.put(Stencil{face, value});
}
void CmdSetStencilWriteMask(VkCommandBuffer cmd, VkStencilFaceFlags face, uint32_t value) {
    if (!deferred(cmd)) return vkCmdSetStencilWriteMask(cmd, face, value);
    Writer w(Op::SetStencilWriteMask, sizeof(Stencil)); w.put(Stencil{face, value});
}
void CmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t offset,
                      uint32_t size, const void* values) {
    if (!deferred(cmd)) return vkCmdPushConstants(cmd, layout, stages, offset, size, values);
    Writer w(Op::PushConstants, sizeof(PushConstants) + size); w.put(PushConstants{layout, stages, offset, size}); w.bytes(values, size);
}
void CmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t firstVertex, uint32_t firstInstance) {
    if (!deferred(cmd)) return vkCmdDraw(cmd, vertices, instances, firstVertex, firstInstance);
    Writer w(Op::Draw, sizeof(Draw)); w.put(Draw{vertices, instances, firstVertex, firstInstance});
}
void CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances, uint32_t firstIndex,
                    int32_t vertexOffset, uint32_t firstInstance) {
    if (!deferred(cmd)) return vkCmdDrawIndexed(cmd, indices, instances, firstIndex, vertexOffset, firstInstance);
    Writer w(Op::DrawIndexed, sizeof(DrawIndexed)); w.put(DrawIndexed{indices, instances, firstIndex, vertexOffset, firstInstance});
}
void CmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    prof_op_begin(cmd, "dispatch", x * 8, y * 8);
    if (!deferred(cmd)) {
        vkCmdDispatch(cmd, x, y, z);
    } else {
        Writer w(Op::Dispatch, sizeof(Dispatch)); w.put(Dispatch{x, y, z});
    }
    prof_op_end(cmd);
}
void CmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags deps,
                        uint32_t memCount, const VkMemoryBarrier* mem, uint32_t bufCount, const VkBufferMemoryBarrier* buf,
                        uint32_t imgCount, const VkImageMemoryBarrier* img) {
    if (!deferred(cmd)) return vkCmdPipelineBarrier(cmd, src, dst, deps, memCount, mem, bufCount, buf, imgCount, img);
    Writer w(Op::PipelineBarrier, sizeof(PipelineBarrier) + sizeof(VkMemoryBarrier) * memCount +
             sizeof(VkBufferMemoryBarrier) * bufCount + sizeof(VkImageMemoryBarrier) * imgCount);
    w.put(PipelineBarrier{src, dst, deps, memCount, bufCount, imgCount});
    w.array(mem, memCount); w.array(buf, bufCount); w.array(img, imgCount);
}
void CmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout,
                  uint32_t count, const VkImageCopy* regions) {
    prof_op_begin(cmd, "copy", count ? regions[0].extent.width : 0, count ? regions[0].extent.height : 0);
    if (!deferred(cmd)) {
        vkCmdCopyImage(cmd, src, srcLayout, dst, dstLayout, count, regions);
    } else {
        Writer w(Op::CopyImage, sizeof(CopyImage) + sizeof(VkImageCopy) * count);
        w.put(CopyImage{src, srcLayout, dst, dstLayout, count}); w.array(regions, count);
    }
    prof_op_end(cmd);
}
void CmdCopyImage2(VkCommandBuffer cmd, const VkCopyImageInfo2* info) {
    prof_op_begin(cmd, "copy", info->regionCount ? info->pRegions[0].extent.width : 0, info->regionCount ? info->pRegions[0].extent.height : 0);
    if (!deferred(cmd)) {
        vkCmdCopyImage2(cmd, info);
    } else {
        VkCopyImageInfo2 copy = *info; copy.pNext = nullptr; copy.pRegions = nullptr;
        Writer w(Op::CopyImage2, sizeof(copy) + sizeof(VkImageCopy2) * info->regionCount); w.put(copy); w.array(info->pRegions, info->regionCount);
    }
    prof_op_end(cmd);
}
void CmdBlitImage(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout,
                  uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    prof_op_begin(cmd, "blit", count ? (uint32_t)regions[0].dstOffsets[1].x : 0, count ? (uint32_t)regions[0].dstOffsets[1].y : 0);
    if (!deferred(cmd)) {
        vkCmdBlitImage(cmd, src, srcLayout, dst, dstLayout, count, regions, filter);
    } else {
        Writer w(Op::BlitImage, sizeof(BlitImage) + sizeof(VkImageBlit) * count);
        w.put(BlitImage{src, srcLayout, dst, dstLayout, count, filter}); w.array(regions, count);
    }
    prof_op_end(cmd);
}
void CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image, VkImageLayout layout,
                          uint32_t count, const VkBufferImageCopy* regions) {
    prof_op_begin(cmd, "upload", count ? regions[0].imageExtent.width : 0, count ? regions[0].imageExtent.height : 0);
    if (!deferred(cmd)) {
        vkCmdCopyBufferToImage(cmd, buffer, image, layout, count, regions);
    } else {
        Writer w(Op::CopyBufferToImage, sizeof(CopyBufferImage) + sizeof(VkBufferImageCopy) * count);
        w.put(CopyBufferImage{buffer, image, layout, count}); w.array(regions, count);
    }
    prof_op_end(cmd);
}
void CmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, VkBuffer buffer,
                          uint32_t count, const VkBufferImageCopy* regions) {
    prof_op_begin(cmd, "readback", count ? regions[0].imageExtent.width : 0, count ? regions[0].imageExtent.height : 0);
    if (!deferred(cmd)) {
        vkCmdCopyImageToBuffer(cmd, image, layout, buffer, count, regions);
    } else {
        Writer w(Op::CopyImageToBuffer, sizeof(CopyImageBuffer) + sizeof(VkBufferImageCopy) * count);
        w.put(CopyImageBuffer{image, layout, buffer, count}); w.array(regions, count);
    }
    prof_op_end(cmd);
}
void CmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const VkClearColorValue* color,
                        uint32_t count, const VkImageSubresourceRange* ranges) {
    prof_op_begin(cmd, "clear color");
    if (!deferred(cmd)) {
        vkCmdClearColorImage(cmd, image, layout, color, count, ranges);
    } else {
        Writer w(Op::ClearColorImage, sizeof(ClearColor) + sizeof(VkImageSubresourceRange) * count);
        w.put(ClearColor{image, layout, *color, count}); w.array(ranges, count);
    }
    prof_op_end(cmd);
}
void CmdClearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment* atts, uint32_t rectCount,
                         const VkClearRect* rects) {
    if (!deferred(cmd)) return vkCmdClearAttachments(cmd, count, atts, rectCount, rects);
    Writer w(Op::ClearAttachments, sizeof(Range) + sizeof(VkClearAttachment) * count + sizeof(VkClearRect) * rectCount);
    w.put(Range{count, rectCount}); w.array(atts, count); w.array(rects, rectCount);
}
void CmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                               const VkClearDepthStencilValue* value, uint32_t count, const VkImageSubresourceRange* ranges) {
    prof_op_begin(cmd, "clear depth");
    if (!deferred(cmd)) {
        vkCmdClearDepthStencilImage(cmd, image, layout, value, count, ranges);
    } else {
        Writer w(Op::ClearDepthStencilImage, sizeof(ClearDepth) + sizeof(VkImageSubresourceRange) * count);
        w.put(ClearDepth{image, layout, *value, count}); w.array(ranges, count);
    }
    prof_op_end(cmd);
}
void CmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count) {
    if (!deferred(cmd)) return vkCmdResetQueryPool(cmd, pool, first, count);
    Writer w(Op::ResetQueryPool, sizeof(ResetQuery)); w.put(ResetQuery{pool, first, count});
}
void CmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    if (!deferred(cmd)) return vkCmdWriteTimestamp(cmd, stage, pool, query);
    Writer w(Op::WriteTimestamp, sizeof(Timestamp)); w.put(Timestamp{stage, pool, query});
}

// GPU breadcrumbs (WWHD_GPU_BREADCRUMBS=1): to find the work a GPU hang is stuck in. After each render
// pass, dispatch, copy, blit or clear the record thread adds a barrier that waits for everything
// before it and a fill of a host-visible word with the work's number; a ring keeps a description of
// each numbered piece of work. After VK_ERROR_DEVICE_LOST (crumbs_report) the word holds the last
// piece the GPU finished, so the next one in the ring is where it hung. Serializes the GPU: a debug
// switch only (the record thread must be on).
namespace {
struct Crumb {
    uint32_t seq = 0;
    uint64_t stream = 0;
    char what[200] = {};
};
constexpr uint32_t kCrumbs = 2048;
Crumb g_crumbs[kCrumbs];
uint32_t g_crumb_seq = 0;  // record thread
std::atomic<uint32_t> g_crumb_seq_seen{0};  // g_crumb_seq once its stream is submitted (crumbs_submitted)
uint64_t g_crumb_stream = 0;
VkBuffer g_crumb_buf = VK_NULL_HANDLE;
VmaAllocation g_crumb_alloc = nullptr;
volatile uint32_t* g_crumb_word = nullptr;
std::mutex g_crumb_pipe_mutex;
std::unordered_map<VkPipeline, std::pair<uint64_t, uint64_t>> g_crumb_pipes;
// the render pass being replayed
struct CrumbPass {
    bool open = false;
    uint32_t w = 0, h = 0, draws = 0;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkPipeline pipes[6] = {};
    uint32_t nPipes = 0;
    bool more = false;
} g_crumb_pass;

bool crumb_buffer() {
    if (g_crumb_word) return true;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 64;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &g_crumb_buf, &g_crumb_alloc, &info) != VK_SUCCESS) return false;
    g_crumb_word = (volatile uint32_t*)info.pMappedData;
    *g_crumb_word = 0;
    LOG("[vk] GPU breadcrumbs on (WWHD_GPU_BREADCRUMBS): the GPU runs one piece of work at a time");
    // the driver recovers from a hang by skipping the submission (up to 3 times before the device is
    // lost): log each piece of work the GPU stops at for 100 ms while more is recorded
    std::thread([] {
        uint32_t last = 0, reported = 0;
        auto since = std::chrono::steady_clock::now();
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            uint32_t done = *g_crumb_word, seq = g_crumb_seq_seen.load(std::memory_order_relaxed);
            auto now = std::chrono::steady_clock::now();
            if (done != last || done >= seq) {
                last = done;
                since = now;
                continue;
            }
            if (reported != done + 1 && now - since > std::chrono::milliseconds(100)) {
                reported = done + 1;
                const Crumb& c = g_crumbs[reported % kCrumbs];
                LOG("[vk] GPU breadcrumbs: no progress for 100 ms at #%u (stream %llu): %s", reported,
                    (unsigned long long)c.stream, c.seq == reported ? c.what : "?");
                const Crumb& p = g_crumbs[done % kCrumbs];
                if (p.seq == done) LOG("[vk]   after #%u (stream %llu): %s", done, (unsigned long long)p.stream, p.what);
            }
        }
    }).detach();
    return true;
}

void crumb_mark(VkCommandBuffer cmd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void crumb_mark(VkCommandBuffer cmd, const char* fmt, ...) {
    if (!crumb_buffer()) return;
    uint32_t seq = ++g_crumb_seq;
    Crumb& c = g_crumbs[seq % kCrumbs];
    c.stream = g_crumb_stream;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c.what, sizeof c.what, fmt, ap);
    va_end(ap);
    c.seq = seq;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vkCmdFillBuffer(cmd, g_crumb_buf, 0, 4, seq);
}

void crumb_end_pass(VkCommandBuffer cmd) {
    CrumbPass& p = g_crumb_pass;
    char pipes[160] = {};
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(g_crumb_pipe_mutex);
        for (uint32_t i = 0; i < p.nPipes && n < sizeof pipes - 40; i++) {
            auto it = g_crumb_pipes.find(p.pipes[i]);
            if (it != g_crumb_pipes.end())
                n += snprintf(pipes + n, sizeof pipes - n, " %016llx/%016llx", (unsigned long long)it->second.first,
                              (unsigned long long)it->second.second);
            else
                n += snprintf(pipes + n, sizeof pipes - n, " (own)");
        }
    }
    crumb_mark(cmd, "pass %ux%u rp %p, %u draws, vs/ps%s%s", p.w, p.h, (void*)p.rp, p.draws, pipes, p.more ? " …" : "");
    p = CrumbPass{};
}
}  // namespace

bool crumbs_enabled() {
    static const bool on = getenv("WWHD_GPU_BREADCRUMBS") != nullptr;
    return on;
}

void crumb_pipeline(VkPipeline pipeline, uint64_t vsKey, uint64_t psKey) {
    if (!crumbs_enabled()) return;
    std::lock_guard<std::mutex> lk(g_crumb_pipe_mutex);
    g_crumb_pipes[pipeline] = {vsKey, psKey};
}

void crumbs_submitted() {
    if (crumbs_enabled()) g_crumb_seq_seen.store(g_crumb_seq, std::memory_order_relaxed);
}

void crumbs_report() {
    if (!crumbs_enabled() || !g_crumb_word) return;
    uint32_t done = *g_crumb_word, last = g_crumb_seq;
    LOG("[vk] GPU breadcrumbs: the GPU finished #%u of #%u recorded", done, last);
    for (uint32_t s = done > 3 ? done - 3 : 1; s <= last && s <= done + 6; s++) {
        const Crumb& c = g_crumbs[s % kCrumbs];
        if (c.seq != s) continue;
        LOG("[vk]   %s #%u (stream %llu): %s", s <= done ? "done" : s == done + 1 ? "HUNG" : "after", s,
            (unsigned long long)c.stream, c.what);
    }
}

void replay(const Stream& stream, VkCommandBuffer cmd) {
    const bool crumbs = crumbs_enabled();
    if (crumbs) g_crumb_stream++;
    size_t at = 0;
    while (at < stream.data.size()) {
        Header h; memcpy(&h, stream.data.data() + at, sizeof(h));
        Reader r{stream.data.data() + at, sizeof(Header)};
        switch (h.op) {
        case Op::BeginRenderPass: { auto v = r.get<VkRenderPassBeginInfo>(); auto c = r.get<VkSubpassContents>();
            v.pClearValues = r.array<VkClearValue>(v.clearValueCount); vkCmdBeginRenderPass(cmd, &v, c);
            if (crumbs) g_crumb_pass = CrumbPass{true, v.renderArea.extent.width, v.renderArea.extent.height, 0, v.renderPass};
            break; }
        case Op::EndRenderPass: vkCmdEndRenderPass(cmd); if (crumbs) crumb_end_pass(cmd); break;
        case Op::BindPipeline: { auto v = r.get<BindPipeline>(); vkCmdBindPipeline(cmd, v.point, v.pipeline);
            if (crumbs && g_crumb_pass.open) {
                CrumbPass& p = g_crumb_pass;
                bool seen = false;
                for (uint32_t i = 0; i < p.nPipes; i++) seen |= p.pipes[i] == v.pipeline;
                if (!seen) { if (p.nPipes < 6) p.pipes[p.nPipes++] = v.pipeline; else p.more = true; }
            }
            break; }
        case Op::BindDescriptorSets: { auto v = r.get<BindDescriptorSets>(); auto sets = r.array<VkDescriptorSet>(v.sets);
            auto offsets = r.array<uint32_t>(v.offsets); vkCmdBindDescriptorSets(cmd, v.point, v.layout, v.first, v.sets, sets, v.offsets, offsets); break; }
        case Op::BindVertexBuffers: { auto v = r.get<BindVertexBuffers>(); auto b = r.array<VkBuffer>(v.count);
            auto o = r.array<VkDeviceSize>(v.count); vkCmdBindVertexBuffers(cmd, v.first, v.count, b, o); break; }
        case Op::BindIndexBuffer: { auto v = r.get<BindIndexBuffer>(); vkCmdBindIndexBuffer(cmd, v.buffer, v.offset, v.type); break; }
        case Op::SetViewport: { auto v = r.get<Range>(); vkCmdSetViewport(cmd, v.first, v.count, r.array<VkViewport>(v.count)); break; }
        case Op::SetScissor: { auto v = r.get<Range>(); vkCmdSetScissor(cmd, v.first, v.count, r.array<VkRect2D>(v.count)); break; }
        case Op::SetBlendConstants: vkCmdSetBlendConstants(cmd, r.array<float>(4)); break;
        case Op::SetDepthBias: { auto v = r.get<DepthBias>(); vkCmdSetDepthBias(cmd, v.constant, v.clamp, v.slope); break; }
        case Op::SetStencilCompareMask: { auto v = r.get<Stencil>(); vkCmdSetStencilCompareMask(cmd, v.face, v.value); break; }
        case Op::SetStencilReference: { auto v = r.get<Stencil>(); vkCmdSetStencilReference(cmd, v.face, v.value); break; }
        case Op::SetStencilWriteMask: { auto v = r.get<Stencil>(); vkCmdSetStencilWriteMask(cmd, v.face, v.value); break; }
        case Op::PushConstants: { auto v = r.get<PushConstants>(); vkCmdPushConstants(cmd, v.layout, v.stages, v.offset, v.size, r.bytes(v.size)); break; }
        case Op::Draw: { auto v = r.get<Draw>(); vkCmdDraw(cmd, v.vertices, v.instances, v.firstVertex, v.firstInstance); g_crumb_pass.draws++; break; }
        case Op::DrawIndexed: { auto v = r.get<DrawIndexed>(); vkCmdDrawIndexed(cmd, v.indices, v.instances, v.firstIndex, v.vertexOffset, v.firstInstance); g_crumb_pass.draws++; break; }
        case Op::Dispatch: { auto v = r.get<Dispatch>(); vkCmdDispatch(cmd, v.x, v.y, v.z);
            if (crumbs) crumb_mark(cmd, "dispatch %ux%ux%u", v.x, v.y, v.z);
            break; }
        case Op::PipelineBarrier: { auto v = r.get<PipelineBarrier>(); auto m = r.array<VkMemoryBarrier>(v.mem);
            auto b = r.array<VkBufferMemoryBarrier>(v.buf); auto i = r.array<VkImageMemoryBarrier>(v.img);
            vkCmdPipelineBarrier(cmd, v.src, v.dst, v.deps, v.mem, m, v.buf, b, v.img, i); break; }
        case Op::CopyImage: { auto v = r.get<CopyImage>(); vkCmdCopyImage(cmd, v.src, v.srcLayout, v.dst, v.dstLayout, v.count, r.array<VkImageCopy>(v.count)); break; }
        case Op::CopyImage2: { auto v = r.get<VkCopyImageInfo2>(); v.pRegions = r.array<VkImageCopy2>(v.regionCount); vkCmdCopyImage2(cmd, &v); break; }
        case Op::BlitImage: { auto v = r.get<BlitImage>(); vkCmdBlitImage(cmd, v.src, v.srcLayout, v.dst, v.dstLayout, v.count, r.array<VkImageBlit>(v.count), v.filter); break; }
        case Op::CopyBufferToImage: { auto v = r.get<CopyBufferImage>(); vkCmdCopyBufferToImage(cmd, v.buffer, v.image, v.layout, v.count, r.array<VkBufferImageCopy>(v.count)); break; }
        case Op::CopyImageToBuffer: { auto v = r.get<CopyImageBuffer>(); vkCmdCopyImageToBuffer(cmd, v.image, v.layout, v.buffer, v.count, r.array<VkBufferImageCopy>(v.count)); break; }
        case Op::ClearColorImage: { auto v = r.get<ClearColor>(); vkCmdClearColorImage(cmd, v.image, v.layout, &v.color, v.count, r.array<VkImageSubresourceRange>(v.count)); break; }
        case Op::ClearDepthStencilImage: { auto v = r.get<ClearDepth>(); vkCmdClearDepthStencilImage(cmd, v.image, v.layout, &v.value, v.count, r.array<VkImageSubresourceRange>(v.count)); break; }
        case Op::ResetQueryPool: { auto v = r.get<ResetQuery>(); vkCmdResetQueryPool(cmd, v.pool, v.first, v.count); break; }
        case Op::WriteTimestamp: { auto v = r.get<Timestamp>(); vkCmdWriteTimestamp(cmd, v.stage, v.pool, v.query); break; }
        case Op::ClearAttachments: {
            auto v = r.get<Range>();  // first = attachments, count = rects
            const VkClearAttachment* atts = r.array<VkClearAttachment>(v.first);
            vkCmdClearAttachments(cmd, v.first, atts, v.count, r.array<VkClearRect>(v.count));
            break;
        }
        }
        if (crumbs && !g_crumb_pass.open) {
            static const char* const kNames[] = {"copy image", "copy image", "blit", "upload (buffer to image)",
                                                 "readback (image to buffer)", "clear color", "clear depth"};
            if (h.op >= Op::CopyImage && h.op <= Op::ClearDepthStencilImage)
                crumb_mark(cmd, "%s", kNames[(int)h.op - (int)Op::CopyImage]);
        }
        at += h.size;
    }
}

}  // namespace gfx::rec
