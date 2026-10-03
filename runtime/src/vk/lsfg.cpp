// Frame generation with the LSFG 3 network (see lsfg.h).
//
// Lossless.dll carries the network as compute shaders in its resources (RCDATA): DXBC for its own
// Direct3D path and two SPIR-V sets, one using 16-bit floats and one without. Each set has 49
// shaders: a frame copy, a luminance pyramid, the final blend, and two variants of the network
// ("quality" and "performance") of 23 shaders each. Every shader reads the uniform block at binding
// 0, samplers at 16+, sampled images at 32+ and writes storage images at 48+.
//
// The network, per game frame:
//  - copy the frame (rgba8) and build a 7-level luminance pyramid (r8);
//  - per pyramid level, a 4-layer encoder turns the luminance into feature maps at a quarter of
//    the level's size (kept for the previous frame too);
//  - per interpolated frame, coarse to fine over the levels: three heads each correlate the two
//    frames' features (warped by the coarser level's estimate) and refine a field in 4 more
//    layers: two bidirectional motion fields (rgba16f: xy towards the previous frame, zw towards
//    the next) on all levels and the other one on the three finest; the third head blends weights.
//  - the final pass warps both frames by the fields and blends them.
// Sizes: luminance level i is floor(W*fs / 2^i); features of level i are ceil(luma_i / 4); the
// network uses 7 levels if H*fs > 512, else 6. Uniforms: FirstIter marks the coarsest level,
// FirstIterS the coarsest level of the fine-only heads, ResolutionInvScale is 1/fs, Timestamp is
// the interpolated frame's position between the previous (0) and the new frame (1).
#include "lsfg.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <string>

#include "../disc/crypto.h"
#include "runtime.h"

