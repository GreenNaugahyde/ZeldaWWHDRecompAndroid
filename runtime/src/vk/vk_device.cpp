// Vulkan renderer: device, submission, transient memory, image layouts, presentation into the
// Android window, clears, copies and debug image dumps. Counterpart of gfx/metal_main.mm.
#include "vk.h"

#include <adrenotools/driver.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <thread>

#include "gx2/gx2.h"
#include "lsfg.h"
#include "platform.h"
#include "runtime.h"
#include "vk.h"
#include "vk_window.h"
#include "../aspect.h"

namespace gfx {
Renderer R;
namespace { void count_present(); }
static std::atomic<bool> g_fast_forward{false};  // set_fast_forward: no generated frames

const char* backend_name() { return "Vulkan"; }
uint64_t current_frame() { return R.frame; }

// ---------------------------------------------------------------- transient memory
// Per-draw data comes from large host-visible buffers recycled once the GPU is done with them
// (the Metal renderer reads guest memory directly; Android GPUs can't import it).
namespace {
struct Chunk {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0;
};
constexpr VkDeviceSize kChunkSize = 32ull << 20;
std::vector<Chunk*> g_chunk_free, g_chunk_used;
Chunk* g_chunk_cur = nullptr;
VkDeviceSize g_chunk_pos = 0;

Chunk* new_chunk(VkDeviceSize size) {
    auto* c = new Chunk();
    c->size = size;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    // storage: the GPU texture decoder reads its input and writes its output here (vk_surfaces.cpp)
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    // cached memory where the device has it coherent (mobile GPUs share memory with the CPU): draws
    // compare guest data with what earlier draws already copied here (vk_draw.cpp, copy_deduped)
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;  // written without explicit flushes
    ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VmaAllocationInfo info{};
    VK_CHECK(vmaCreateBuffer(R.vma, &bi, &ai, &c->buf, &c->alloc, &info));
    c->ptr = (uint8_t*)info.pMappedData;
    VkMemoryPropertyFlags mf = 0;
    vmaGetAllocationMemoryProperties(R.vma, c->alloc, &mf);
    static bool logged = false;
    if (!logged) {
        logged = true;
        R.uploadCached = (mf & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
        LOG("[vk] transient upload memory: %s", R.uploadCached ? "cached (copies deduplicated)" : "uncached");
    }
    return c;
}

// descriptor pools, retired with the submission that used their sets
std::vector<VkDescriptorPool> g_pool_free, g_pool_used;
VkDescriptorPool g_pool_cur = VK_NULL_HANDLE;
}  // namespace

Upload upload_alloc(VkDeviceSize size, VkDeviceSize align) {
    align = std::max<VkDeviceSize>(align, 4);
    VkDeviceSize start = g_chunk_cur ? (g_chunk_pos + align - 1) & ~(align - 1) : 0;
    if (!g_chunk_cur || start + size > g_chunk_cur->size) {
        if (g_chunk_cur) g_chunk_used.push_back(g_chunk_cur);
        VkDeviceSize want = std::max(kChunkSize, size);
        g_chunk_cur = nullptr;
        for (size_t i = 0; i < g_chunk_free.size(); i++)
            if (g_chunk_free[i]->size >= want) {
                g_chunk_cur = g_chunk_free[i];
                g_chunk_free.erase(g_chunk_free.begin() + i);
                break;
            }
        if (!g_chunk_cur) g_chunk_cur = new_chunk(want);
        start = 0;
    }
    g_chunk_pos = start + size;
    return Upload{g_chunk_cur->buf, start, g_chunk_cur->ptr + start};
}

Upload upload(const void* data, VkDeviceSize size, VkDeviceSize align) {
    Upload u = upload_alloc(size, align);
    memcpy(u.ptr, data, size);
    return u;
}


VkDescriptorSet alloc_descriptor_set(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!g_pool_cur) {
            if (!g_pool_free.empty()) {
                g_pool_cur = g_pool_free.back();
                g_pool_free.pop_back();
            } else {
                VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192},
                                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}};
                VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
                pi.maxSets = 2048;
                pi.poolSizeCount = 3;
                pi.pPoolSizes = sizes;
                VK_CHECK(vkCreateDescriptorPool(R.device, &pi, nullptr, &g_pool_cur));
            }
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_pool_cur;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(R.device, &ai, &set) == VK_SUCCESS) return set;
        g_pool_used.push_back(g_pool_cur);  // full
        g_pool_cur = VK_NULL_HANDLE;
    }
    fatal("cannot allocate a descriptor set");
}

// ---------------------------------------------------------------- submission
namespace {
struct InFlight {
    VkFence fence;
    VkCommandBuffer cmd;
    std::vector<Chunk*> chunks;
    std::vector<VkDescriptorPool> pools;
    std::vector<std::function<void()>> done;
};
std::deque<InFlight> g_inflight;
std::vector<std::function<void()>> g_pending_done;
std::vector<VkFence> g_fence_free;
std::vector<VkCommandBuffer> g_cmd_free;
uint32_t g_draws_since_commit_ = 0;

void retire(InFlight& f) {
    for (auto& fn : f.done) fn();
    for (Chunk* c : f.chunks) g_chunk_free.push_back(c);
    for (VkDescriptorPool p : f.pools) {
        vkResetDescriptorPool(R.device, p, 0);
        g_pool_free.push_back(p);
    }
    vkResetFences(R.device, 1, &f.fence);
    g_fence_free.push_back(f.fence);
    vkResetCommandBuffer(f.cmd, 0);
    g_cmd_free.push_back(f.cmd);
}

// With deferred recording, the render thread owns transient allocations until it seals a stream.
// The record thread then records/submits that stream and returns the allocations only after its GPU
// fence signals. Completion callbacks remain on the render thread because they mutate renderer caches.
struct RecordJob {
    uint64_t id = 0;
    uint64_t presentId = 0;
    bool present = false, countPresent = false;
    rec::Stream stream;
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    std::atomic<bool>* presentOutdated = nullptr;
    std::vector<Chunk*> chunks;
    std::vector<VkDescriptorPool> pools;
    std::vector<std::function<void()>> done;
};
struct RecordDone {
    uint64_t id = 0;
    std::vector<Chunk*> chunks;
    std::vector<VkDescriptorPool> pools;
    std::vector<std::function<void()>> done;
};
struct RecordFlight {
    RecordJob job;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
};
std::mutex g_record_mutex;
std::condition_variable g_record_cv;
std::deque<RecordJob> g_record_jobs;
std::deque<RecordDone> g_record_done;
uint64_t g_record_next_id = 0, g_record_submitted_id = 0, g_record_complete_id = 0;
uint64_t g_record_present_next = 0, g_record_present_done = 0;
std::atomic<uint64_t> g_record_cpu_ns{0}, g_record_stream_bytes{0}, g_record_streams{0};

uint64_t thread_cpu_ns() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

void record_thread_main() {
    platform::set_thread_name("GX2 record");
    platform::set_thread_high_priority();  // also joins the performance hint session (Android)
    std::deque<RecordFlight> flights;
    std::vector<VkFence> fences;
    std::vector<VkCommandBuffer> commands;
    for (;;) {
        RecordJob job;
        {
            std::unique_lock<std::mutex> lk(g_record_mutex);
            if (g_record_jobs.empty()) {
                if (flights.empty()) g_record_cv.wait(lk, [] { return !g_record_jobs.empty(); });
                else g_record_cv.wait_for(lk, std::chrono::milliseconds(1));
            }
            if (!g_record_jobs.empty()) {
                job = std::move(g_record_jobs.front());
                g_record_jobs.pop_front();
            }
        }
        if (job.present) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &job.wait;
            pi.swapchainCount = 1;
            pi.pSwapchains = &job.swapchain;
            pi.pImageIndices = &job.imageIndex;
            VkResult r;
            {
                std::lock_guard<std::mutex> lk(R.queueMutex);
                r = vkQueuePresentKHR(R.queue, &pi);
            }
            if (r == VK_ERROR_OUT_OF_DATE_KHR && job.presentOutdated)
                job.presentOutdated->store(true, std::memory_order_release);
            if (job.countPresent) count_present();
            {
                std::lock_guard<std::mutex> lk(g_record_mutex);
                g_record_present_done = job.presentId;
            }
            g_record_cv.notify_all();
        } else if (job.id) {
            uint64_t cpu0 = thread_cpu_ns();
            VkCommandBuffer cmd;
            if (!commands.empty()) {
                cmd = commands.back();
                commands.pop_back();
            } else {
                VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                ai.commandPool = R.cmdPool;
                ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = 1;
                VK_CHECK(vkAllocateCommandBuffers(R.device, &ai, &cmd));
            }
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            size_t streamBytes = job.stream.data.size();
            rec::replay(job.stream, cmd);
            rec::recycle(std::move(job.stream));
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkFence fence;
            if (!fences.empty()) {
                fence = fences.back();
                fences.pop_back();
            } else {
                VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                VK_CHECK(vkCreateFence(R.device, &fi, nullptr, &fence));
            }
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            if (job.wait) {
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &job.wait;
                si.pWaitDstStageMask = &stage;
            }
            if (job.signal) {
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &job.signal;
            }
            {
                std::lock_guard<std::mutex> lk(R.queueMutex);
                VK_CHECK(vkQueueSubmit(R.queue, 1, &si, fence));
            }
            g_record_cpu_ns.fetch_add(thread_cpu_ns() - cpu0, std::memory_order_relaxed);
            g_record_stream_bytes.fetch_add(streamBytes, std::memory_order_relaxed);
            g_record_streams.fetch_add(1, std::memory_order_relaxed);
            uint64_t id = job.id;
            flights.push_back(RecordFlight{std::move(job), fence, cmd});
            {
                std::lock_guard<std::mutex> lk(g_record_mutex);
                g_record_submitted_id = id;
            }
            g_record_cv.notify_all();
        }
        while (!flights.empty()) {
            RecordFlight& f = flights.front();
            if (flights.size() > 16)
                vkWaitForFences(R.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
            else if (vkGetFenceStatus(R.device, f.fence) != VK_SUCCESS)
                break;
            RecordDone done;
            done.id = f.job.id;
            done.chunks = std::move(f.job.chunks);
            done.pools = std::move(f.job.pools);
            done.done = std::move(f.job.done);
            for (VkDescriptorPool p : done.pools) vkResetDescriptorPool(R.device, p, 0);
            vkResetFences(R.device, 1, &f.fence);
            vkResetCommandBuffer(f.cmd, 0);
            fences.push_back(f.fence);
            commands.push_back(f.cmd);
            flights.pop_front();
            {
                std::lock_guard<std::mutex> lk(g_record_mutex);
                g_record_complete_id = done.id;
                g_record_done.push_back(std::move(done));
            }
            g_record_cv.notify_all();
        }
    }
}

void start_record_thread() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(record_thread_main).detach(); });
}

void drain_record_done() {
    std::deque<RecordDone> done;
    {
        std::lock_guard<std::mutex> lk(g_record_mutex);
        done.swap(g_record_done);
    }
    for (auto& f : done) {
        for (auto& fn : f.done) fn();
        for (Chunk* c : f.chunks) g_chunk_free.push_back(c);
        for (VkDescriptorPool p : f.pools) g_pool_free.push_back(p);
    }
}

