// Frame generation: frames interpolated between the game's frames by the LSFG 3 network of
// Lossless Scaling. The network's compute shaders are not part of this project: they are read at
// run time from the user's own copy of Lossless.dll (WWHD_LSFG_DLL). This file and lsfg.cpp are an
// independent implementation that only drives those shaders.
#pragma once
#include <string>
#include <vector>

#include "vk.h"

namespace gfx::fg {

struct Config {
    std::string dll;          // path to Lossless.dll
    bool performance = true;  // the smaller of the DLL's two networks
    float flowScale = 1.0f;   // resolution of the motion estimate relative to the frame (0.25..1)
    int multiplier = 2;       // presented frames per game frame (2..4)
    bool uiDetection = true;  // keep static content (the HUD) from being warped
    float uiThreshold = 0.5f;
};

// Read and check the shaders; false (and a log message) if the DLL can't be used.
bool load(const Config& cfg);
bool loaded();
// GPU time of the network per game frame in ms, smoothed (0 until measured)
float gpu_ms();
// Without a GPU: "" if `path` is a Lossless.dll with the shaders this code drives, else why not.
// `tested` (optional): whether its shaders are a version this code was tested with (a structurally
// matching but unknown version may compute something else and show wrong frames).
std::string check_dll(const std::string& path, bool* tested = nullptr);
// Why the last attempt to start frame generation failed ("" if it runs or is off).
std::string last_error();
const Config& config();

// Render thread. Size the network for input frames `src` of w x h (recreates everything; GPU idle).
bool resize(uint32_t w, uint32_t h, VkImageView src);
// Records the network for a new frame in `src` (sampled in GENERAL layout) into `cmd`. Returns
// the images to present in order: the generated frames (none for the first frame after a resize
// or without `generate`), then a copy of the input. They stay unchanged until record() was called
// twice more.
std::vector<const Image*> record(VkCommandBuffer cmd, bool generate);
VkImageView view_of(const Image* img);
void destroy();
// the GPU is idle: release everything (pipelines too); load() can be called again
void unload();

}  // namespace gfx::fg