namespace gfx::fg {
namespace {

// ---------------------------------------------------------------- shaders from the DLL
constexpr uint32_t kFp16Base = 303, kFp32Base = 352, kShaders = 49;
// shader indices within a set
enum : uint32_t { S_COPY = 0, S_MIPS = 1, S_BLEND = 2, S_QUALITY = 3, S_PERF = 26 };
// bindings per shader: samplers, sampled images, storage images (checked against the DLL)
const uint8_t kIface[kShaders][3] = {
    {0, 1, 1},  {1, 1, 7},  {2, 5, 1},  {2, 9, 3},  {2, 10, 2}, {1, 3, 4}, {1, 4, 4}, {1, 4, 4},  {2, 6, 1}, {1, 3, 4},
    {1, 4, 4},  {1, 4, 4},  {2, 6, 1},  {1, 1, 2},  {1, 2, 2},  {1, 2, 4}, {1, 4, 4}, {1, 2, 2},  {1, 2, 2}, {1, 2, 2},
    {2, 3, 1},  {1, 12, 2}, {1, 2, 2},  {1, 2, 2},  {1, 2, 2},  {1, 2, 6}, {2, 5, 3}, {2, 6, 1},  {1, 3, 2}, {1, 2, 2},
    {1, 2, 2},  {2, 4, 1},  {1, 3, 2},  {1, 2, 2},  {1, 2, 2},  {2, 4, 1}, {1, 1, 1}, {1, 1, 1},  {1, 1, 2}, {1, 2, 2},
    {1, 1, 1},  {1, 1, 1},  {1, 1, 1},  {2, 2, 1},  {1, 6, 2},  {1, 2, 2}, {1, 2, 2}, {1, 2, 2},  {1, 2, 6}};

// Roles of a network variant's shaders (offsets from S_QUALITY / S_PERF).
struct Variant {
    uint32_t feats;          // feature maps per frame and level
    uint32_t enc[4];         // encoder: luma -> half (stride 2), half -> half, half -> quarter (stride 2), -> features
    uint32_t encOut[3];      // outputs of the first three encoder layers
    uint32_t corr, corr3;    // correlation layers of heads 1/2 and of head 3
    uint32_t corrOut, corr3Out;
    uint32_t head[3][4];     // the 4 refinement layers of each head (the last writes the field)
    uint32_t headOut[3];     // outputs of each head's first three refinement layers
    uint32_t mask[5];        // UI detection: 4 layers over three frames' features, then the mask pyramid
};
const Variant kQuality = {4, {13, 14, 15, 16}, {2, 2, 4}, 3, 4, 3, 2, {{5, 6, 7, 8}, {9, 10, 11, 12}, {17, 18, 19, 20}}, {4, 4, 2},
                          {21, 22, 23, 24, 25}};
const Variant kPerf = {2, {36, 37, 38, 39}, {1, 1, 2}, 26, 27, 3, 1, {{28, 29, 30, 31}, {32, 33, 34, 35}, {40, 41, 42, 43}}, {2, 2, 1},
                       {44, 45, 46, 47, 48}};

struct Shader {
    std::vector<uint32_t> spirv;
    std::vector<uint32_t> bindings;
};

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n > 0 && fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

template <typename T> bool rd(const std::vector<uint8_t>& f, size_t off, T& v) {
    if (off + sizeof(T) > f.size()) return false;
    memcpy(&v, f.data() + off, sizeof(T));
    return true;
}

// RCDATA resources of a PE file: id -> (file offset, size)
bool pe_rcdata(const std::vector<uint8_t>& f, std::map<uint32_t, std::pair<size_t, size_t>>& out, std::string& err) {
    uint32_t pe = 0, sig = 0;
    uint16_t nsec = 0, optSize = 0, magic = 0;
    if (!rd(f, 0x3c, pe) || !rd(f, pe, sig) || sig != 0x4550) return err = "not a PE file", false;
    rd(f, pe + 6, nsec);
    rd(f, pe + 20, optSize);
    size_t opt = pe + 24;
    rd(f, opt, magic);
    size_t dirs = opt + (magic == 0x20b ? 112 : 96);
    uint32_t rsrcRva = 0, rsrcSize = 0;
    if (!rd(f, dirs + 2 * 8, rsrcRva) || !rd(f, dirs + 2 * 8 + 4, rsrcSize) || !rsrcRva) return err = "no resources", false;
    auto toOff = [&](uint32_t rva, size_t& off) {
        for (uint32_t i = 0; i < nsec; i++) {
            size_t s = opt + optSize + 40 * i;
            uint32_t vsize = 0, va = 0, rsize = 0, raw = 0;
            rd(f, s + 8, vsize);
            rd(f, s + 12, va);
            rd(f, s + 16, rsize);
            rd(f, s + 20, raw);
            if (rva >= va && rva < va + std::max(vsize, rsize)) {
                off = raw + (rva - va);
                return off < f.size();
            }
        }
        return false;
    };
    size_t root;
    if (!toOff(rsrcRva, root)) return err = "bad resource section", false;
    // directory entries: (id, offset); high bit of the offset: a subdirectory
    auto entries = [&](size_t dir, std::vector<std::pair<uint32_t, uint32_t>>& e) {
        uint16_t named = 0, ids = 0;
        if (!rd(f, dir + 12, named) || !rd(f, dir + 14, ids)) return false;
        for (uint32_t i = 0; i < (uint32_t)named + ids; i++) {
            uint32_t name = 0, off = 0;
            if (!rd(f, dir + 16 + 8 * i, name) || !rd(f, dir + 20 + 8 * i, off)) return false;
            e.push_back({name, off});
        }
        return true;
    };
    std::vector<std::pair<uint32_t, uint32_t>> types;
    if (!entries(root, types)) return err = "bad resource directory", false;
    for (auto [type, toff] : types) {
        if (type != 10 || !(toff & 0x80000000u)) continue;  // RT_RCDATA
        std::vector<std::pair<uint32_t, uint32_t>> names;
        if (!entries(root + (toff & 0x7fffffff), names)) return err = "bad resource directory", false;
        for (auto [id, noff] : names) {
            if ((id & 0x80000000u) || !(noff & 0x80000000u)) continue;
            std::vector<std::pair<uint32_t, uint32_t>> langs;
            if (!entries(root + (noff & 0x7fffffff), langs) || langs.empty()) continue;
            size_t data = root + (langs[0].second & 0x7fffffff);
            uint32_t rva = 0, size = 0;
            size_t off;
            if (!rd(f, data, rva) || !rd(f, data + 4, size) || !toOff(rva, off) || off + size > f.size()) continue;
            out[id] = {off, size};
        }
    }
    if (out.empty()) return err = "no RCDATA resources", false;
    return true;
}

// Collects the descriptor bindings and makes the images' declared formats match ours: the copy
// declares rgba32f and the blend leaves the format open (needs WriteWithoutFormat), both write rgba8.
bool prepare_spirv(Shader& s, bool rgba8Output, std::string& err) {
    auto& w = s.spirv;
    if (w.size() < 5 || w[0] != 0x07230203) return err = "not SPIR-V", false;
    for (size_t p = 5; p < w.size();) {
        uint32_t op = w[p] & 0xffff, n = w[p] >> 16;
        if (!n || p + n > w.size()) return err = "malformed SPIR-V", false;
        if (op == 71 /*OpDecorate*/ && n >= 4 && w[p + 2] == 33 /*Binding*/) s.bindings.push_back(w[p + 3]);
        if (rgba8Output && op == 25 /*OpTypeImage*/ && n >= 9 && w[p + 7] == 2 /*storage*/) w[p + 8] = 4 /*Rgba8*/;
        if (rgba8Output && op == 17 /*OpCapability*/ && n == 2 && w[p + 1] == 56 /*StorageImageWriteWithoutFormat*/)
            w[p + 1] = 1;  // Shader (declared already; a repeat is harmless)
        p += n;
    }
    std::sort(s.bindings.begin(), s.bindings.end());
    return true;
}

// one shader set (base: first resource id) into `out`; false and `err` if it doesn't match
bool read_set(const std::vector<uint8_t>& file, const std::map<uint32_t, std::pair<size_t, size_t>>& res, uint32_t base,
              Shader* out, std::string& err) {
    for (uint32_t i = 0; i < kShaders; i++) {
        auto it = res.find(base + i);
        Shader& s = out[i];
        s = Shader{};
        if (it == res.end() || it->second.second % 4) {
            err = "shader " + std::to_string(base + i) + " missing: unsupported Lossless.dll version";
            return false;
        }
        s.spirv.resize(it->second.second / 4);
        memcpy(s.spirv.data(), file.data() + it->second.first, it->second.second);
        if (!prepare_spirv(s, i == S_COPY || i == S_BLEND, err)) {
            err = "shader " + std::to_string(base + i) + ": " + err;
            return false;
        }
        uint32_t n[3] = {};
        for (uint32_t b : s.bindings)
            if (b >= 16) n[std::min(2u, (b - 16) / 16)]++;
        if (n[0] != kIface[i][0] || n[1] != kIface[i][1] || n[2] != kIface[i][2]) {
            err = "shader " + std::to_string(base + i) + " has an unexpected interface: unsupported Lossless.dll version";
            return false;
        }
    }
    return true;
}

bool read_dll(const std::string& path, std::vector<uint8_t>& file, std::map<uint32_t, std::pair<size_t, size_t>>& res,
              std::string& err) {
    if (!read_file(path, file)) return err = "cannot read the file", false;
    return pe_rcdata(file, res, err);
}

// ---------------------------------------------------------------- state
Config g_cfg;
std::atomic<float> g_gpu_ms{0};  // the network's GPU time per game frame, smoothed (performance overlay)
bool g_loaded = false;
std::mutex g_error_mutex;
std::string g_error;  // why the last load failed

void set_error(const std::string& e) {
    std::lock_guard<std::mutex> lk(g_error_mutex);
    g_error = e;
}

// SHA-1 of both shader sets as stored in the DLL: identifies the shader version, not the DLL build
std::string shader_fingerprint(const std::vector<uint8_t>& file, const std::map<uint32_t, std::pair<size_t, size_t>>& res) {
    std::vector<uint8_t> all;
    for (uint32_t id = kFp16Base; id < kFp32Base + kShaders; id++) {
        auto it = res.find(id);
        if (it == res.end()) return "";
        all.insert(all.end(), file.begin() + it->second.first, file.begin() + it->second.first + it->second.second);
    }
    uint8_t h[20];
    disc::sha1(all.data(), all.size(), h);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return hex;
}

// shader versions this code was built and tested against (the pass graph was recovered from them)
const char* const kTestedShaders[] = {
    "fc6092a72b94003fac3d68e9a7b54954422db757",  // the Lossless.dll this was developed with (7,521,280 bytes)
};
Shader g_shaders[kShaders];

struct Tex {
    Image img;
    VkImageView view = VK_NULL_HANDLE;
};

struct Pipe {
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};
Pipe g_pipes[kShaders];
VkSampler g_borderBlack = VK_NULL_HANDLE, g_borderWhite = VK_NULL_HANDLE, g_clamp = VK_NULL_HANDLE;

// a dispatch with its resources
struct Pass {
    uint32_t shader = 0;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t gx = 1, gy = 1;
    uint32_t level = 0;                 // selects the uniforms
    std::vector<const Tex*> ins, outs;  // for barriers
};

constexpr uint32_t kSlots = 3;  // frames kept: copies and features
constexpr uint32_t kMaxLevels = 7, kMaxOut = 3;
constexpr VkDeviceSize kUboStride = 256;

struct Net {
    uint32_t w = 0, h = 0, levels = 0;
    float fs = 1;
    const Variant* v = nullptr;
    std::vector<std::unique_ptr<Tex>> texs;  // everything, for destruction
    Tex* dummy = nullptr;                    // unbound inputs read zero
    Tex* frames[kSlots] = {};                // copies of the input
    Tex* luma[7] = {};
    Tex* feats[kSlots][kMaxLevels][4] = {};
    Tex* out[2][kMaxOut] = {};  // generated frames, double-buffered
    std::vector<Tex*> enc[kMaxLevels][3];                 // encoder scratch (shared by the slots)
    Tex* field[kMaxLevels][3] = {};                       // per level and head: the field it produces
    std::vector<Tex*> scrA[kMaxLevels][3], scrB[kMaxLevels][3];  // and its scratch maps
    Tex* maskScr[2][2] = {};  // UI detection scratch
    Tex* mask[6] = {};        // where the image is static (the HUD): motion is suppressed there
    VkBuffer ubo = VK_NULL_HANDLE;
    VmaAllocation uboAlloc = nullptr;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkImageView srcView = VK_NULL_HANDLE;
    std::vector<Pass> framePasses[kSlots];       // per slot written
    std::vector<Pass> genPasses[kSlots];         // per slot of the newer frame
    std::vector<Pass> blend[kSlots][2][kMaxOut];  // per slot, output parity, generated frame
    uint64_t frame = 0;                          // frames recorded since resize
    VkQueryPool queries = VK_NULL_HANDLE;        // GPU time of the network: start and end per slot
    double gpuMs = 0;
    uint32_t gpuSamples = 0;
    bool fresh = true;                           // images not yet in GENERAL layout
};
Net g;

Tex* new_tex(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags extra = 0) {
    auto t = std::make_unique<Tex>();
    if (!create_image(t->img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, fmt, std::max(w, 1u), std::max(h, 1u), 1, 1, 1,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra, false,
                      false, false))
        fatal("frame generation: out of memory");
    t->view = make_view(t->img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    g.texs.push_back(std::move(t));
    return g.texs.back().get();
}

uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

VkShaderModule module_for(uint32_t i) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = g_shaders[i].spirv.size() * 4;
    ci.pCode = g_shaders[i].spirv.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, nullptr, &m));
    return m;
}

