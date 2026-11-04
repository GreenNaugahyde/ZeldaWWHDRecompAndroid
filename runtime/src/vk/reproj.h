// 60 fps (generated): in-between frames made on the GPU by reprojecting the real frame (reproj.cpp)
#pragma once
#include <vector>

#include "vk.h"

namespace gfx::rp {
bool enabled();
void set_enabled(bool on);
// render thread: the frame's camera (OP_FRAMEGEN_CAMERA) and the start of a layout (OP_LAYOUT_ROOT)
void note_camera(const float eye[3], const float center[3], const float up[3], float fovy, float znear, float zfar);
void layout_root(const uint32_t* regs, bool tv);
// render thread: the main depth buffer is about to be cleared again in the same frame (a second 3D
// pass: the title's boat): what the first pass left is kept for the in-between frame
void keep_depth(Surface* s);
// window-sized pictures, as fg::resize / fg::record / fg::view_of (vk_device.cpp present_frame_fg):
// record() returns the real picture (a copy of `comp`), then the generated one
bool resize(uint32_t w, uint32_t h);
std::vector<const Image*> record(VkCommandBuffer cmd, const Image& comp, VkImageView compView, const float tvRect[4], bool generate);
VkImageView view_of(const Image* img);
void destroy();
}  // namespace gfx::rp