uint64_t enqueue_record(RecordJob job) {
    start_record_thread();
    std::unique_lock<std::mutex> lk(g_record_mutex);
    g_record_cv.wait(lk, [] { return g_record_next_id - g_record_complete_id < 16; });
    job.id = ++g_record_next_id;
    uint64_t id = job.id;
    g_record_jobs.push_back(std::move(job));
    lk.unlock();
    g_record_cv.notify_all();
    return id;
}

void enqueue_record_present(VkSemaphore wait, VkSwapchainKHR swapchain, uint32_t imageIndex,
                            std::atomic<bool>* outdated, bool count) {
    start_record_thread();
    RecordJob job;
    job.present = true;
    job.countPresent = count;
    job.wait = wait;
    job.swapchain = swapchain;
    job.imageIndex = imageIndex;
    job.presentOutdated = outdated;
    {
        std::lock_guard<std::mutex> lk(g_record_mutex);
        job.presentId = ++g_record_present_next;
        g_record_jobs.push_back(std::move(job));
    }
    g_record_cv.notify_all();
}

void wait_record_submitted(uint64_t id) {
    if (!id) return;
    std::unique_lock<std::mutex> lk(g_record_mutex);
    g_record_cv.wait(lk, [=] { return g_record_submitted_id >= id; });
}

void wait_record_idle(bool drain = true) {
    uint64_t id, presentId;
    {
        std::lock_guard<std::mutex> lk(g_record_mutex);
        id = g_record_next_id;
        presentId = g_record_present_next;
    }
    if (id || presentId) {
        std::unique_lock<std::mutex> lk(g_record_mutex);
        g_record_cv.wait(lk, [=] { return g_record_complete_id >= id && g_record_present_done >= presentId; });
    }
    if (drain) drain_record_done();
}

// retire finished submissions; `wait` blocks for all of them
void poll(bool wait = false) {
    while (!g_inflight.empty()) {
        InFlight& f = g_inflight.front();
        if (wait) vkWaitForFences(R.device, 1, &f.fence, VK_TRUE, UINT64_MAX);
        else if (vkGetFenceStatus(R.device, f.fence) != VK_SUCCESS) break;
        InFlight done = std::move(f);
        g_inflight.pop_front();
        retire(done);
    }
}

uint64_t submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE) {
    end_pass();
    if (!R.cmd) {
        if (!wait && !signal) return 0;
        command_buffer();
    }
    prof_cmd_end();
    if (rec::enabled()) {
        RecordJob job;
        job.stream = rec::finish();
        job.wait = wait;
        job.signal = signal;
        R.cmd = VK_NULL_HANDLE;
        if (g_chunk_cur) {
            g_chunk_used.push_back(g_chunk_cur);
            g_chunk_cur = nullptr;
        }
        job.chunks.swap(g_chunk_used);
        if (g_pool_cur) {
            g_pool_used.push_back(g_pool_cur);
            g_pool_cur = VK_NULL_HANDLE;
        }
        job.pools.swap(g_pool_used);
        job.done.swap(g_pending_done);
        g_draws_since_commit_ = 0;
        drain_record_done();
        return enqueue_record(std::move(job));
    }
    VK_CHECK(vkEndCommandBuffer(R.cmd));
    InFlight f;
    f.cmd = R.cmd;
    R.cmd = VK_NULL_HANDLE;
    if (!g_fence_free.empty()) {
        f.fence = g_fence_free.back();
        g_fence_free.pop_back();
    } else {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(R.device, &fi, nullptr, &f.fence));
    }
    if (g_chunk_cur) {
        g_chunk_used.push_back(g_chunk_cur);
        g_chunk_cur = nullptr;
    }
    f.chunks.swap(g_chunk_used);
    if (g_pool_cur) {
        g_pool_used.push_back(g_pool_cur);
        g_pool_cur = VK_NULL_HANDLE;
    }
    f.pools.swap(g_pool_used);
    f.done.swap(g_pending_done);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
        si.pWaitDstStageMask = &stage;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    {
        std::lock_guard<std::mutex> lk(R.queueMutex);
        VK_CHECK(vkQueueSubmit(R.queue, 1, &si, f.fence));
    }
    g_inflight.push_back(std::move(f));
    g_draws_since_commit_ = 0;
    // bound the work in flight (memory held by transient buffers)
    while (g_inflight.size() > 16) {
        vkWaitForFences(R.device, 1, &g_inflight.front().fence, VK_TRUE, UINT64_MAX);
        poll();
    }
    poll();
    return 0;
}
}  // namespace

uint32_t& draws_since_commit() { return g_draws_since_commit_; }

VkCommandBuffer command_buffer() {
    if (R.cmd) return R.cmd;
    if (rec::enabled()) {
        drain_record_done();
        rec::begin();
        R.cmd = rec::virtual_command_buffer();
        R.cmdSerial++;
        prof_cmd_begin();
        return R.cmd;
    }
    if (!g_cmd_free.empty()) {
        R.cmd = g_cmd_free.back();
        g_cmd_free.pop_back();
    } else {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = R.cmdPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(R.device, &ai, &R.cmd));
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(R.cmd, &bi));
    R.cmdSerial++;
    prof_cmd_begin();
    return R.cmd;
}

void on_complete(std::function<void()> fn) { g_pending_done.push_back(std::move(fn)); }

void end_pass() {
    if (R.pass) {
        vkCmdEndRenderPass(R.cmd);
        prof_pass_end();
        R.pass = VK_NULL_HANDLE;
        // submit work in chunks so the GPU starts while the frame is still being built (like the
        // hardware command processor), instead of all at once on swap
        static const bool chunked = getenv("WWHD_NO_CHUNK") == nullptr;
        if (chunked && g_draws_since_commit_ >= 1024) submit();
    }
    for (auto& c : R.passColor) c = nullptr;
    R.passDepth = nullptr;
}

void flush() { submit(); }

void wait_idle() {
    submit();
    if (rec::enabled()) wait_record_idle();
    else poll(true);
}

// GX2DrawDone. The game waits here before reusing memory the GPU reads, or to read what it wrote.
// This renderer copies all guest data (vertices, uniforms, textures) when commands are recorded and
// writes results back to guest memory only for linear surfaces (write_back_linear), so once the render thread has processed the commands
// (the caller syncs with it) there is nothing left to wait for. Waiting for the GPU as well would
// serialize CPU and GPU: WWHD calls this twice a frame, which kept the GPU half idle and at its
// lowest clock. Frame pacing still waits for finished frames (flips). WWHD_STRICT_DRAWDONE=1 waits.
void draw_done() {
    static const bool strict = getenv("WWHD_STRICT_DRAWDONE") != nullptr;
    if (strict) wait_idle();
    else submit();
    write_back_linear();  // except what the CPU reads: linear targets (the Picto Box picture, issue #22)
}

// ---------------------------------------------------------------- image layouts
static VkImageLayout layout_for(Use u, const Image& img) {
    switch (u) {
    case Use::SAMPLED: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case Use::COLOR: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case Use::DEPTH: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case Use::COPY_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case Use::COPY_DST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_GENERAL;
    }
}

// The pipeline stages and accesses of a use of an image. Precise masks let the GPU overlap
// unrelated work across a layout change (tiled GPUs run the next pass's vertex work while the
// previous pass's pixels are still being shaded); WWHD_BROAD_BARRIERS=1 waits for everything.
static bool broad_barriers() {
    static const bool b = getenv("WWHD_BROAD_BARRIERS") != nullptr;
    return b;
}
static VkPipelineStageFlags use_stages(Use u) {
    switch (u) {
    case Use::SAMPLED: return VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case Use::COLOR: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case Use::DEPTH: return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case Use::COPY_SRC: case Use::COPY_DST: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    default: return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}
static VkAccessFlags use_writes(Use u) {
    switch (u) {
    case Use::COLOR: return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case Use::DEPTH: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case Use::COPY_DST: return VK_ACCESS_TRANSFER_WRITE_BIT;
    default: return 0;  // reads: an execution dependency is enough
    }
}
static VkAccessFlags use_accesses(Use u) {
    switch (u) {
    case Use::SAMPLED: return VK_ACCESS_SHADER_READ_BIT;
    case Use::COLOR: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case Use::DEPTH: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case Use::COPY_SRC: return VK_ACCESS_TRANSFER_READ_BIT;
    case Use::COPY_DST: return VK_ACCESS_TRANSFER_WRITE_BIT;
    default: return 0;
    }
}

void prepare(Image& img, Use use) {
    if (!img.image) return;
    // render passes order attachment writes among themselves (subpass dependencies); reads after reads need nothing
    if (img.use == use && (use == Use::SAMPLED || use == Use::COLOR || use == Use::DEPTH)) return;
    end_pass();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    const bool broad = broad_barriers();
    VkPipelineStageFlags srcStage = broad ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : use_stages(img.use);
    VkPipelineStageFlags dstStage = broad ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : use_stages(use);
    if (broad) {
        b.srcAccessMask = img.use == Use::NONE ? 0 : VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    } else {
        b.srcAccessMask = use_writes(img.use);
        b.dstAccessMask = use_accesses(use);
    }
    b.oldLayout = img.layout;
    b.newLayout = layout_for(use, img);
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdPipelineBarrier(command_buffer(), srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    img.layout = b.newLayout;
    img.use = use;
}

// ---------------------------------------------------------------- images
bool create_image(Image& img, VkImageType type, VkImageViewType viewType, VkFormat format, uint32_t w, uint32_t h, uint32_t depth,
                  uint32_t layers, uint32_t mips, VkImageUsageFlags usage, bool cube, bool isDepth, bool stencil) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = type;
    ci.format = format;
    ci.extent = {w, h, depth};
    ci.mipLevels = mips;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(R.vma, &ci, &ai, &img.image, &img.alloc, nullptr) != VK_SUCCESS) {
        img.image = VK_NULL_HANDLE;
        return false;
    }
    img.format = format;
    img.type = type;
    img.viewType = viewType;
    img.width = w;
    img.height = h;
    img.depth = depth;
    img.layers = layers;
    img.mips = mips;
    img.cube = cube;
    img.aspect = isDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    img.use = Use::NONE;
    img.attachViews.assign(layers, VK_NULL_HANDLE);
    return true;
}

VkImageView make_view(Image& img, VkImageViewType type, uint32_t baseLayer, uint32_t layers, VkComponentMapping sw,
                      VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = type;
    vi.format = img.format;
    vi.components = sw;
    vi.subresourceRange = {aspect, 0, img.mips, baseLayer, layers};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(R.device, &vi, nullptr, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

VkImageView attachment_view(Image& img, uint32_t layer) {
    if (layer >= img.attachViews.size()) return VK_NULL_HANDLE;
    VkImageView& v = img.attachViews[layer];
    if (!v) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = img.format;
        vi.subresourceRange = {img.aspect, 0, 1, layer, 1};
        VK_CHECK(vkCreateImageView(R.device, &vi, nullptr, &v));
    }
    return v;
}

// ---------------------------------------------------------------- device
static bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (auto& e : exts)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

// one pipeline cache per driver (a user-installed GPU driver and the system's keep their own);
// pipelines.vkcache and pipelines-*.vkcache are the names of builds before (the latter holds pipelines
// of shaders translated before strictMul: not loaded, so they don't stay in the cache for good)
static std::string pipeline_cache_path(bool legacy = false) {
    std::string dir = config::cache_dir.empty() ? "." : config::cache_dir;
    if (legacy) return dir + "/pipelines.vkcache";
    uint32_t h = 2166136261u;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    };
    mix(&R.props.vendorID, 4);
    mix(&R.props.deviceID, 4);
    mix(R.props.pipelineCacheUUID, VK_UUID_SIZE);
    char name[40];
    snprintf(name, sizeof name, "/pipelines2-%08x.vkcache", h);
    return dir + name;
}
static std::string pipeline_cache_path_v1() {
    std::string p = pipeline_cache_path();
    return p.replace(p.rfind("/pipelines2-"), 12, "/pipelines-");
}

