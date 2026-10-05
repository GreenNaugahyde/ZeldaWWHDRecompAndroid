#pragma once

// Display vsync timestamps for the guest vsync clock (display_vsync.cpp)
namespace display_vsync {
void start();  // starts the Choreographer thread once
}  // namespace display_vsync
