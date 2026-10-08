#define WWHD_VK_RECORD_IMPLEMENTATION
#include "vk_record.h"
#include "runtime.h"
#include "vk.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>

namespace gfx::rec {
namespace {

enum class Op : uint16_t {
    BeginRenderPass, EndRenderPass, BindPipeline, BindDescriptorSets, BindVertexBuffers, BindIndexBuffer,
    SetViewport, SetScissor, SetBlendConstants, SetDepthBias, SetStencilCompareMask, SetStencilReference,
    SetStencilWriteMask, PushConstants, Draw, DrawIndexed, Dispatch, PipelineBarrier, CopyImage, CopyImage2,
    BlitImage, CopyBufferToImage, CopyImageToBuffer, ClearColorImage, ClearDepthStencilImage, ResetQueryPool,
    WriteTimestamp,
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

// On with Qualcomm's own driver: on a tablet (Adreno 732, driver v762) the render thread went from
// 21.5 to 14.6 ms a frame (the record thread taking 6.4 ms beside it), no picture errors. Off with
// other drivers: on a Mi 9 (Adreno 640, Turnip) it made the render thread slower (17.5 -> 24 ms a
// frame, plus 9 ms on the record thread) and the picture flickered black now and then.
// WWHD_RECORD_THREAD=0 / 1 decides instead. Decided once the device exists (its driver is known).
bool enabled() {
    static int on = -1;
    if (on >= 0) return on != 0;
    if (const char* e = getenv("WWHD_RECORD_THREAD")) return (on = strcmp(e, "0") != 0);
    if (!R.device) return false;
    on = R.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
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
    if (!deferred(cmd)) return vkCmdDispatch(cmd, x, y, z);
    Writer w(Op::Dispatch, sizeof(Dispatch)); w.put(Dispatch{x, y, z});
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
    if (!deferred(cmd)) return vkCmdCopyImage(cmd, src, srcLayout, dst, dstLayout, count, regions);
    Writer w(Op::CopyImage, sizeof(CopyImage) + sizeof(VkImageCopy) * count);
    w.put(CopyImage{src, srcLayout, dst, dstLayout, count}); w.array(regions, count);
}
void CmdCopyImage2(VkCommandBuffer cmd, const VkCopyImageInfo2* info) {
    if (!deferred(cmd)) return vkCmdCopyImage2(cmd, info);
    VkCopyImageInfo2 copy = *info; copy.pNext = nullptr; copy.pRegions = nullptr;
    Writer w(Op::CopyImage2, sizeof(copy) + sizeof(VkImageCopy2) * info->regionCount); w.put(copy); w.array(info->pRegions, info->regionCount);
}
void CmdBlitImage(VkCommandBuffer cmd, VkImage src, VkImageLayout srcLayout, VkImage dst, VkImageLayout dstLayout,
                  uint32_t count, const VkImageBlit* regions, VkFilter filter) {
    if (!deferred(cmd)) return vkCmdBlitImage(cmd, src, srcLayout, dst, dstLayout, count, regions, filter);
    Writer w(Op::BlitImage, sizeof(BlitImage) + sizeof(VkImageBlit) * count);
    w.put(BlitImage{src, srcLayout, dst, dstLayout, count, filter}); w.array(regions, count);
}
void CmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer buffer, VkImage image, VkImageLayout layout,
                          uint32_t count, const VkBufferImageCopy* regions) {
    if (!deferred(cmd)) return vkCmdCopyBufferToImage(cmd, buffer, image, layout, count, regions);
    Writer w(Op::CopyBufferToImage, sizeof(CopyBufferImage) + sizeof(VkBufferImageCopy) * count);
    w.put(CopyBufferImage{buffer, image, layout, count}); w.array(regions, count);
}
void CmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, VkBuffer buffer,
                          uint32_t count, const VkBufferImageCopy* regions) {
    if (!deferred(cmd)) return vkCmdCopyImageToBuffer(cmd, image, layout, buffer, count, regions);
    Writer w(Op::CopyImageToBuffer, sizeof(CopyImageBuffer) + sizeof(VkBufferImageCopy) * count);
    w.put(CopyImageBuffer{image, layout, buffer, count}); w.array(regions, count);
}
void CmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const VkClearColorValue* color,
                        uint32_t count, const VkImageSubresourceRange* ranges) {
    if (!deferred(cmd)) return vkCmdClearColorImage(cmd, image, layout, color, count, ranges);
    Writer w(Op::ClearColorImage, sizeof(ClearColor) + sizeof(VkImageSubresourceRange) * count);
    w.put(ClearColor{image, layout, *color, count}); w.array(ranges, count);
}
void CmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                               const VkClearDepthStencilValue* value, uint32_t count, const VkImageSubresourceRange* ranges) {
    if (!deferred(cmd)) return vkCmdClearDepthStencilImage(cmd, image, layout, value, count, ranges);
    Writer w(Op::ClearDepthStencilImage, sizeof(ClearDepth) + sizeof(VkImageSubresourceRange) * count);
    w.put(ClearDepth{image, layout, *value, count}); w.array(ranges, count);
}
void CmdResetQueryPool(VkCommandBuffer cmd, VkQueryPool pool, uint32_t first, uint32_t count) {
    if (!deferred(cmd)) return vkCmdResetQueryPool(cmd, pool, first, count);
    Writer w(Op::ResetQueryPool, sizeof(ResetQuery)); w.put(ResetQuery{pool, first, count});
}
void CmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, VkQueryPool pool, uint32_t query) {
    if (!deferred(cmd)) return vkCmdWriteTimestamp(cmd, stage, pool, query);
    Writer w(Op::WriteTimestamp, sizeof(Timestamp)); w.put(Timestamp{stage, pool, query});
}

void replay(const Stream& stream, VkCommandBuffer cmd) {
    size_t at = 0;
    while (at < stream.data.size()) {
        Header h; memcpy(&h, stream.data.data() + at, sizeof(h));
        Reader r{stream.data.data() + at, sizeof(Header)};
        switch (h.op) {
        case Op::BeginRenderPass: { auto v = r.get<VkRenderPassBeginInfo>(); auto c = r.get<VkSubpassContents>();
            v.pClearValues = r.array<VkClearValue>(v.clearValueCount); vkCmdBeginRenderPass(cmd, &v, c); break; }
        case Op::EndRenderPass: vkCmdEndRenderPass(cmd); break;
        case Op::BindPipeline: { auto v = r.get<BindPipeline>(); vkCmdBindPipeline(cmd, v.point, v.pipeline); break; }
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
        case Op::Draw: { auto v = r.get<Draw>(); vkCmdDraw(cmd, v.vertices, v.instances, v.firstVertex, v.firstInstance); break; }
        case Op::DrawIndexed: { auto v = r.get<DrawIndexed>(); vkCmdDrawIndexed(cmd, v.indices, v.instances, v.firstIndex, v.vertexOffset, v.firstInstance); break; }
        case Op::Dispatch: { auto v = r.get<Dispatch>(); vkCmdDispatch(cmd, v.x, v.y, v.z); break; }
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
        }
        at += h.size;
    }
}

}  // namespace gfx::rec