VkDescriptorType type_of(uint32_t binding) {
    if (binding < 16) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    if (binding < 32) return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (binding < 48) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

void create_pipelines() {
    std::vector<VkComputePipelineCreateInfo> cis;
    std::vector<VkShaderModule> mods;
    uint32_t first = g_cfg.performance ? S_PERF : S_QUALITY;
    std::vector<uint32_t> used = {S_COPY, S_MIPS, S_BLEND};
    for (uint32_t i = first; i < first + 23; i++) used.push_back(i);
    for (uint32_t i : used) {
        Pipe& p = g_pipes[i];
        std::vector<VkDescriptorSetLayoutBinding> bs;
        for (uint32_t b : g_shaders[i].bindings) bs.push_back({b, type_of(b), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = (uint32_t)bs.size();
        dl.pBindings = bs.data();
        VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &p.dsl));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &p.dsl;
        VK_CHECK(vkCreatePipelineLayout(R.device, &pl, nullptr, &p.layout));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_for(i);
        ci.stage.pName = "main";
        ci.layout = p.layout;
        mods.push_back(ci.stage.module);
        cis.push_back(ci);
    }
    std::vector<VkPipeline> pipes(cis.size());
    VK_CHECK(vkCreateComputePipelines(R.device, R.pipelineCache, (uint32_t)cis.size(), cis.data(), nullptr, pipes.data()));
    for (size_t k = 0; k < used.size(); k++) g_pipes[used[k]].pipe = pipes[k];
    for (auto m : mods) vkDestroyShaderModule(R.device, m, nullptr);

    auto sampler = [](VkSamplerAddressMode mode, VkBorderColor border) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = mode;
        si.borderColor = border;
        si.maxLod = VK_LOD_CLAMP_NONE;
        VkSampler s;
        VK_CHECK(vkCreateSampler(R.device, &si, nullptr, &s));
        return s;
    };
    g_borderBlack = sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
    g_borderWhite = sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE);
    g_clamp = sampler(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
}