static void load_pipeline_cache() {
    std::vector<uint8_t> data;
    if (const char* e = getenv("WWHD_SHADER_CACHE"); !(e && !strcmp(e, "0"))) {
        FILE* f = fopen(pipeline_cache_path().c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                data.resize(n);
                if (fread(data.data(), 1, n, f) != (size_t)n) data.clear();
            }
            fclose(f);
        }
    }
    // only data of this driver and GPU: some drivers misbehave on another driver's cache (switching
    // between the system driver and an installed one) instead of rejecting it
    struct Header {
        uint32_t size, version, vendor, device;
        uint8_t uuid[VK_UUID_SIZE];
    } h;
    if (data.size() >= sizeof h) {
        memcpy(&h, data.data(), sizeof h);
        if (h.vendor != R.props.vendorID || h.device != R.props.deviceID || memcmp(h.uuid, R.props.pipelineCacheUUID, VK_UUID_SIZE)) {
            LOG("[vk] pipeline cache is from another driver: starting empty");
            data.clear();
        }
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    // the driver rejects data from another driver version: start empty then
    if (vkCreatePipelineCache(R.device, &ci, nullptr, &R.pipelineCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        VK_CHECK(vkCreatePipelineCache(R.device, &ci, nullptr, &R.pipelineCache));
    }
    if (!data.empty()) LOG("[vk] pipeline cache: %zu KiB", data.size() / 1024);
}

void save_caches() {
    if (!R.pipelineCache) return;
    if (const char* e = getenv("WWHD_SHADER_CACHE"); e && !strcmp(e, "0")) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(R.device, R.pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(R.device, R.pipelineCache, &n, data.data()) != VK_SUCCESS) return;
    std::string path = pipeline_cache_path(), tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        bool ok = fwrite(data.data(), 1, n, f) == n;
        ok &= fclose(f) == 0;
        if (ok && rename(tmp.c_str(), path.c_str()) == 0) {
            remove(pipeline_cache_path(true).c_str());
            remove(pipeline_cache_path_v1().c_str());
        }
    }
}

// The Vulkan library: the system's, or on Adreno GPUs one whose driver is a package the user installed
// (libadrenotools loads it in place of the system driver). MainActivity passes the installed driver:
// WWHD_GPU_DRIVER_DIR (with a trailing /), WWHD_GPU_DRIVER_LIB (its file name) and
// WWHD_GPU_HOOK_DIR (libadrenotools' hook libraries).
static std::string g_driver_file;  // the installed driver asked for ("dir/lib"), or ""
static bool g_driver_fallback = false;  // ... but the system's runs (libadrenotools falls back when it can't load it)

static void load_vulkan() {
    void* lib = nullptr;
    const char* dir = getenv("WWHD_GPU_DRIVER_DIR");
    const char* name = getenv("WWHD_GPU_DRIVER_LIB");
    const char* hooks = getenv("WWHD_GPU_HOOK_DIR");
    if (dir && *dir && name && *name && hooks && *hooks) {
        g_driver_file = std::string(dir) + name;
        lib = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hooks, dir, name, nullptr, nullptr);
        if (!lib) g_driver_fallback = true;
        LOG("[vk] GPU driver %s%s: %s", dir, name, lib ? "loading" : "could not be loaded, using the system driver");
    }
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    auto gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
    if (!gipa) fatal("Vulkan is not available");
    volkInitializeCustom(gipa);
}

// true if the installed driver's library is mapped into the process (the driver loads with the instance)
static bool driver_file_mapped() {
    // its directory and file name (the linker may report the path as /data/data/... or /data/user/0/...)
    std::string tail = g_driver_file.substr(g_driver_file.rfind('/', g_driver_file.rfind('/') - 1));
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return true;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) found = strstr(line, tail.c_str()) != nullptr;
    fclose(f);
    return found;
}

// the driver's name and version for people: Qualcomm's own version number (as V@0762.24 in its
// build string) and date instead of its four-line build information
static std::string describe_driver(const VkPhysicalDeviceDriverProperties& dp) {
    std::string info = dp.driverInfo;
    if (dp.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
        uint32_t v = R.props.driverVersion;
        char s[96];
        snprintf(s, sizeof s, "Qualcomm Adreno driver v%u.%02u", (v >> 12) & 0x3FF, v & 0xFFF);
        std::string out = s;
        size_t d = info.find("Date: ");
        if (d != std::string::npos) out += " (" + info.substr(d + 6, info.find('\n', d) - d - 6) + ")";
        return out;
    }
    return std::string(dp.driverName) + (info.empty() ? "" : " " + info.substr(0, info.find('\n')));
}

