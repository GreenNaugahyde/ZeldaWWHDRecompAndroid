// Online co-op compat (README.md): the few Dear ImGui calls upstream's co-op overlays make (coop_radar, coop_emotes,
// coop_pvp: shapes on the foreground draw list; coop_sync: a small panel). There is no ImGui on Android: the shapes are
// recorded (compat.cpp) and handed to the app, which draws them over the game (Native.coopOverlay); the panel's
// calls do nothing.
#pragma once
#include <cstdint>
typedef uint32_t ImU32;
struct ImVec2 {
    float x = 0, y = 0;
    constexpr ImVec2() = default;
    constexpr ImVec2(float x_, float y_) : x(x_), y(y_) {}
};
#define IM_COL32(R, G, B, A) (((ImU32)(A) << 24) | ((ImU32)(B) << 16) | ((ImU32)(G) << 8) | ((ImU32)(R)))
typedef int ImGuiCond;
typedef int ImGuiWindowFlags;
enum { ImGuiCond_Always = 1 };
enum {
    ImGuiWindowFlags_NoDecoration = 1, ImGuiWindowFlags_AlwaysAutoResize = 2, ImGuiWindowFlags_NoSavedSettings = 4,
    ImGuiWindowFlags_NoNav = 8, ImGuiWindowFlags_NoInputs = 16, ImGuiWindowFlags_NoFocusOnAppearing = 32,
};
struct ImGuiIO { ImVec2 DisplaySize; float FontGlobalScale = 1; };
struct ImDrawList {
    void AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float thickness = 1.0f);
    void AddRect(const ImVec2& lo, const ImVec2& hi, ImU32 col, float rounding = 0, int flags = 0, float thickness = 1.0f);
    void AddRect(const ImVec2& lo, const ImVec2& hi, ImU32 col, float rounding, float thickness) { AddRect(lo, hi, col, rounding, 0, thickness); }
    void AddRectFilled(const ImVec2& lo, const ImVec2& hi, ImU32 col, float rounding = 0, int flags = 0);
    void AddQuad(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d, ImU32 col, float thickness = 1.0f);
    void AddQuadFilled(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d, ImU32 col);
    void AddTriangleFilled(const ImVec2& a, const ImVec2& b, const ImVec2& c, ImU32 col);
    void AddCircle(const ImVec2& c, float r, ImU32 col, int segments = 0, float thickness = 1.0f);
    void AddCircleFilled(const ImVec2& c, float r, ImU32 col, int segments = 0);
    void PushClipRect(const ImVec2& lo, const ImVec2& hi, bool intersect = false);
    void PopClipRect();
    void AddText(const ImVec2& at, ImU32 col, const char* text, const char* end = nullptr);
};
namespace ImGui {
ImDrawList* GetForegroundDrawList();
ImVec2 CalcTextSize(const char* text, const char* end = nullptr);
ImGuiIO& GetIO();
inline void SetNextWindowPos(const ImVec2&, ImGuiCond = 0, const ImVec2& = ImVec2()) {}
inline void SetNextWindowBgAlpha(float) {}
inline bool Begin(const char*, bool* = nullptr, ImGuiWindowFlags = 0) { return false; }
inline void End() {}
inline void Separator() {}
inline void TextDisabled(const char*, ...) {}
inline void TextUnformatted(const char*, const char* = nullptr) {}
}  // namespace ImGui
