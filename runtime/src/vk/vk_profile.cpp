// GPU profiling (WWHD_GPU_PROFILE=1): timestamps at the start and end of every command buffer and
// around every render pass of the game. Read back when the GPU has finished the command buffer and
// summed per kind of pass (size, formats); every 150 frames the log shows GPU busy time per frame
// against the frame time, and the passes that cost the most.
// Without it, dynamic resolution (vk_dynres.cpp) still gets the two command buffer timestamps.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime.h"
#include "vk.h"

namespace gfx {
namespace {

constexpr uint32_t kQueries = 2048;

struct Pass {
    uint32_t begin, end;  // query indices
    int label;
    uint32_t draws;
    uint32_t ps;  // pixel shader of its last draw (named in the label of passes with few draws)
    bool op;      // a transfer or compute command outside the render passes (prof_op)
};
struct CmdProf {
    VkQueryPool pool = VK_NULL_HANDLE;
    uint32_t used = 0;
    uint32_t cmdBegin = 0;
    std::vector<Pass> passes;
};

struct Stat {
    double ns = 0;
    uint64_t count = 0, draws = 0;
    bool op = false;
};

bool g_on = false, g_checked = false, g_timestamps = false;
double g_period = 1;  // ns per tick
uint64_t g_mask = ~0ull;
std::vector<VkQueryPool> g_free;
std::shared_ptr<CmdProf> g_cur;
uint64_t g_cur_serial = 0;
int g_open = -1;  // index in g_cur->passes of the open pass
uint64_t g_draws_at_begin = 0;
std::vector<std::string> g_labels;
std::map<std::string, int> g_label_ids;
std::vector<Stat> g_stats;
double g_busy_ns = 0, g_pass_ns = 0, g_op_ns = 0;
uint64_t g_pass_count = 0, g_op_count = 0;
uint64_t g_last_end = 0;  // end of the previous command buffer (ticks)
double g_gap_ns = 0;
uint64_t g_frames = 0;
std::chrono::steady_clock::time_point g_window_start;

bool on() {
    if (!g_checked) {
        g_checked = true;
        g_on = getenv("WWHD_GPU_PROFILE") != nullptr && R.props.limits.timestampPeriod > 0;
        uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(R.pd, &n, nullptr);
        std::vector<VkQueueFamilyProperties> qf(n);
        vkGetPhysicalDeviceQueueFamilyProperties(R.pd, &n, qf.data());
        uint32_t bits = R.queueFamily < n ? qf[R.queueFamily].timestampValidBits : 0;
        if (g_on && !bits) g_on = false;
        g_timestamps = bits && R.props.limits.timestampPeriod > 0;
        g_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        g_period = R.props.limits.timestampPeriod;
        if (getenv("WWHD_GPU_PROFILE"))
            LOG("[gpuprof] %s (timestamp bits %u, period %.2f ns)", g_on ? "on" : "unavailable", bits, g_period);
        g_window_start = std::chrono::steady_clock::now();
    }
    return g_on;
}

// timestamps at the command buffers' start and end: for the profile, dynamic resolution or the benchmark
bool timing() { return on() || (g_timestamps && (dynamic_resolution_on() || benchmark_on())); }
// queries per pool: the profile's passes, or only the two
uint32_t pool_queries() { return g_on ? kQueries : 2; }

const char* fmt_name(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8s";
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "R11G11B10F";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "RGB10A2";
    case VK_FORMAT_R8_UNORM: return "R8";
    case VK_FORMAT_R8G8_UNORM: return "RG8";
    case VK_FORMAT_R16_SFLOAT: return "R16F";
    case VK_FORMAT_R16G16_SFLOAT: return "RG16F";
    case VK_FORMAT_R32_SFLOAT: return "R32F";
    case VK_FORMAT_D16_UNORM: return "D16";
    case VK_FORMAT_D32_SFLOAT: return "D32";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32S8";
    default: return nullptr;
    }
}

uint32_t query(VkPipelineStageFlagBits stage) {
    if (!g_cur || g_cur->used >= pool_queries()) return UINT32_MAX;
    uint32_t q = g_cur->used++;
    vkCmdWriteTimestamp(R.cmd, stage, g_cur->pool, q);
    return q;
}

// the GPU has finished the command buffer: read its timestamps and add them up
void collect(const std::shared_ptr<CmdProf>& p) {
    std::vector<uint64_t> t(p->used);
    if (p->used &&
        vkGetQueryPoolResults(R.device, p->pool, 0, p->used, t.size() * 8, t.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        auto at = [&](uint32_t q) { return t[q] & g_mask; };
        uint64_t first = at(p->cmdBegin), last = at(p->used - 1);
        if (last > first) {
            g_busy_ns += (last - first) * g_period;
            // dynamic resolution: only time no earlier command buffer covered (the next one's start
            // timestamp can come while the GPU still works on the previous one: the sum went past 100%)
            uint64_t from = std::max(first, g_last_end);
            if (last > from) dynres_gpu_busy((last - from) * g_period);
        }
        if (g_last_end && first > g_last_end) g_gap_ns += (first - g_last_end) * g_period;
        g_last_end = last;
        for (const Pass& ps : p->passes) {
            if (ps.end == UINT32_MAX || ps.begin == UINT32_MAX || at(ps.end) < at(ps.begin)) continue;
            double ns = (at(ps.end) - at(ps.begin)) * g_period;
            Stat& s = g_stats[ps.label];
            s.ns += ns;
            s.count++;
            s.draws += ps.draws;
            (ps.op ? g_op_ns : g_pass_ns) += ns;
            (ps.op ? g_op_count : g_pass_count)++;
        }
    }
    g_free.push_back(p->pool);
}

}  // namespace

// a command buffer was begun (vk_device.cpp)
void prof_cmd_begin() {
    if (!timing()) return;
    g_cur = std::make_shared<CmdProf>();
    if (!g_free.empty()) {
        g_cur->pool = g_free.back();
        g_free.pop_back();
    } else {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = pool_queries();
        VK_CHECK(vkCreateQueryPool(R.device, &qi, nullptr, &g_cur->pool));
    }
    vkCmdResetQueryPool(R.cmd, g_cur->pool, 0, pool_queries());
    g_cur->cmdBegin = query(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    g_open = -1;
    std::shared_ptr<CmdProf> p = g_cur;
    on_complete([p] { collect(p); });
}

// before vkEndCommandBuffer
void prof_cmd_end() {
    if (!g_cur) return;
    query(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    g_cur.reset();
}

static int label_id(const std::string& label, bool op = false) {
    auto it = g_label_ids.find(label);
    if (it != g_label_ids.end()) return it->second;
    int id = (int)g_labels.size();
    g_labels.push_back(label);
    g_label_ids[label] = id;
    g_stats.emplace_back();
    g_stats.back().op = op;
    return id;
}

// a game render pass begins: its attachments describe it
void prof_pass_begin(uint32_t w, uint32_t h, const VkFormat* colors, uint32_t nColors, VkFormat depth) {
    if (!g_on || !g_cur) return;
    std::string label = std::to_string(w) + "x" + std::to_string(h);
    for (uint32_t i = 0; i < nColors; i++) {
        const char* n = fmt_name(colors[i]);
        label += std::string(i ? "+" : " ") + (n ? n : std::to_string((int)colors[i]));
    }
    if (depth) {
        const char* n = fmt_name(depth);
        label += std::string(" depth ") + (n ? n : std::to_string((int)depth));
    }
    int id = label_id(label);
    g_cur->passes.push_back({query(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT), UINT32_MAX, id, 0, 0, false});
    g_open = (int)g_cur->passes.size() - 1;
    g_draws_at_begin = R.drawCount;
}

// a draw with this pixel shader (guest address) was recorded
static uint32_t g_last_ps = 0;
void prof_draw(uint32_t ps) { g_last_ps = ps; }

// the open game render pass ended
void prof_pass_end() {
    if (!g_on || !g_cur || g_open < 0) return;
    Pass& p = g_cur->passes[g_open];
    p.end = query(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    p.draws = (uint32_t)(R.drawCount - g_draws_at_begin);
    if (p.draws && p.draws <= 2) {  // full-screen effects: one kind per pixel shader
        char b[16];
        snprintf(b, sizeof b, "%08X", g_last_ps);
        p.label = label_id(g_labels[p.label] + " ps " + b);
    }
    g_open = -1;
}

// a transfer or compute command outside the render passes (vk_record.cpp wrappers): timed on its own
static int g_op_open = -1;
void prof_op_begin(VkCommandBuffer cmd, const char* kind, uint32_t w, uint32_t h) {
    if (!g_on || !g_cur || cmd != R.cmd || g_open >= 0 || g_op_open >= 0) return;
    std::string label = std::string("[") + kind + "]";
    if (w) label += " " + std::to_string(w) + "x" + std::to_string(h);
    g_cur->passes.push_back({query(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT), UINT32_MAX, label_id(label, true), 0, 0, true});
    g_op_open = (int)g_cur->passes.size() - 1;
}
void prof_op_end(VkCommandBuffer cmd) {
    if (!g_on || !g_cur || cmd != R.cmd || g_op_open < 0) return;
    g_cur->passes[g_op_open].end = query(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    g_op_open = -1;
}

// the GPU can time command buffers (dynamic resolution needs it)
bool gpu_timestamps() {
    on();
    return g_timestamps;
}

// once per game frame: the report every 150 frames
void prof_frame() {
    if (!g_on) return;
    if (++g_frames < 150) return;
    double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_window_start).count();
    double f = (double)g_frames;
    LOG("[gpuprof] %llu frames: frame %.1f ms; GPU busy %.1f ms/frame (game passes %.1f in %.0f, other commands %.1f in %.0f), "
        "idle between submissions %.1f ms/frame",
        (unsigned long long)g_frames, wall / f, g_busy_ns / 1e6 / f, g_pass_ns / 1e6 / f, g_pass_count / f, g_op_ns / 1e6 / f,
        g_op_count / f, g_gap_ns / 1e6 / f);
    std::vector<int> order(g_stats.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [](int a, int b) { return g_stats[a].ns > g_stats[b].ns; });
    for (size_t k = 0; k < order.size() && k < 24; k++) {
        const Stat& s = g_stats[order[k]];
        if (!s.count) break;
        LOG("[gpuprof]   %6.2f ms/frame  %5.1f passes/frame  %6.0f draws/frame  %s", s.ns / 1e6 / f, s.count / f, s.draws / f,
            g_labels[order[k]].c_str());
    }
    for (Stat& s : g_stats) s = Stat{};
    g_busy_ns = g_pass_ns = g_op_ns = g_gap_ns = 0;
    g_pass_count = g_op_count = 0;
    g_frames = 0;
    g_window_start = std::chrono::steady_clock::now();
}

}  // namespace gfx