static void create_device() {
    load_vulkan();
    uint32_t apiVersion = VK_API_VERSION_1_0;
    vkEnumerateInstanceVersion(&apiVersion);
    if (apiVersion < VK_API_VERSION_1_1) fatal("Vulkan 1.1 is required (device has %u.%u)", VK_API_VERSION_MAJOR(apiVersion),
                                               VK_API_VERSION_MINOR(apiVersion));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Wind Waker HD recompiled";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExts;
    // debug: WWHD_VK_VALIDATION=1 enables the Khronos validation layer (if installed with the app)
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("WWHD_VK_VALIDATION")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ici, nullptr, &R.instance) != VK_SUCCESS) {
        ici.enabledLayerCount = 0;
        VK_CHECK(vkCreateInstance(&ici, nullptr, &R.instance));
    }
    volkLoadInstance(R.instance);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(R.instance, &n, nullptr);
    if (!n) fatal("no Vulkan device");
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(R.instance, &n, pds.data());
    R.pd = pds[0];
    vkGetPhysicalDeviceProperties(R.pd, &R.props);
    // which driver actually runs (an installed one may have fallen back to the system's)
    R.driverInfo = R.props.deviceName;
    {
        uint32_t m = 0;
        vkEnumerateDeviceExtensionProperties(R.pd, nullptr, &m, nullptr);
        std::vector<VkExtensionProperties> e(m);
        vkEnumerateDeviceExtensionProperties(R.pd, nullptr, &m, e.data());
        if (R.props.apiVersion >= VK_API_VERSION_1_2 || has_ext(e, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            p2.pNext = &dp;
            vkGetPhysicalDeviceProperties2(R.pd, &p2);
            R.driverInfo = describe_driver(dp);
            R.driverID = dp.driverID;
        }
        if (!g_driver_file.empty() && !g_driver_fallback && !driver_file_mapped()) g_driver_fallback = true;
        if (g_driver_fallback) LOG("[vk] the installed GPU driver could not be loaded; the system driver runs");
        LOG("[vk] driver: %s (Vulkan %u.%u.%u)", R.driverInfo.c_str(), VK_API_VERSION_MAJOR(R.props.apiVersion),
            VK_API_VERSION_MINOR(R.props.apiVersion), VK_API_VERSION_PATCH(R.props.apiVersion));
    }

    vkGetPhysicalDeviceQueueFamilyProperties(R.pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(R.pd, &n, qf.data());
    R.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            R.queueFamily = i;
            break;
        }
    if (R.queueFamily == UINT32_MAX) fatal("no Vulkan graphics queue");

    VkPhysicalDeviceFeatures avail{};
    vkGetPhysicalDeviceFeatures(R.pd, &avail);
    VkPhysicalDeviceFeatures& en = R.features;
    en.textureCompressionBC = avail.textureCompressionBC;
    en.samplerAnisotropy = avail.samplerAnisotropy;
    en.depthClamp = avail.depthClamp;
    en.depthBiasClamp = avail.depthBiasClamp;
    en.imageCubeArray = avail.imageCubeArray;
    en.independentBlend = avail.independentBlend;
    en.dualSrcBlend = avail.dualSrcBlend;
    en.largePoints = avail.largePoints;
    en.shaderClipDistance = avail.shaderClipDistance;
    en.fragmentStoresAndAtomics = avail.fragmentStoresAndAtomics;
    en.vertexPipelineStoresAndAtomics = avail.vertexPipelineStoresAndAtomics;
    en.shaderStorageImageExtendedFormats = avail.shaderStorageImageExtendedFormats;  // r8 for frame generation

    vkEnumerateDeviceExtensionProperties(R.pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(R.pd, nullptr, &n, exts.data());
    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (has_ext(exts, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) {
        devExts.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        R.mirrorClampToEdge = true;
    }

    // frame generation runs 16-bit float shaders where the GPU supports them
    VkPhysicalDeviceShaderFloat16Int8Features f16{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
    if (has_ext(exts, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f16;
        vkGetPhysicalDeviceFeatures2(R.pd, &f2);
        f16.pNext = nullptr;
        f16.shaderInt8 = VK_FALSE;
        if (f16.shaderFloat16) {
            devExts.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
            R.shaderFloat16 = true;
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = R.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)devExts.size();
    di.ppEnabledExtensionNames = devExts.data();
    di.pEnabledFeatures = &en;
    if (R.shaderFloat16) di.pNext = &f16;
    VK_CHECK(vkCreateDevice(R.pd, &di, nullptr, &R.device));
    volkLoadDevice(R.device);  // direct entry points of the one device
    vkGetDeviceQueue(R.device, R.queueFamily, 0, &R.queue);

    VmaVulkanFunctions vf{};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo vi{};
    vi.pVulkanFunctions = &vf;
    vi.vulkanApiVersion = VK_API_VERSION_1_1;
    vi.physicalDevice = R.pd;
    vi.device = R.device;
    vi.instance = R.instance;
    VK_CHECK(vmaCreateAllocator(&vi, &R.vma));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = R.queueFamily;
    VK_CHECK(vkCreateCommandPool(R.device, &pi, nullptr, &R.cmdPool));
    LOG("[vk] command recording: %s", rec::enabled() ? "GX2 record thread" : "direct (WWHD_RECORD_THREAD=0)");

    formats_init(R.pd);
    load_pipeline_cache();
    // an installed driver: note its pipeline cache in its folder, so removing the driver removes it too (GpuDrivers.java)
    if (!g_driver_file.empty() && !g_driver_fallback) {
        std::string note = g_driver_file.substr(0, g_driver_file.rfind('/') + 1) + ".pipeline_cache";
        if (FILE* f = fopen(note.c_str(), "w")) {
            fputs(pipeline_cache_path().c_str(), f);
            fclose(f);
        }
    }
    LOG("[vk] device: %s (Vulkan %u.%u.%u, driver %08X); BC %s, aniso %s, depth clamp %s, cube arrays %s, mirror-clamp %s",
        R.props.deviceName, VK_API_VERSION_MAJOR(R.props.apiVersion), VK_API_VERSION_MINOR(R.props.apiVersion),
        VK_API_VERSION_PATCH(R.props.apiVersion), R.props.driverVersion, en.textureCompressionBC ? "yes" : "no (unpacked)",
        en.samplerAnisotropy ? "yes" : "no", en.depthClamp ? "yes" : "no", en.imageCubeArray ? "yes" : "no",
        R.mirrorClampToEdge ? "yes" : "no");
}

// ---------------------------------------------------------------- presentation
namespace {
const char* kPresentVS = R"(#version 450
layout(push_constant) uniform PC { vec4 rect; vec4 opts; } pc;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // triangle strip corners
    uv = p * 0.5;
    vec2 ndc = pc.rect.xy + p * 0.5 * pc.rect.zw;                    // 0..1 within the window
    gl_Position = vec4(ndc * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kPresentFS = R"(#version 450
layout(push_constant) uniform PC { vec4 rect; vec4 opts; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec3 c = clamp(texture(tex, uv).rgb, 0.0, 1.0);
    // opts.x: the scan buffer is sRGB, i.e. the display encodes (the window surface is UNORM)
    if (pc.opts.x > 0.5) c = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
    color = vec4(c, 1.0);
}
)";

struct Swapchain {
    ANativeWindow* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkSemaphore> renderDone;  // per image
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool stale = false;  // out of date: recreate before the next present
};
Swapchain g_sc;
Swapchain g_sc_drc;  // the GamePad picture on a second display (dual-screen devices), if the app gives one
std::mutex g_window_mutex;  // g_sc, g_sc_drc and the layout; held by the render thread while presenting
ScreenRect g_tv_rect, g_drc_rect;
bool g_drc_visible = false;
std::vector<VkSemaphore> g_acquire_free;
VkDescriptorSetLayout g_present_dsl = VK_NULL_HANDLE;
VkPipelineLayout g_present_layout = VK_NULL_HANDLE;
VkShaderModule g_present_vs = VK_NULL_HANDLE, g_present_fs = VK_NULL_HANDLE;

VkShaderModule make_module(const char* src, bool vertex) {
    std::vector<uint32_t> spv;
    std::string log;
    if (!compile_glsl(src, vertex, spv, log)) fatal("present shader: %s", log.c_str());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, nullptr, &m));
    return m;
}

void create_present_objects() {
    g_present_vs = make_module(kPresentVS, true);
    g_present_fs = make_module(kPresentFS, false);
    VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 1;
    dl.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_present_dsl));
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_present_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    VK_CHECK(vkCreatePipelineLayout(R.device, &pl, nullptr, &g_present_layout));
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.25f;
    VK_CHECK(vkCreateSampler(R.device, &si, nullptr, &R.linearClamp));
}

VkPipeline create_present_pipeline(VkRenderPass pass) {
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = g_present_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = g_present_fs;
    st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &ds;
    pi.layout = g_present_layout;
    pi.renderPass = pass;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(R.device, R.pipelineCache, 1, &pi, nullptr, &p));
    return p;
}

// g_window_mutex and the queue idle
void destroy_swapchain(Swapchain& sc, bool keepSurface) {
    for (auto fb : sc.fbs) vkDestroyFramebuffer(R.device, fb, nullptr);
    for (auto v : sc.views) vkDestroyImageView(R.device, v, nullptr);
    for (auto s : sc.renderDone) vkDestroySemaphore(R.device, s, nullptr);
    sc.fbs.clear();
    sc.views.clear();
    sc.renderDone.clear();
    sc.images.clear();
    if (sc.sc) vkDestroySwapchainKHR(R.device, sc.sc, nullptr);
    sc.sc = VK_NULL_HANDLE;
    if (!keepSurface && sc.surface) {
        vkDestroySurfaceKHR(R.instance, sc.surface, nullptr);
        sc.surface = VK_NULL_HANDLE;
    }
}

// g_window_mutex held; true if a swapchain is ready. `main`: the game window (frame generation
static bool no_60hz_request() {
    static const bool off = getenv("WWHD_NO_60HZ") != nullptr;
    return off;
}

// presents there); else the GamePad's own display
bool ensure_swapchain(Swapchain& sc, bool main = true) {
    if (!sc.window) return false;
    if (sc.sc && !sc.stale) return true;
    if (sc.sc) {
        if (rec::enabled()) wait_record_idle(false);
        std::lock_guard<std::mutex> lk(R.queueMutex);
        vkQueueWaitIdle(R.queue);
        destroy_swapchain(sc, true);
    }
    if (!sc.surface) {
        VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        si.window = sc.window;
        if (vkCreateAndroidSurfaceKHR(R.instance, &si, nullptr, &sc.surface) != VK_SUCCESS) return false;
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(R.pd, R.queueFamily, sc.surface, &ok);
        if (!ok) LOG("[vk] warning: queue family %u reports no present support", R.queueFamily);
    }
    // frame generation presents at a steady multiple of the game's 30 fps: ask for a display mode
    // that is a multiple of it (a 144 Hz panel can't space 60 or 120 fps evenly; 120 Hz can)
    if (main && fg::loaded()) ANativeWindow_setFrameRate(sc.window, 30.0f * fg::config().multiplier,
                                                 ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
    // without it the game flips at most 60 times a second: let a 90/120 Hz panel drop to 60 Hz
    // (from pull request #8 by SSunnKing, 62a723b); WWHD_NO_60HZ=1 leaves the panel's rate alone
    else if (main && !no_60hz_request()) ANativeWindow_setFrameRate(sc.window, 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(R.pd, sc.surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.pd, sc.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(R.pd, sc.surface, &n, fmts.data());
    if (fmts.empty()) return false;
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts)
        if ((f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            break;
        }
    // the window's size in its current orientation; the compositor applies any rotation
    VkExtent2D ext{(uint32_t)ANativeWindow_getWidth(sc.window), (uint32_t)ANativeWindow_getHeight(sc.window)};
    if (!ext.width || !ext.height) return false;
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.pd, sc.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(R.pd, sc.surface, &n, modes.data());
    // the game paces itself (GX2 flips are timed in gx2_core.cpp): don't let presentation block it.
    // With frame generation the present thread paces the frames and each one must be shown: FIFO.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR && !(main && fg::loaded())) mode = m;

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = sc.surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                        : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;  // Android often offers only INHERIT
    for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                                          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
        if (caps.supportedCompositeAlpha & a) {
            ci.compositeAlpha = a;
            break;
        }
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(R.device, &ci, nullptr, &sc.sc) != VK_SUCCESS) {
        sc.sc = VK_NULL_HANDLE;
        return false;
    }
    if (sc.format != fmt.format) {
        if (sc.pass) vkDestroyRenderPass(R.device, sc.pass, nullptr);
        if (sc.pipeline) vkDestroyPipeline(R.device, sc.pipeline, nullptr);
        VkAttachmentDescription a{};
        a.format = fmt.format;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sp;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &sc.pass));
        sc.pipeline = create_present_pipeline(sc.pass);
        sc.format = fmt.format;
    }
    sc.extent = ext;
    vkGetSwapchainImagesKHR(R.device, sc.sc, &n, nullptr);
    sc.images.resize(n);
    vkGetSwapchainImagesKHR(R.device, sc.sc, &n, sc.images.data());
    for (VkImage img : sc.images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(R.device, &vi, nullptr, &v));
        sc.views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = sc.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        VkFramebuffer fb;
        VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &fb));
        sc.fbs.push_back(fb);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        VK_CHECK(vkCreateSemaphore(R.device, &si, nullptr, &s));
        sc.renderDone.push_back(s);
    }
    sc.stale = false;
    LOG("[vk] %sswapchain %ux%u, %u images, format %d, %s, display transform %d", main ? "" : "GamePad display ", ext.width, ext.height, n, (int)fmt.format,
        mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo", (int)caps.currentTransform);
    return true;
}

// letterboxed rect for an image of aspect `a` in the given pixel rect, in 0..1 units of a window of `ext`
// how the TV picture fills its rect (the app's setting): 0 keeps its aspect ratio with bars,
// 1 stretches it over the rect, 2 fills the rect keeping the aspect ratio (the overflow is cut)
std::atomic<int> g_tv_aspect{0};

// rect for an image of aspect `a` in the given pixel rect, in 0..1 units of a window of `ext`
void fit(const ScreenRect& r, float a, float out[4], VkExtent2D ext, int mode = 0) {
    float W = (float)ext.width, H = (float)ext.height;
    float x = r.x, y = r.y, w = r.w, h = r.h;
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = W; h = H; }
    bool wider = w / h > a;
    if (mode == 0 ? wider : mode == 2 ? !wider : false) { x += (w - h * a) / 2; w = h * a; }
    else if (mode != 1) { y += (h - w / a) / 2; h = w / a; }
    out[0] = x / W; out[1] = y / H; out[2] = w / W; out[3] = h / H;
}

// the shape of the TV picture's rect on screen: the aspect the game renders at in "screen" mode (aspect.cpp)
void report_tv_shape(const ScreenRect& r, VkExtent2D ext) {
    if (r.w < 0) return;  // the TV picture is hidden (GamePad on demand): its shape stays
    float w = r.w > 0 ? r.w : (float)ext.width, h = r.h > 0 ? r.h : (float)ext.height;
    if (h > 0) aspect::set_window_aspect(w / h);
}