// a pass reading `ins` (null: unbound) and writing `outs`; `srcView` stands in for the input frame
Pass make_pass(uint32_t shader, uint32_t level, std::vector<const Tex*> ins, std::vector<const Tex*> outs, uint32_t gx,
               uint32_t gy, VkImageView srcView = VK_NULL_HANDLE) {
    Pass p;
    p.shader = shader;
    p.level = level;
    p.gx = std::max(gx, 1u);
    p.gy = std::max(gy, 1u);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g_pipes[shader].dsl;
    VK_CHECK(vkAllocateDescriptorSets(R.device, &ai, &p.set));
    // samplers: the correlation layers sample their warped inputs with a white border
    bool white = shader == g.v->corr || shader == g.v->corr3 || shader == g.v->mask[0];
    std::vector<VkWriteDescriptorSet> ws;
    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(32);
    VkDescriptorBufferInfo ub{g.ubo, 0, 48};
    for (uint32_t b : g_shaders[shader].bindings) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p.set;
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = type_of(b);
        if (b < 16) {
            w.pBufferInfo = &ub;
        } else if (b < 32) {
            infos.push_back({b == 16 ? (white ? g_borderWhite : g_borderBlack) : g_clamp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
            w.pImageInfo = &infos.back();
        } else if (b < 48) {
            uint32_t i = b - 32;
            const Tex* t = i < ins.size() ? ins[i] : nullptr;
            VkImageView v = (i == 0 && srcView) ? srcView : (t ? t->view : g.dummy->view);
            infos.push_back({VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
            w.pImageInfo = &infos.back();
        } else {
            uint32_t i = b - 48;
            if (i >= outs.size() || !outs[i]) fatal("frame generation: shader %u output %u unbound", shader, i);
            infos.push_back({VK_NULL_HANDLE, outs[i]->view, VK_IMAGE_LAYOUT_GENERAL});
            w.pImageInfo = &infos.back();
        }
        ws.push_back(w);
    }
    vkUpdateDescriptorSets(R.device, (uint32_t)ws.size(), ws.data(), 0, nullptr);
    for (auto* t : ins)
        if (t) p.ins.push_back(t);
    p.outs = outs;
    return p;
}

struct LevelDims {
    uint32_t lw, lh;  // luminance
    uint32_t hw, hh;  // encoder's half size
    uint32_t fw, fh;  // features
};
LevelDims dims(uint32_t i) {
    LevelDims d;
    d.lw = std::max(1u, (uint32_t)std::floor(g.w * g.fs / (float)(1u << i)));
    d.lh = std::max(1u, (uint32_t)std::floor(g.h * g.fs / (float)(1u << i)));
    d.hw = div_up(d.lw, 2);
    d.hh = div_up(d.lh, 2);
    d.fw = div_up(d.hw, 2);
    d.fh = div_up(d.hh, 2);
    return d;
}

void write_uniforms() {
    // [frame index (0: per-frame passes, k: k-th generated frame)][level]
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(R.vma, g.uboAlloc, &info);
    auto* base = (uint8_t*)info.pMappedData;
    for (uint32_t k = 0; k < kMaxOut + 1; k++)
        for (uint32_t l = 0; l < kMaxLevels; l++) {
            struct {
                uint32_t offset[2], firstIter, firstIterS, advancedColorKind, hdrSupport;
                float resolutionInvScale, timestamp, uiThreshold, pad[3];
            } u{};
            u.firstIter = l == g.levels - 1;
            u.firstIterS = l == 2;
            u.resolutionInvScale = 1.0f / g.fs;
            u.timestamp = k ? (float)k / g_cfg.multiplier : 0.0f;
            u.uiThreshold = g_cfg.uiThreshold;
            memcpy(base + (k * kMaxLevels + l) * kUboStride, &u, sizeof u);
        }
}

void build_passes() {
    const Variant& v = *g.v;
    // ---- per frame: copy, luminance pyramid, encoder on each level
    for (uint32_t s = 0; s < kSlots; s++) {
        auto& P = g.framePasses[s];
        P.push_back(make_pass(S_COPY, 0, {nullptr}, {g.frames[s]}, div_up(g.w, 8), div_up(g.h, 8), g.srcView));
        LevelDims d0 = dims(0);
        P.push_back(make_pass(S_MIPS, 0, {g.frames[s]}, {g.luma[0], g.luma[1], g.luma[2], g.luma[3], g.luma[4], g.luma[5], g.luma[6]},
                              div_up(d0.lw, 64), div_up(d0.lh, 64)));
        // encoder layer by layer over all levels: the levels are independent
        std::vector<std::vector<const Tex*>> prev(g.levels);
        for (uint32_t l = 0; l < g.levels; l++) prev[l] = {g.luma[l]};
        for (uint32_t layer = 0; layer < 4; layer++)
            for (uint32_t l = 0; l < g.levels; l++) {
                LevelDims d = dims(l);
                uint32_t ow = layer < 2 ? d.hw : d.fw, oh = layer < 2 ? d.hh : d.fh;
                std::vector<const Tex*> outs;
                if (layer < 3) {
                    outs.assign(g.enc[l][layer].begin(), g.enc[l][layer].end());
                } else {
                    for (uint32_t i = 0; i < v.feats; i++) outs.push_back(g.feats[s][l][i]);
                }
                P.push_back(make_pass(v.enc[layer], l, prev[l], outs, div_up(ow, 8), div_up(oh, 8)));
                prev[l] = outs;
            }
        if (g_cfg.uiDetection) {
            // static content in the last three frames (oldest first) at the finest feature level
            LevelDims d = dims(0);
            uint32_t gx = div_up(d.fw, 8), gy = div_up(d.fh, 8);
            std::vector<const Tex*> in;
            for (uint32_t back = 2; back != ~0u; back--)
                for (uint32_t i = 0; i < v.feats; i++) in.push_back(g.feats[(s + kSlots - back) % kSlots][0][i]);
            Tex** a = g.maskScr[0];
            Tex** b = g.maskScr[1];
            P.push_back(make_pass(v.mask[0], 0, in, {a[0], a[1]}, gx, gy));
            P.push_back(make_pass(v.mask[1], 0, {a[0], a[1]}, {b[0], b[1]}, gx, gy));
            P.push_back(make_pass(v.mask[2], 0, {b[0], b[1]}, {a[0], a[1]}, gx, gy));
            P.push_back(make_pass(v.mask[3], 0, {a[0], a[1]}, {b[0], b[1]}, gx, gy));
            P.push_back(make_pass(v.mask[4], 0, {b[0], b[1]}, {g.mask[0], g.mask[1], g.mask[2], g.mask[3], g.mask[4], g.mask[5]},
                                  div_up(d.fw, 32), div_up(d.fh, 32)));
        }
    }

    // ---- per generated frame: the heads, coarse to fine; then the blend
    auto& field = g.field;
    auto& scrA = g.scrA;
    auto& scrB = g.scrB;
    for (uint32_t s = 0; s < kSlots; s++) {
        uint32_t ps = (s + kSlots - 1) % kSlots;  // the previous frame's slot
        auto& P = g.genPasses[s];
        for (int l = (int)g.levels - 1; l >= 0; l--) {
            LevelDims d = dims(l);
            uint32_t gx = div_up(d.fw, 8), gy = div_up(d.fh, 8);
            bool coarsest = l == (int)g.levels - 1;
            std::vector<const Tex*> feats;
            for (uint32_t i = 0; i < v.feats; i++) feats.push_back(g.feats[ps][l][i]);
            for (uint32_t i = 0; i < v.feats; i++) feats.push_back(g.feats[s][l][i]);
            const Tex* c1 = coarsest ? nullptr : field[l + 1][0];
            const Tex* c2 = l + 1 <= 2 ? field[l + 1][1] : nullptr;
            const Tex* c3 = l + 1 <= 2 ? field[l + 1][2] : nullptr;
            int heads = l <= 2 ? 3 : 1;
            auto take = [](const std::vector<Tex*>& t, uint32_t n) { return std::vector<const Tex*>(t.begin(), t.begin() + n); };
            // correlation
            for (int hd = 0; hd < heads; hd++) {
                std::vector<const Tex*> in = feats;
                if (hd < 2) {
                    in.push_back(hd == 0 ? c1 : c2);
                    P.push_back(make_pass(v.corr, l, in, take(scrA[l][hd], v.corrOut), gx, gy));
                } else {
                    in.push_back(c1);
                    in.push_back(c2);
                    P.push_back(make_pass(v.corr3, l, in, take(scrA[l][hd], v.corr3Out), gx, gy));
                }
            }
            // refinement: three layers ping-ponging, then the field
            for (uint32_t layer = 0; layer < 4; layer++)
                for (int hd = 0; hd < heads; hd++) {
                    uint32_t nin = layer == 0 ? (hd < 2 ? v.corrOut : v.corr3Out) : v.headOut[hd];
                    auto& src = (layer % 2 == 0) ? scrA[l][hd] : scrB[l][hd];
                    auto& dst = (layer % 2 == 0) ? scrB[l][hd] : scrA[l][hd];
                    std::vector<const Tex*> in = take(src, nin);
                    if (layer < 3) {
                        P.push_back(make_pass(v.head[hd][layer], l, in, take(dst, v.headOut[hd]), gx, gy));
                    } else {
                        in.push_back(hd == 0 ? c1 : hd == 1 ? c2 : c3);
                        if (hd < 2) in.push_back(g.mask[std::min(l, 5)]);  // null without UI detection: no mask
                        P.push_back(make_pass(v.head[hd][layer], l, in, {field[l][hd]}, gx, gy));
                    }
                }
        }
        for (uint32_t par = 0; par < 2; par++)
            for (uint32_t k = 0; k < kMaxOut; k++)
                if (g.out[par][k])
                    g.blend[s][par][k].push_back(make_pass(S_BLEND, 0, {g.frames[ps], g.frames[s], field[0][0], field[0][1], field[0][2]},
                                                           {g.out[par][k]}, div_up(g.w, 16), div_up(g.h, 16)));
    }
}

void free_net() {
    if (!g.w) return;
    for (auto& t : g.texs) {
        if (t->view) vkDestroyImageView(R.device, t->view, nullptr);
        vmaDestroyImage(R.vma, t->img.image, t->img.alloc);
    }
    if (g.ubo) vmaDestroyBuffer(R.vma, g.ubo, g.uboAlloc);
    if (g.pool) vkDestroyDescriptorPool(R.device, g.pool, nullptr);
    if (g.queries) vkDestroyQueryPool(R.device, g.queries, nullptr);
    g = Net{};
}

// Records `passes` with barriers only where a pass reads or overwrites what an earlier one wrote
// (or overwrites what it read) since the last barrier.
void run(VkCommandBuffer cmd, const std::vector<Pass>& passes, uint32_t frameIndex, std::vector<const Tex*>& written,
         std::vector<const Tex*>& read) {
    auto has = [](const std::vector<const Tex*>& v, const Tex* t) { return std::find(v.begin(), v.end(), t) != v.end(); };
    for (const Pass& p : passes) {
        bool dep = false;
        for (auto* t : p.ins) dep |= has(written, t);
        for (auto* t : p.outs) dep |= has(written, t) || has(read, t);
        if (dep) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
            written.clear();
            read.clear();
        }
        const Pipe& pp = g_pipes[p.shader];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp.pipe);
        uint32_t off = (uint32_t)((frameIndex * kMaxLevels + p.level) * kUboStride);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp.layout, 0, 1, &p.set, 1, &off);
        vkCmdDispatch(cmd, p.gx, p.gy, 1);
        for (auto* t : p.outs) written.push_back(t);
        for (auto* t : p.ins) read.push_back(t);
    }
}
}  // namespace

