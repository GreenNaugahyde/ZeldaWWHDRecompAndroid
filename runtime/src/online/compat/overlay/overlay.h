// Online co-op compat (../README.md): the overlay's input capture and panels (none on Android: the app's own views).
#pragma once
namespace overlay {
inline bool captures() { return false; }
inline void show_panel(bool (*)(const float*)) {}
}  // namespace overlay