void draw_screen(VkCommandBuffer cmd, Screen& scr, const ScreenRect& r, VkExtent2D ext, int mode = 0) {
    if (!scr.img.image) return;
    float pc[8];
    fit(r, (float)scr.img.width / scr.img.height, pc, ext, mode);
    if (mode == 2) {  // the picture is larger than its rect: cut at the rect
        VkRect2D sc{{0, 0}, ext};
        if (r.w > 0 && r.h > 0) {
            int32_t x0 = std::max(0, (int32_t)r.x), y0 = std::max(0, (int32_t)r.y);
            int32_t x1 = std::min((int32_t)ext.width, (int32_t)(r.x + r.w)), y1 = std::min((int32_t)ext.height, (int32_t)(r.y + r.h));
            sc = {{x0, y0}, {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)}};
        }
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    pc[4] = scr.srgb ? 1.0f : 0.0f;
    pc[5] = pc[6] = pc[7] = 0;
    VkDescriptorSet set = alloc_descriptor_set(g_present_dsl);
    if (!scr.view) scr.view = make_view(scr.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkDescriptorImageInfo ii{R.linearClamp, scr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(R.device, 1, &w, 0, nullptr);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, g_present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    if (mode == 2) {
        VkRect2D full{{0, 0}, ext};
        vkCmdSetScissor(cmd, 0, 1, &full);
    }
}

// performance overlay: frames presented (game frames, or with frame generation all presented frames)
std::atomic<uint64_t> g_presents{0};
void count_present() { g_presents.fetch_add(1, std::memory_order_relaxed); }
std::atomic<bool> g_present_outdated{false}, g_drc_present_outdated{false};

// record the window image and submit the frame; false if there is no window to present to
bool present_frame() {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (g_present_outdated.exchange(false, std::memory_order_acquire)) g_sc.stale = true;
    if (!ensure_swapchain(g_sc)) return false;
    VkSemaphore acquire;
    if (!g_acquire_free.empty()) {
        acquire = g_acquire_free.back();
        g_acquire_free.pop_back();
    } else {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(R.device, &si, nullptr, &acquire));
    }
    uint32_t idx = 0;
    // a short timeout: a window that isn't being composited must not stall the game
    VkResult r = vkAcquireNextImageKHR(R.device, g_sc.sc, 100 * 1000000ull, acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT || r == VK_NOT_READY || r < 0) {
        if (r != VK_TIMEOUT && r != VK_NOT_READY) g_sc.stale = true;
        g_acquire_free.push_back(acquire);  // unsignaled
        return false;
    }
    // SUBOPTIMAL is normal on Android when the compositor rotates the image (identity pre-transform):
    // recreate only when the window size changed
    if (r == VK_SUBOPTIMAL_KHR && ((uint32_t)ANativeWindow_getWidth(g_sc.window) != g_sc.extent.width ||
                                   (uint32_t)ANativeWindow_getHeight(g_sc.window) != g_sc.extent.height))
        g_sc.stale = true;
    if (R.tv.img.image) prepare(R.tv.img, Use::SAMPLED);
    if (R.drc.img.image) prepare(R.drc.img, Use::SAMPLED);
    end_pass();
    VkCommandBuffer cmd = command_buffer();
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = g_sc.pass;
    bi.framebuffer = g_sc.fbs[idx];
    bi.renderArea = {{0, 0}, g_sc.extent};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_sc.extent.width, (float)g_sc.extent.height, 0, 1};
    VkRect2D sc{{0, 0}, g_sc.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_sc.pipeline);
    report_tv_shape(g_tv_rect, g_sc.extent);
    if (g_tv_rect.w >= 0) draw_screen(cmd, R.tv, g_tv_rect, g_sc.extent, g_tv_aspect.load(std::memory_order_relaxed));
    if (g_drc_visible) draw_screen(cmd, R.drc, g_drc_rect, g_sc.extent);
    vkCmdEndRenderPass(cmd);
    on_complete([acquire] { g_acquire_free.push_back(acquire); });
    uint64_t submitId = submit(acquire, g_sc.renderDone[idx]);
    if (rec::enabled()) enqueue_record_present(g_sc.renderDone[idx], g_sc.sc, idx, &g_present_outdated, true);
    else {
        wait_record_submitted(submitId);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_sc.renderDone[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_sc.sc;
        pi.pImageIndices = &idx;
        {
            std::lock_guard<std::mutex> lk(R.queueMutex);
            r = vkQueuePresentKHR(R.queue, &pi);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc.stale = true;
        count_present();
    }
    return true;
}

// The GamePad picture on its own display (dual-screen devices): once per game frame, after the main
// window. It must never hold up the game: a short acquire timeout, mailbox where offered, and a
// frame that can't be shown is skipped.
std::vector<VkSemaphore> g_drc_acquire_free;
void present_drc_window() {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (g_drc_present_outdated.exchange(false, std::memory_order_acquire)) g_sc_drc.stale = true;
    if (!g_sc_drc.window || !R.drc.img.image || !ensure_swapchain(g_sc_drc, false)) return;
    VkSemaphore acquire;
    if (!g_drc_acquire_free.empty()) {
        acquire = g_drc_acquire_free.back();
        g_drc_acquire_free.pop_back();
    } else {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(R.device, &si, nullptr, &acquire));
    }
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(R.device, g_sc_drc.sc, 10 * 1000000ull, acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT || r == VK_NOT_READY || r < 0) {
        if (r != VK_TIMEOUT && r != VK_NOT_READY) g_sc_drc.stale = true;
        g_drc_acquire_free.push_back(acquire);
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR && ((uint32_t)ANativeWindow_getWidth(g_sc_drc.window) != g_sc_drc.extent.width ||
                                   (uint32_t)ANativeWindow_getHeight(g_sc_drc.window) != g_sc_drc.extent.height))
        g_sc_drc.stale = true;
    prepare(R.drc.img, Use::SAMPLED);
    end_pass();
    VkCommandBuffer cmd = command_buffer();
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = g_sc_drc.pass;
    bi.framebuffer = g_sc_drc.fbs[idx];
    bi.renderArea = {{0, 0}, g_sc_drc.extent};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_sc_drc.extent.width, (float)g_sc_drc.extent.height, 0, 1};
    VkRect2D scissor{{0, 0}, g_sc_drc.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_sc_drc.pipeline);
    draw_screen(cmd, R.drc, ScreenRect{}, g_sc_drc.extent);  // the whole display, 16:9 with bars
    vkCmdEndRenderPass(cmd);
    on_complete([acquire] { g_drc_acquire_free.push_back(acquire); });
    uint64_t submitId = submit(acquire, g_sc_drc.renderDone[idx]);
    if (rec::enabled()) enqueue_record_present(g_sc_drc.renderDone[idx], g_sc_drc.sc, idx, &g_drc_present_outdated, false);
    else {
        wait_record_submitted(submitId);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &g_sc_drc.renderDone[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &g_sc_drc.sc;
        pi.pImageIndices = &idx;
        {
            std::lock_guard<std::mutex> lk(R.queueMutex);
            r = vkQueuePresentKHR(R.queue, &pi);
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc_drc.stale = true;
    }
}

// ---------------------------------------------------------------- frame generation
// The render thread composes the window image off screen and runs the frame generation network on
// it (lsfg.cpp); a present thread shows the generated frames and the game frame evenly spaced over
// the game's frame interval. This delays the game frame by about one interval.
struct Composite {
    Image img;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};
Composite g_comp;

using Clock = std::chrono::steady_clock;
struct FgJob {
    std::vector<const Image*> images;
    Clock::time_point arrival;
};
std::mutex g_fgq_mutex;
std::condition_variable g_fgq_cv;
std::deque<FgJob> g_fgq;
bool g_fg_busy = false;

// present thread
struct PresentSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire = VK_NULL_HANDLE;
};
VkCommandPool g_fg_cmdpool = VK_NULL_HANDLE;
PresentSlot g_fg_slots[3];
uint32_t g_fg_next = 0;
VkDescriptorPool g_fg_dpool = VK_NULL_HANDLE;
std::unordered_map<VkImageView, VkDescriptorSet> g_fg_sets;

void fg_present_init() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = R.queueFamily;
    VK_CHECK(vkCreateCommandPool(R.device, &pi, nullptr, &g_fg_cmdpool));
    for (auto& s : g_fg_slots) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_fg_cmdpool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(R.device, &ai, &s.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(R.device, &fi, nullptr, &s.fence));
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(R.device, &si, nullptr, &s.acquire));
    }
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 64;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    VK_CHECK(vkCreateDescriptorPool(R.device, &dp, nullptr, &g_fg_dpool));
}

// present thread: show `img` (GENERAL layout, written by compute or as an attachment) in the window
bool fg_present(const Image* img) {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (!ensure_swapchain(g_sc)) return false;
    PresentSlot& ps = g_fg_slots[g_fg_next];
    vkWaitForFences(R.device, 1, &ps.fence, VK_TRUE, UINT64_MAX);
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(R.device, g_sc.sc, 100 * 1000000ull, ps.acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_TIMEOUT || r == VK_NOT_READY || r < 0) {
        if (r != VK_TIMEOUT && r != VK_NOT_READY) g_sc.stale = true;
        return false;
    }
    if (r == VK_SUBOPTIMAL_KHR && ((uint32_t)ANativeWindow_getWidth(g_sc.window) != g_sc.extent.width ||
                                   (uint32_t)ANativeWindow_getHeight(g_sc.window) != g_sc.extent.height))
        g_sc.stale = true;
    g_fg_next = (g_fg_next + 1) % 3;
    vkResetFences(R.device, 1, &ps.fence);
    VkImageView view = fg::view_of(img);
    if (!view) view = g_comp.view;
    VkDescriptorSet& set = g_fg_sets[view];
    if (!set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_fg_dpool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_present_dsl;
        VK_CHECK(vkAllocateDescriptorSets(R.device, &ai, &set));
        VkDescriptorImageInfo ii{R.linearClamp, view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(R.device, 1, &w, 0, nullptr);
    }
    VkCommandBuffer cmd = ps.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    // the image was written by work submitted earlier on this queue
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g_sc.pass;
    rb.framebuffer = g_sc.fbs[idx];
    rb.renderArea = {{0, 0}, g_sc.extent};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)g_sc.extent.width, (float)g_sc.extent.height, 0, 1};
    VkRect2D sc{{0, 0}, g_sc.extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_sc.pipeline);
    float pc[8] = {0, 0, 1, 1, 0, 0, 0, 0};  // the whole window; already display encoded
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_present_layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, g_present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &ps.acquire;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_sc.renderDone[idx];
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_sc.renderDone[idx];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_sc.sc;
    pi.pImageIndices = &idx;
    {
        std::lock_guard<std::mutex> lk(R.queueMutex);
        VK_CHECK(vkQueueSubmit(R.queue, 1, &si, ps.fence));
        r = vkQueuePresentKHR(R.queue, &pi);
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) g_sc.stale = true;
    count_present();
    return true;
}

void fg_present_thread() {
    platform::set_thread_name("wwhd-present");
    platform::set_thread_high_priority();
    double interval = 1.0 / 30;  // the game's frame interval, smoothed
    Clock::time_point last{};
    for (;;) {
        FgJob job;
        {
            std::unique_lock<std::mutex> lk(g_fgq_mutex);
            g_fgq_cv.wait(lk, [] { return !g_fgq.empty(); });
            job = std::move(g_fgq.front());
            g_fgq.pop_front();
            g_fg_busy = true;
        }
        if (last != Clock::time_point{}) {
            double d = std::chrono::duration<double>(job.arrival - last).count();
            if (d > 0.004 && d < 0.2) interval += (d - interval) * 0.1;
        }
        last = job.arrival;
        size_t n = job.images.size();
        for (size_t k = 0; k < n; k++) {
            if (k) std::this_thread::sleep_until(job.arrival + std::chrono::duration<double>(interval * k / n));
            if (fg_present(job.images[k])) {
                // presentation statistics: rate and spacing of the presents
                static Clock::time_point statStart = Clock::now(), prev{};
                static int count = 0, generated = 0;
                static double sum = 0, sumSq = 0;
                Clock::time_point t = Clock::now();
                if (prev != Clock::time_point{}) {
                    double d = std::chrono::duration<double, std::milli>(t - prev).count();
                    sum += d;
                    sumSq += d * d;
                }
                prev = t;
                count++;
                generated += k + 1 < n;
                double el = std::chrono::duration<double>(t - statStart).count();
                if (el >= 10) {
                    double mean = sum / std::max(count - 1, 1);
                    LOG("[fg] presented %.1f fps (%.1f generated), interval %.1f ms +- %.1f", count / el, generated / el, mean,
                        std::sqrt(std::max(0.0, sumSq / std::max(count - 1, 1) - mean * mean)));
                    statStart = t;
                    count = generated = 0;
                    sum = sumSq = 0;
                }
            }
        }
        std::lock_guard<std::mutex> lk(g_fgq_mutex);
        g_fg_busy = false;
        g_fgq_cv.notify_all();
    }
}

// wait until the present thread has taken every job (and with `idle`, finished them)
void fg_wait_present(bool idle) {
    std::unique_lock<std::mutex> lk(g_fgq_mutex);
    g_fgq_cv.wait_for(lk, std::chrono::milliseconds(idle ? 2000 : 250),
                      [idle] { return g_fgq.empty() && !(idle && g_fg_busy); });
}

void destroy_composite() {
    if (g_comp.fb) vkDestroyFramebuffer(R.device, g_comp.fb, nullptr);
    if (g_comp.view) vkDestroyImageView(R.device, g_comp.view, nullptr);
    if (g_comp.img.image) vmaDestroyImage(R.vma, g_comp.img.image, g_comp.img.alloc);
    g_comp.fb = VK_NULL_HANDLE;
    g_comp.view = VK_NULL_HANDLE;
    g_comp.img = Image{};
}