// ---------------------------------------------------------------- interface
bool load(const Config& cfg) {
    g_cfg = cfg;
    g_cfg.multiplier = std::clamp(cfg.multiplier, 2, (int)kMaxOut + 1);
    g_cfg.flowScale = std::clamp(cfg.flowScale, 0.25f, 1.0f);
    std::vector<uint8_t> file;
    std::map<uint32_t, std::pair<size_t, size_t>> res;
    std::string err;
    // the 16-bit float set when the device can run it
    bool fp16 = R.shaderFloat16 && !getenv("WWHD_LSFG_FP32");
    if (!read_dll(cfg.dll, file, res, err) || !read_set(file, res, fp16 ? kFp16Base : kFp32Base, g_shaders, err)) {
        LOG("[fg] %s: %s; frame generation disabled", cfg.dll.c_str(), err.c_str());
        set_error(err);
        return false;
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R.pd, VK_FORMAT_R8_UNORM, &fp);
    if (!R.features.shaderStorageImageExtendedFormats || !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        LOG("[fg] the GPU can't write r8 storage images; frame generation disabled");
        set_error("this GPU can't write the image format frame generation needs");
        return false;
    }
    create_pipelines();
    g_loaded = true;
    set_error("");
    LOG("[fg] shader version %s", shader_fingerprint(file, res).c_str());
    LOG("[fg] LSFG 3 shaders loaded from %s (%s network, %s floats), x%d, flow scale %.2f, UI detection %s", cfg.dll.c_str(),
        g_cfg.performance ? "performance" : "quality", fp16 ? "16-bit" : "32-bit", g_cfg.multiplier, g_cfg.flowScale,
        g_cfg.uiDetection ? "on" : "off");
    return true;
}

