#pragma once

// The app's 60 fps setting (fps60.cpp): 0 off (30 fps), 1 variable 30-60 (frame interpolation),
// 2 adaptive (60, back to 30 while the device can't hold it), 3 true 60 (experimental),
// 4 40 fps (120 Hz displays: 3 refreshes per frame)
namespace fps60 {
void set_mode(int m);
int mode();
void on_swap();  // called by the game thread at each GX2 swap
}  // namespace fps60