bool create_composite(uint32_t w, uint32_t h) {
    if (!g_comp.pass) {
        VkAttachmentDescription a{};
        a.format = VK_FORMAT_R8G8B8A8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // read by compute and the present thread
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1;
        sp.pColorAttachments = &ref;
        // earlier reads of the image (the network's copy) before it's overwritten
        VkSubpassDependency dep{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0};
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1;
        rp.pAttachments = &a;
        rp.subpassCount = 1;
        rp.pSubpasses = &sp;
        rp.dependencyCount = 1;
        rp.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &g_comp.pass));
        g_comp.pipeline = create_present_pipeline(g_comp.pass);
    }
    if (!create_image(g_comp.img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false, false, false))
        return false;
    g_comp.view = make_view(g_comp.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g_comp.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &g_comp.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &g_comp.fb));
    return true;
}

// ---- frame generation settings changed in the app: applied at the next frame boundary
struct FgRequest {
    bool on = false;
    fg::Config cfg;
};
std::mutex g_fg_req_mutex;
bool g_fg_req_pending = false;
FgRequest g_fg_req;

void apply_frame_generation() {
    FgRequest r;
    {
        std::lock_guard<std::mutex> lk(g_fg_req_mutex);
        if (!g_fg_req_pending) return;
        r = g_fg_req;
        g_fg_req_pending = false;
    }
    if (fg::loaded()) {
        // the present thread finishes what it has; then nothing uses the network's images
        wait_idle();
        fg_wait_present(true);
        {
            std::lock_guard<std::mutex> lk(R.queueMutex);
            vkQueueWaitIdle(R.queue);
        }
        if (!rec::enabled()) poll();
        {
            std::lock_guard<std::mutex> lk(g_fgq_mutex);
            g_fg_sets.clear();
            if (g_fg_dpool) vkResetDescriptorPool(R.device, g_fg_dpool, 0);
        }
        destroy_composite();
        fg::unload();
    }
    if (r.on) fg::load(r.cfg);  // logs why if the DLL can't be used
    // present mode (FIFO with frame generation) and the display rate hint differ: new swapchain
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (g_sc.window && !fg::loaded())
        ANativeWindow_setFrameRate(g_sc.window, no_60hz_request() ? 0.0f : 60.0f, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
    g_sc.stale = true;
}

// swap with frame generation; false if there is no window
bool present_frame_fg() {
    static std::once_flag once;
    std::call_once(once, [] {
        fg_present_init();
        std::thread(fg_present_thread).detach();
    });
    uint32_t w, h;
    {
        std::lock_guard<std::mutex> wl(g_window_mutex);
        if (!g_sc.window) return false;
        w = (uint32_t)ANativeWindow_getWidth(g_sc.window);
        h = (uint32_t)ANativeWindow_getHeight(g_sc.window);
    }
    if (!w || !h) return false;
    if (w != g_comp.img.width || h != g_comp.img.height) {
        wait_idle();
        fg_wait_present(true);
        {
            std::lock_guard<std::mutex> lk(R.queueMutex);
            vkQueueWaitIdle(R.queue);
        }
        if (!rec::enabled()) poll();
        std::lock_guard<std::mutex> lk(g_fgq_mutex);  // the present thread is idle
        g_fg_sets.clear();
        vkResetDescriptorPool(R.device, g_fg_dpool, 0);
        destroy_composite();
        if (!create_composite(w, h)) return false;
        fg::resize(w, h, g_comp.view);
    }
    // the game's frame interval decides if generated frames fit the display's refresh rate
    static const double hz = getenv("WWHD_DISPLAY_HZ") ? atof(getenv("WWHD_DISPLAY_HZ")) : 60.0;
    static Clock::time_point last{};
    static double interval = 1.0 / 30;
    Clock::time_point now = Clock::now();
    if (last != Clock::time_point{}) {
        double d = std::chrono::duration<double>(now - last).count();
        if (d > 0.004 && d < 0.2) interval += (d - interval) * 0.1;
    }
    last = now;
    // fast forward (mods/turbo.cpp): game frames only, the game's own frame rate is the point
    bool generate = fg::config().multiplier / interval <= hz * 1.1 && !g_fast_forward.load(std::memory_order_relaxed);

    if (R.tv.img.image) prepare(R.tv.img, Use::SAMPLED);
    if (R.drc.img.image) prepare(R.drc.img, Use::SAMPLED);
    end_pass();
    VkCommandBuffer cmd = command_buffer();
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = g_comp.pass;
    bi.framebuffer = g_comp.fb;
    bi.renderArea = {{0, 0}, {w, h}};
    bi.clearValueCount = 1;
    bi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_comp.pipeline);
    ScreenRect tvRect, drcRect;
    bool drcVisible;
    {
        std::lock_guard<std::mutex> wl(g_window_mutex);
        tvRect = g_tv_rect;
        drcRect = g_drc_rect;
        drcVisible = g_drc_visible;
    }
    report_tv_shape(tvRect, {w, h});
    if (tvRect.w >= 0) draw_screen(cmd, R.tv, tvRect, {w, h}, g_tv_aspect.load(std::memory_order_relaxed));
    if (drcVisible) draw_screen(cmd, R.drc, drcRect, {w, h});
    vkCmdEndRenderPass(cmd);
    FgJob job;
    job.images = fg::record(cmd, generate);
    uint64_t submitId = submit();
    wait_record_submitted(submitId);  // the present thread also submits to R.queue
    job.arrival = Clock::now();
    fg_wait_present(false);  // at most one frame waiting: the images are reused two frames later
    {
        std::lock_guard<std::mutex> lk(g_fgq_mutex);
        g_fgq.push_back(std::move(job));
    }
    g_fgq_cv.notify_all();
    return true;
}
}  // namespace

void set_fast_forward(bool on) { g_fast_forward.store(on, std::memory_order_relaxed); }

void set_window(ANativeWindow* w) {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (w == g_sc.window) {
        g_sc.stale = true;  // same window, new size
        return;
    }
    if (g_sc.sc || g_sc.surface) {
        if (rec::enabled()) wait_record_idle(false);
        std::lock_guard<std::mutex> lk(R.queueMutex);
        vkQueueWaitIdle(R.queue);
        destroy_swapchain(g_sc, false);
    }
    if (g_sc.window) ANativeWindow_release(g_sc.window);
    g_sc.window = w;
    if (w) ANativeWindow_acquire(w);
}

void set_drc_window(ANativeWindow* w) {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    if (w == g_sc_drc.window) {
        g_sc_drc.stale = true;  // same window, new size
        return;
    }
    if (g_sc_drc.sc || g_sc_drc.surface) {
        if (rec::enabled()) wait_record_idle(false);
        std::lock_guard<std::mutex> lk(R.queueMutex);
        vkQueueWaitIdle(R.queue);
        destroy_swapchain(g_sc_drc, false);
    }
    if (g_sc_drc.window) ANativeWindow_release(g_sc_drc.window);
    g_sc_drc.window = w;
    if (w) ANativeWindow_acquire(w);
    LOG("[vk] GamePad display %s", w ? "attached" : "detached");
}

void request_frame_generation(bool on, const char* dll, bool quality, float flowScale, int multiplier, bool uiDetection) {
    std::lock_guard<std::mutex> lk(g_fg_req_mutex);
    g_fg_req.on = on && dll && *dll;
    g_fg_req.cfg.dll = dll ? dll : "";
    g_fg_req.cfg.performance = !quality;
    g_fg_req.cfg.flowScale = flowScale;
    g_fg_req.cfg.multiplier = multiplier;
    g_fg_req.cfg.uiDetection = uiDetection;
    g_fg_req_pending = true;
}

void set_tv_aspect(int mode) { g_tv_aspect = std::clamp(mode, 0, 2); }
int tv_aspect() { return g_tv_aspect.load(); }

void set_layout(ScreenRect tv, ScreenRect drc, bool drcVisible) {
    std::lock_guard<std::mutex> wl(g_window_mutex);
    g_tv_rect = tv;
    g_drc_rect = drc;
    g_drc_visible = drcVisible;
}

// ---------------------------------------------------------------- init
void init() {
    shader_compiler_init();
    create_device();
    create_present_objects();
    if (const char* dll = getenv("WWHD_LSFG_DLL"); dll && *dll) {
        fg::Config c;
        c.dll = dll;
        if (const char* e = getenv("WWHD_LSFG_QUALITY")) c.performance = !atoi(e);
        if (const char* e = getenv("WWHD_LSFG_FLOW_SCALE")) c.flowScale = (float)atof(e);
        if (const char* e = getenv("WWHD_LSFG_MULTIPLIER")) c.multiplier = atoi(e);
        if (const char* e = getenv("WWHD_LSFG_UI_DETECTION")) c.uiDetection = atoi(e) != 0;
        fg::load(c);
    }
}

void run_main_loop() {}  // Android: the activity owns the main thread

void with_autorelease_pool(void (*fn)()) { fn(); }