std::string check_dll(const std::string& path, bool* tested) {
    std::vector<uint8_t> file;
    std::map<uint32_t, std::pair<size_t, size_t>> res;
    std::string err;
    std::vector<Shader> tmp(kShaders);
    if (!read_dll(path, file, res, err) || !read_set(file, res, kFp16Base, tmp.data(), err) ||
        !read_set(file, res, kFp32Base, tmp.data(), err))
        return err;
    if (tested) {
        std::string fp = shader_fingerprint(file, res);
        *tested = false;
        for (const char* k : kTestedShaders) *tested |= fp == k;
    }
    return "";
}

std::string last_error() {
    std::lock_guard<std::mutex> lk(g_error_mutex);
    return g_error;
}

bool loaded() { return g_loaded; }
float gpu_ms() { return g_gpu_ms.load(std::memory_order_relaxed); }
const Config& config() { return g_cfg; }

bool resize(uint32_t w, uint32_t h, VkImageView src) {
    free_net();
    g.w = w;
    g.h = h;
    g.fs = g_cfg.flowScale;
    g.levels = h * g.fs > 512 ? 7 : 6;
    g.v = g_cfg.performance ? &kPerf : &kQuality;
    g.dummy = new_tex(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    for (auto& f : g.frames) f = new_tex(w, h, VK_FORMAT_R8G8B8A8_UNORM);
    for (uint32_t i = 0; i < 7; i++) {
        LevelDims d = dims(i);
        g.luma[i] = new_tex(d.lw, d.lh, VK_FORMAT_R8_UNORM);
    }
    for (uint32_t s = 0; s < kSlots; s++)
        for (uint32_t l = 0; l < g.levels; l++)
            for (uint32_t i = 0; i < g.v->feats; i++) g.feats[s][l][i] = new_tex(dims(l).fw, dims(l).fh, VK_FORMAT_R8G8B8A8_UNORM);
    for (uint32_t par = 0; par < 2; par++)
        for (int k = 0; k < g_cfg.multiplier - 1; k++) g.out[par][k] = new_tex(w, h, VK_FORMAT_R8G8B8A8_UNORM);
    for (uint32_t l = 0; l < g.levels; l++) {
        LevelDims d = dims(l);
        for (uint32_t layer = 0; layer < 3; layer++)
            for (uint32_t i = 0; i < g.v->encOut[layer]; i++)
                g.enc[l][layer].push_back(new_tex(layer < 2 ? d.hw : d.fw, layer < 2 ? d.hh : d.fh, VK_FORMAT_R8G8B8A8_UNORM));
        for (uint32_t hd = 0; hd < 3; hd++) {
            if (hd > 0 && l > 2) continue;  // heads 2 and 3 work on the three finest levels
            g.field[l][hd] = new_tex(d.fw, d.fh, VK_FORMAT_R16G16B16A16_SFLOAT);
            for (uint32_t i = 0; i < 4; i++) {
                g.scrA[l][hd].push_back(new_tex(d.fw, d.fh, VK_FORMAT_R8G8B8A8_UNORM));
                g.scrB[l][hd].push_back(new_tex(d.fw, d.fh, VK_FORMAT_R8G8B8A8_UNORM));
            }
        }
    }

    if (g_cfg.uiDetection) {
        LevelDims d = dims(0);
        for (auto& pair : g.maskScr)
            for (auto& t : pair) t = new_tex(d.fw, d.fh, VK_FORMAT_R8G8B8A8_UNORM);
        for (uint32_t m = 0; m < 6; m++) g.mask[m] = new_tex(d.fw >> m, d.fh >> m, VK_FORMAT_R8_UNORM);
    }

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (kMaxOut + 1) * kMaxLevels * kUboStride;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VK_CHECK(vmaCreateBuffer(R.vma, &bi, &ai, &g.ubo, &g.uboAlloc, nullptr));
    write_uniforms();

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, 8192},
                                    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16384},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16384}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 4096;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(R.device, &pi, nullptr, &g.pool));
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2 * kSlots;
    if (R.props.limits.timestampComputeAndGraphics) VK_CHECK(vkCreateQueryPool(R.device, &qi, nullptr, &g.queries));
    g.srcView = src;
    build_passes();
    g.frame = 0;
    g.fresh = true;
    LOG("[fg] network for %ux%u: %u levels, features %ux%u to %ux%u, %zu images", w, h, g.levels, dims(0).fw, dims(0).fh,
        dims(g.levels - 1).fw, dims(g.levels - 1).fh, g.texs.size());
    return true;
}

