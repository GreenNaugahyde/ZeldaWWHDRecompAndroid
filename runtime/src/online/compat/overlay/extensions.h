// Online co-op compat (../README.md): upstream's overlay extension points that the co-op code uses. Frame layers are
// drawn by the app over the game (compat.cpp records their ImGui shapes: Native.coopOverlay).
#pragma once
#include <functional>
#include <string>
#include <vector>
namespace overlay::ext {
struct FrameLayer {
    std::function<bool(std::string& toast)> wanted;  // drawn this frame? (toast: unused here)
    std::function<bool(float w, float h)> draw;      // the shapes, on the foreground draw list, in pixels of w x h
};
void add_frame_layer(FrameLayer layer);
// the app's overlay (any thread but one at a time): every layer's shapes for a w x h view, as floats
// {kind, colour bits, x0, y0, x1, y1, x2, y2, x3, y3, size, text index}; texts in `texts`
void draw_layers(float w, float h, std::vector<float>& shapes, std::vector<std::string>& texts);
}  // namespace overlay::ext