// ---------------------------------------------------------------- clears
static void clear_surface(Surface* s, const float* rgba, bool clearDepth, float depth, bool clearStencil, uint32_t stencil,
                          uint32_t firstSlice = 0, uint32_t numSlices = 1) {
    if (!s || !s->img.image) return;
    prepare(s->img, Use::COPY_DST);
    VkCommandBuffer cmd = command_buffer();
    uint32_t n = std::min(numSlices, s->img.layers - std::min(firstSlice, s->img.layers - 1));
    if (s->isDepth) {
        VkImageAspectFlags aspect = (clearDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                    (clearStencil && s->fmt.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (aspect) {
            VkClearDepthStencilValue v{depth, stencil};
            VkImageSubresourceRange range{aspect, 0, 1, firstSlice, n};
            vkCmdClearDepthStencilImage(cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        }
    } else {
        VkClearColorValue v{};
        if (s->fmt.kind == FormatInfo::FLOAT) memcpy(v.float32, rgba, 16);
        else for (int i = 0; i < 4; i++) v.uint32[i] = (uint32_t)rgba[i];
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, firstSlice, n};
        vkCmdClearColorImage(cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
    }
    mark_gpu_written(s);
}

void clear_color(const uint32_t* regs, uint32_t cb, const float rgba[4]) {
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_color_buffer(cb, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] color %08X %ux%u fmt %X -> %.3f %.3f %.3f %.3f", s->addr, s->width, s->height, s->format, rgba[0], rgba[1], rgba[2], rgba[3]);
    clear_surface(s, rgba, false, 0, false, 0, first, num);
}

void clear_depth_stencil(const uint32_t* regs, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
    // flags: 1 = depth, 2 = stencil
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_depth_buffer(db, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] depth %08X %ux%ux%u fmt %X vk %d -> depth %.4f stencil %u flags %u slices %u+%u", s->addr, s->width, s->height,
            s->slices, s->format, (int)s->fmt.format, depth, stencil, flags, first, num);
    clear_surface(s, nullptr, flags & 1, depth, (flags & 2) != 0, stencil, first, num);
}

// ---------------------------------------------------------------- copies
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice) {
    void copy_surface_impl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    copy_surface_impl(src, srcMip, srcSlice, dst, dstMip, dstSlice);
}

void copy_to_scan(uint32_t cb, uint32_t target) {
    // target: 1 = TV, 4/8 = GamePad
    if (log_this_frame()) LOG("[scan] copy %08X to %s", cb, (target & 1) ? "TV" : "DRC");
    Screen& scr = (target & 1) ? R.tv : R.drc;
    Surface* s = surface_from_color_buffer(cb);
    if (!s || !s->img.image) return;
    if (!scr.img.image || scr.img.width != s->img.width || scr.img.height != s->img.height || scr.img.format != s->img.format) {
        if (scr.img.image) {
            // the old image may still be in use by queued presents
            Image old = scr.img;
            VkImageView oldView = scr.view;
            on_complete([old, oldView] {
                if (oldView) vkDestroyImageView(R.device, oldView, nullptr);
                vmaDestroyImage(R.vma, old.image, old.alloc);
            });
        }
        scr.img = Image{};
        scr.view = VK_NULL_HANDLE;
        if (!create_image(scr.img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, s->img.format, s->img.width, s->img.height, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false,
                          false, false))
            return;
    }
    prepare(s->img, Use::COPY_SRC);
    prepare(scr.img, Use::COPY_DST);
    VkImageCopy c{};
    c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {s->img.width, s->img.height, 1};
    vkCmdCopyImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scr.img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void set_tv_format(uint32_t gx2Format, bool tv) {
    (tv ? R.tv : R.drc).srgb = (gx2Format & 0x400) != 0;
}

// ---------------------------------------------------------------- debug image dumps (PNG)
static void png_chunk(FILE* f, const char* type, const uint8_t* data, uint32_t len) {
    uint8_t be[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    uLong crc = crc32(0, (const Bytef*)type, 4);
    if (len) crc = crc32(crc, data, len);
    uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(c, 1, 4, f);
}

static bool write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw((size_t)(w * 4 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw[(size_t)y * (w * 4 + 1)] = 0;
        memcpy(&raw[(size_t)y * (w * 4 + 1) + 1], rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uLongf clen = compressBound(raw.size());
    std::vector<uint8_t> comp(clen);
    if (compress2(comp.data(), &clen, raw.data(), raw.size(), 6) != Z_OK) return false;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", comp.data(), (uint32_t)clen);
    png_chunk(f, "IEND", nullptr, 0);
    fclose(f);
    return true;
}

static float half_to_float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v = e ? ldexpf(1.0f + m / 1024.0f, (int)e - 15) : ldexpf(m / 1024.0f, -14);
    return s ? -v : v;
}

// Depth peeks (peekz.cpp): the depth of the main TV depth buffer at a few points of the 640x480
// game screen, as the 24-bit value the game compares (0xFFFFFF = nothing drawn there). One pixel
// per point is copied to a host buffer here, in command order (after the scene the game peeks
// at); the answers are written to guest memory when the GPU has finished.
void peek_z(const uint32_t* cells, uint32_t n) {
    Surface* d = nullptr;
    if (R.mainDepthAddr) {
        auto range = R.surfaces.equal_range(R.mainDepthAddr);
        for (auto it = range.first; it != range.second && !d; ++it)
            if (it->second->isDepth && it->second->width == 1280 && it->second->height == 720 && it->second->img.image)
                d = it->second.get();
    }
    const uint32_t points = n / 3;
    if (!d || !points) return;
    Image& img = d->img;
    const VkFormat f = img.format;
    if (f != VK_FORMAT_D32_SFLOAT && f != VK_FORMAT_D32_SFLOAT_S8_UINT && f != VK_FORMAT_D24_UNORM_S8_UINT &&
        f != VK_FORMAT_X8_D24_UNORM_PACK32 && f != VK_FORMAT_D16_UNORM)
        return;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)points * 4;  // 4 bytes per point keeps every depth copy's offset aligned
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf;
    VmaAllocation alloc;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) return;
    std::vector<VkBufferImageCopy> regions(points);
    std::vector<uint32_t> dst(points), px(points * 2);
    for (uint32_t i = 0; i < points; i++) {
        // 640x480 game screen -> image pixels (the image includes the resolution scale and the
        // widescreen factor; the game projects into the whole picture)
        float x = (float)(int32_t)cells[i * 3] * img.width / 640.0f, y = (float)(int32_t)cells[i * 3 + 1] * img.height / 480.0f;
        VkBufferImageCopy& c = regions[i];
        c.bufferOffset = (VkDeviceSize)i * 4;
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageOffset = {std::clamp((int32_t)x, 0, (int32_t)img.width - 1), std::clamp((int32_t)y, 0, (int32_t)img.height - 1), 0};
        c.imageExtent = {1, 1, 1};
        dst[i] = cells[i * 3 + 2];
        px[i * 2] = cells[i * 3];
        px[i * 2 + 1] = cells[i * 3 + 1];
    }
    prepare(img, Use::COPY_SRC);
    vkCmdCopyImageToBuffer(command_buffer(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, points, regions.data());
    static const bool log = getenv("WWHD_PEEKZ_LOG") != nullptr;
    on_complete([=]() {
        vmaInvalidateAllocation(R.vma, alloc, 0, VK_WHOLE_SIZE);
        const uint8_t* raw = (const uint8_t*)info.pMappedData;
        for (uint32_t i = 0; i < points; i++) {
            uint32_t v;
            if (f == VK_FORMAT_D16_UNORM) {
                uint16_t h;
                memcpy(&h, raw + i * 4, 2);
                v = h == 0xFFFF ? 0xFFFFFF : (uint32_t)h << 8;
            } else if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32) {
                memcpy(&v, raw + i * 4, 4);
                v &= 0xFFFFFF;
            } else {
                float z;
                memcpy(&z, raw + i * 4, 4);
                v = z >= 1.0f ? 0xFFFFFF : (uint32_t)(std::clamp(z, 0.0f, 1.0f) * 16777215.0f);
            }
            st32(dst[i], v);
            if (log) LOG("[peekz] point %u (%d, %d): %06X", i, (int32_t)px[i * 2], (int32_t)px[i * 2 + 1], v);
        }
        vmaDestroyBuffer(R.vma, buf, alloc);
    });
}

void dump_texture(Image& src, const char* name, bool async, bool srgbEncode) {
    if (!src.image) return;
    uint32_t w = src.width, h = src.height;
    VkFormat f = src.format;
    bool depth = (src.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    uint32_t layers = depth ? std::max(src.layers, 1u) : 1;  // depth: all array slices side by side
    uint32_t bpp;
    switch (f) {
    case VK_FORMAT_R8_UNORM: bpp = 1; break;
    case VK_FORMAT_D16_UNORM: bpp = 2; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: bpp = 8; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: bpp = 4; break;
    default: LOG("[gfx] dump: unsupported format %d", (int)f); return;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)w * h * bpp * layers;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf;
    VmaAllocation alloc;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) return;
    prepare(src, Use::COPY_SRC);
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t z = 0; z < layers; z++) {
        VkBufferImageCopy c{};
        c.bufferOffset = (VkDeviceSize)z * w * h * bpp;
        c.imageSubresource = {depth ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT,
                              0, z, 1};
        c.imageExtent = {w, h, 1};
        regions.push_back(c);
    }
    vkCmdCopyImageToBuffer(command_buffer(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, (uint32_t)regions.size(),
                           regions.data());
    std::string file = name;
    auto write = [=]() {
        vmaInvalidateAllocation(R.vma, alloc, 0, VK_WHOLE_SIZE);
        const uint8_t* raw = (const uint8_t*)info.pMappedData;
        uint32_t W = w * layers;
        std::vector<uint8_t> px((size_t)W * h * 4);
        if (depth) {
            // grey image, contrast-stretched to the range of values present
            for (uint32_t z = 0; z < layers; z++) {
                std::vector<float> v((size_t)w * h);
                for (size_t i = 0; i < v.size(); i++) {
                    size_t k = (size_t)z * w * h + i;
                    if (f == VK_FORMAT_D16_UNORM) v[i] = ((const uint16_t*)raw)[k] / 65535.0f;
                    else if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32)
                        v[i] = (((const uint32_t*)raw)[k] & 0xFFFFFF) / 16777215.0f;
                    else v[i] = ((const float*)raw)[k];
                }
                float lo = 1, hi = 0;
                for (float x : v) if (x < 1.0f) { lo = std::min(lo, x); hi = std::max(hi, x); }
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        float d = v[(size_t)y * w + x];
                        uint8_t g = d >= 1.0f ? 255 : (uint8_t)std::clamp((d - lo) / std::max(hi - lo, 1e-6f) * 230.0f, 0.0f, 230.0f);
                        uint8_t* o = &px[((size_t)y * W + z * w + x) * 4];
                        o[0] = o[1] = o[2] = g;
                        o[3] = 255;
                    }
            }
        } else {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                uint8_t* o = &px[i * 4];
                uint32_t v = 0;
                if (bpp == 4) memcpy(&v, raw + i * 4, 4);
                if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                    o[0] = (v & 0x3FF) >> 2; o[1] = ((v >> 10) & 0x3FF) >> 2; o[2] = ((v >> 20) & 0x3FF) >> 2;
                } else if (f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB) {
                    memcpy(o, raw + i * 4, 3);
                } else if (f == VK_FORMAT_R8_UNORM) {
                    o[0] = o[1] = o[2] = raw[i];
                } else if (f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
                    auto f11 = [](uint32_t m, uint32_t e) { return e ? ldexpf(1.0f + m / 64.0f, (int)e - 15) : ldexpf(m / 64.0f, -14); };
                    float r = f11(v & 0x3F, (v >> 6) & 0x1F), g = f11((v >> 11) & 0x3F, (v >> 17) & 0x1F),
                          b = ldexpf(1.0f + ((v >> 22) & 0x1F) / 32.0f, (int)((v >> 27) & 0x1F) - 15);
                    o[0] = (uint8_t)std::min(255.0f, r * 255); o[1] = (uint8_t)std::min(255.0f, g * 255); o[2] = (uint8_t)std::min(255.0f, b * 255);
                } else {  // RGBA16F
                    for (int c = 0; c < 3; c++) {
                        uint16_t hv;
                        memcpy(&hv, raw + i * 8 + c * 2, 2);
                        o[c] = (uint8_t)std::clamp(half_to_float(hv) * 255.0f, 0.0f, 255.0f);
                    }
                }
                o[3] = 255;
                if (srgbEncode)
                    for (int c = 0; c < 3; c++) {
                        float x = o[c] / 255.0f;
                        x = x <= 0.0031308f ? x * 12.92f : 1.055f * powf(x, 1 / 2.4f) - 0.055f;
                        o[c] = (uint8_t)std::clamp(x * 255.0f + 0.5f, 0.0f, 255.0f);
                    }
            }
        }
        if (write_png(file.c_str(), px.data(), W, h)) LOG("[gfx] wrote %s (%ux%u)", file.c_str(), W, h);
        vmaDestroyBuffer(R.vma, buf, alloc);
    };
    if (async) {
        on_complete(write);
    } else {
        wait_idle();
        write();
    }
}

// debug: WWHD_DUMP_FRAMES=100,300 writes the TV image of those frames to frame_<n>.png
static std::set<uint64_t> g_dump_frames = [] {
    std::set<uint64_t> f;
    if (const char* e = getenv("WWHD_DUMP_FRAMES"))
        for (const char* p = e; *p;) {
            f.insert(strtoull(p, (char**)&p, 10));
            while (*p == ',') p++;
        }
    return f;
}();

static void dump_tv(uint64_t frame) {
    char name[64];
    snprintf(name, sizeof name, "frame_%llu.png", (unsigned long long)frame);
    dump_texture(R.tv.img, name, true, R.tv.srgb);
    if (R.drc.img.image) {
        snprintf(name, sizeof name, "frame_%llu_drc.png", (unsigned long long)frame);
        dump_texture(R.drc.img, name, true, R.drc.srgb);
    }
}