std::vector<const Image*> record(VkCommandBuffer cmd, bool generate) {
    if (g.fresh) {
        std::vector<VkImageMemoryBarrier> bs;
        for (auto& t : g.texs) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = t->img.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bs.push_back(b);
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                             (uint32_t)bs.size(), bs.data());
        // zero: the dummy input, and the frames the UI detection reads before they exist
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (auto& t : g.texs) vkCmdClearColorImage(cmd, t->img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        g.fresh = false;
    }
    // the input was just rendered; the previous frames' outputs may still be being presented
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    uint32_t s = g.frame % kSlots, par = g.frame % 2;
    if (g.queries) {
        // the slot's previous use was two frames ago: usually done, else skip the sample
        uint64_t t[2];
        if (g.frame >= kSlots && vkGetQueryPoolResults(R.device, g.queries, 2 * s, 2, sizeof t, t, sizeof t[0],
                                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ms = (t[1] - t[0]) * R.props.limits.timestampPeriod * 1e-6;
            g.gpuMs += ms;
            float prev = g_gpu_ms.load(std::memory_order_relaxed);
            g_gpu_ms.store(prev ? prev + ((float)ms - prev) * 0.1f : (float)ms, std::memory_order_relaxed);
            if (++g.gpuSamples == 300) {
                LOG("[fg] network GPU time %.2f ms per game frame (%d generated)", g.gpuMs / g.gpuSamples, g_cfg.multiplier - 1);
                g.gpuMs = 0;
                g.gpuSamples = 0;
            }
        }
        vkCmdResetQueryPool(cmd, g.queries, 2 * s, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.queries, 2 * s);
    }
    std::vector<const Tex*> written, read;
    run(cmd, g.framePasses[s], 0, written, read);
    std::vector<const Image*> show;
    if (g.frame > 0 && generate)
        for (int k = 0; k < g_cfg.multiplier - 1; k++) {
            // every layer warps by the timestamp: the whole estimate is per generated frame
            run(cmd, g.genPasses[s], k + 1, written, read);
            run(cmd, g.blend[s][par][k], k + 1, written, read);
            show.push_back(&g.out[par][k]->img);
        }
    show.push_back(&g.frames[s]->img);
    if (g.queries) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.queries, 2 * s + 1);
    g.frame++;
    return show;
}

VkImageView view_of(const Image* img) {
    for (auto& t : g.texs)
        if (&t->img == img) return t->view;
    return VK_NULL_HANDLE;
}

void destroy() { free_net(); }

void unload() {
    free_net();
    for (Pipe& p : g_pipes) {
        if (p.pipe) vkDestroyPipeline(R.device, p.pipe, nullptr);
        if (p.layout) vkDestroyPipelineLayout(R.device, p.layout, nullptr);
        if (p.dsl) vkDestroyDescriptorSetLayout(R.device, p.dsl, nullptr);
        p = Pipe{};
    }
    for (VkSampler* s : {&g_borderBlack, &g_borderWhite, &g_clamp}) {
        if (*s) vkDestroySampler(R.device, *s, nullptr);
        *s = VK_NULL_HANDLE;
    }
    for (Shader& s : g_shaders) s = Shader{};
    g_loaded = false;
    g_gpu_ms = 0;
    set_error("");
    LOG("[fg] frame generation off");
}

}  // namespace gfx::fg