// ---------------------------------------------------------------- swap
static std::atomic<uint64_t> g_frames_completed{0}, g_frames_submitted{0};
// Frames the game may treat as done (GX2 flips wait for this). On the console a flip waits for the
// GPU so the game can reuse the frame's buffers; this renderer has copied all guest data by the time
// a frame is submitted, so a submitted frame counts, as long as the GPU is at most one frame behind.
// The CPU then builds frame N+1 while the GPU renders frame N (WWHD_STRICT_FLIPS=1: wait for the GPU).
uint64_t frames_completed() {
    static const bool strict = getenv("WWHD_STRICT_FLIPS") != nullptr;
    uint64_t done = g_frames_completed.load();
    return strict ? done : std::min<uint64_t>(g_frames_submitted.load(), done + 1);
}

// The game paces itself on frames finishing on the GPU (flips wait for them). Submissions are only
// polled when the render thread submits again, which can be most of a frame later while the game
// waits, serializing CPU and GPU. Instead each frame ends with an empty submission signalling a
// fence of its own, and a thread waits on those and reports completion right away.
namespace {
std::mutex g_fw_mutex;
std::condition_variable g_fw_cv;
std::deque<VkFence> g_fw_pending;
std::vector<VkFence> g_fw_free;

void frame_waiter_thread() {
    for (;;) {
        VkFence f;
        {
            std::unique_lock<std::mutex> lk(g_fw_mutex);
            g_fw_cv.wait(lk, [] { return !g_fw_pending.empty(); });
            f = g_fw_pending.front();
        }
        vkWaitForFences(R.device, 1, &f, VK_TRUE, UINT64_MAX);
        vkResetFences(R.device, 1, &f);
        g_frames_completed++;
        std::lock_guard<std::mutex> lk(g_fw_mutex);
        g_fw_pending.pop_front();
        g_fw_free.push_back(f);
    }
}

// after the frame's work has been submitted: queue order makes this fence signal once it is done
void signal_frame_end() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(frame_waiter_thread).detach(); });
    VkFence f = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lk(g_fw_mutex);
        if (!g_fw_free.empty()) {
            f = g_fw_free.back();
            g_fw_free.pop_back();
        }
    }
    if (!f) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(R.device, &fi, nullptr, &f));
    }
    {
        std::lock_guard<std::mutex> lk(R.queueMutex);
        VK_CHECK(vkQueueSubmit(R.queue, 0, nullptr, f));
    }
    std::lock_guard<std::mutex> lk(g_fw_mutex);
    g_fw_pending.push_back(f);
    g_fw_cv.notify_one();
}
}  // namespace

void cache_warm_step();
const char* capture_begin_frame();
void report_skips();
size_t pipelines_created();

void descriptor_cache_trim();

// TV image dumps requested ahead of time (save state thumbnails and checks)
static std::mutex g_tv_dump_mu;
static std::vector<std::pair<uint64_t, std::string>> g_tv_dumps;
uint64_t frame_count() { return __atomic_load_n(&R.frame, __ATOMIC_RELAXED); }
void request_tv_dump(const std::string& path, int frames_ahead) {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    g_tv_dumps.push_back({frame_count() + frames_ahead, path});
}
static void service_tv_dumps() {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    for (auto it = g_tv_dumps.begin(); it != g_tv_dumps.end();)
        if (it->first <= R.frame && R.tv.img.image) {
            dump_texture(R.tv.img, it->second.c_str(), true, R.tv.srgb);
            it = g_tv_dumps.erase(it);
        } else {
            ++it;
        }
}

// performance overlay: game frame times over one-second windows, and running totals for the average
static std::mutex g_perf_mu;
static float g_perf[7];
static void perf_frame() {
    using clk = std::chrono::steady_clock;
    static clk::time_point last{}, windowStart{};
    static double sumMs = 0, maxMs = 0;
    static int frames = 0, late = 0;
    static uint64_t presents0 = 0;
    static double totalFrames = 0, totalSec = 0;
    static const bool logIt = getenv("WWHD_PERF_LOG") != nullptr;  // debug: log the numbers every second
    clk::time_point now = clk::now();
    if (last != clk::time_point{}) {
        double ms = std::chrono::duration<double, std::milli>(now - last).count();
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
        if (ms > 40) late++;
        frames++;
        // gaps over a second are the app paused or in the background, not game frames
        if (ms <= 1000) {
            std::lock_guard<std::mutex> lk(g_perf_mu);
            totalFrames++;
            totalSec += ms / 1000;
            g_perf[5] = (float)totalFrames;
            g_perf[6] = (float)totalSec;
        }
    } else {
        windowStart = now;
    }
    last = now;
    double el = std::chrono::duration<double>(now - windowStart).count();
    if (el >= 1.0 && frames) {
        uint64_t p = g_presents.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_perf_mu);
        g_perf[0] = (float)(frames / el);
        g_perf[1] = (float)(sumMs / frames);
        g_perf[2] = (float)maxMs;
        g_perf[3] = (float)((p - presents0) / el);
        g_perf[4] = fg::loaded() ? fg::gpu_ms() : 0.0f;
        if (logIt) LOG("[perf] game %.1f fps, frame %.1f ms, max %.1f, late %d", g_perf[0], g_perf[1], g_perf[2], late);
        late = 0;
        presents0 = p;
        windowStart = now;
        sumMs = maxMs = 0;
        frames = 0;
    }
}
const char* driver_info() { return R.driverInfo.c_str(); }
const char* gpu_name() { return R.device ? R.props.deviceName : ""; }
bool driver_fallback() { return g_driver_fallback; }

void perf_stats(float out[7]) {
    std::lock_guard<std::mutex> lk(g_perf_mu);
    memcpy(out, g_perf, sizeof g_perf);
}

void swap() {
    cache_warm_step();
    perf_frame();
    platform::perf_hint_frame();
    prof_frame();
    latch_resolution_scale();
    apply_frame_generation();
    descriptor_cache_trim();
    static bool sync_gpu = getenv("WWHD_SYNC_GPU") != nullptr;  // debug: no CPU/GPU overlap
    if (sync_gpu) wait_idle();
    end_pass();
    if (log_this_frame()) LOG("[frame] end %llu", (unsigned long long)R.frame);
    R.frame++;
    // an installed GPU driver ran the game for a few seconds: its start marker goes (MainActivity
    // switches back to the system driver if it finds one); not if the system driver runs instead
    if (R.frame == 120 && !g_driver_fallback)
        if (const char* probe = getenv("WWHD_GPU_DRIVER_PROBE"); probe && *probe) remove(probe);
    if (g_dump_frames.count(R.frame)) dump_tv(R.frame);
    service_tv_dumps();
    static std::string pendingCapture;  // the TV image is dumped once the captured frame has been drawn
    if (!pendingCapture.empty()) {
        dump_texture(R.tv.img, (pendingCapture + "/tv.png").c_str(), true, R.tv.srgb);
        if (R.drc.img.image) dump_texture(R.drc.img, (pendingCapture + "/gamepad.png").c_str(), true, R.drc.srgb);
        LOG("[gfx] capture written to %s", pendingCapture.c_str());
        pendingCapture.clear();
    }
    if (const char* dir = capture_begin_frame()) pendingCapture = dir;
    if (!(fg::loaded() ? present_frame_fg() : present_frame())) submit();
    present_drc_window();
    signal_frame_end();
    g_frames_submitted++;
    if (R.frame % 300 == 1) {
        LOG("[gfx] frame %llu, %llu draws so far", (unsigned long long)R.frame, (unsigned long long)R.drawCount);
        if (rec::enabled()) {
            uint64_t ns = g_record_cpu_ns.exchange(0), bytes = g_record_stream_bytes.exchange(0),
                     streams = g_record_streams.exchange(0);
            LOG("[gfx] last 300 frames: GX2 record %.1f ms/frame, stream %.1f KiB/frame, %.1f submits/frame",
                ns / 300.0 / 1e6, bytes / 300.0 / 1024.0, streams / 300.0);
        }
        report_skips();
    }
    // keep the driver's compiled pipelines across launches (the app can be killed at any time)
    static size_t savedPipelines = 0;
    if (R.frame % 1800 == 0 && pipelines_created() != savedPipelines) {
        savedPipelines = pipelines_created();
        save_caches();
    }
}

extern uint64_t g_stat_invalidates, g_stat_invalidated_surfaces;

// CPU-side surfaces sorted by address, for invalidate (the game sends ~200 a frame; going through
// every surface each time cost the render thread several percent). Rebuilt when surfaces are added
// or their data size changes (surfaces_changed).
static std::vector<std::pair<uint32_t, Surface*>> g_inval_index;
static uint32_t g_inval_maxlen = 0;
static bool g_inval_dirty = true;
void surfaces_changed() { g_inval_dirty = true; }
static uint32_t surface_extent(const Surface* s) { return std::max<uint32_t>(s->dataSize, s->pitch * s->height * 4); }

void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    g_stat_invalidates++;
    static int logged = 0;
    static const bool logInval = getenv("WWHD_LOG_INVALIDATE") != nullptr;
    if (logInval && (flags & 0x2) && logged++ < 400)
        LOG("[inval] frame %llu flags %X addr %08X size %X", (unsigned long long)R.frame, flags, addr, size);
    // GX2_INVALIDATE_MODE_TEXTURE (0x2): the CPU wrote texture data; force a full check of surfaces in
    // range on next use. Uniform/attribute/shader invalidations need nothing here (data is copied per draw).
    if (!(flags & 0x2)) return;
    // "invalidate everything" (sent several times per frame) carries no information about CPU writes;
    // changed textures are still caught by the per-frame sparse check and the periodic full check
    if (size >= 0x10000000) return;
    static const bool scan = getenv("WWHD_INVALIDATE_SCAN") != nullptr;  // debug: the old full scan
    if (scan) {
        for (auto& [a, s] : R.surfaces)
            // MEM1 holds render targets; CPU-side surfaces there are views of GPU data, not CPU uploads
            if (!s->gpuWritten && !(a >= 0xF4000000 && a < 0xF6000000) && a < addr + size && addr < a + surface_extent(s.get())) {
                s->lastCheckedFrame = ~0ull;
                s->dirty = true;
                g_stat_invalidated_surfaces++;
            }
        return;
    }
    if (g_inval_dirty) {
        g_inval_dirty = false;
        g_inval_index.clear();
        g_inval_maxlen = 0;
        for (auto& [a, s] : R.surfaces)
            if (!(a >= 0xF4000000 && a < 0xF6000000)) {  // MEM1: render targets, not CPU uploads
                g_inval_index.push_back({a, s.get()});
                g_inval_maxlen = std::max(g_inval_maxlen, surface_extent(s.get()));
            }
        std::sort(g_inval_index.begin(), g_inval_index.end(),
                  [](const std::pair<uint32_t, Surface*>& x, const std::pair<uint32_t, Surface*>& y) { return x.first < y.first; });
    }
    // surfaces starting in [addr - longest, addr + size) can overlap
    uint32_t from = addr > g_inval_maxlen ? addr - g_inval_maxlen : 0;
    auto it = std::lower_bound(g_inval_index.begin(), g_inval_index.end(), from,
                               [](const std::pair<uint32_t, Surface*>& e, uint32_t v) { return e.first < v; });
    for (; it != g_inval_index.end() && it->first < addr + size; ++it) {
        Surface* s = it->second;
        if (!s->gpuWritten && addr < it->first + surface_extent(s)) {
            s->lastCheckedFrame = ~0ull;
            s->dirty = true;
            g_stat_invalidated_surfaces++;
            next_write_seq();  // draws must check their textures again (draw's fast path)
        }
    }
}

}  // namespace gfx
