#pragma once
#include <cstdint>

// GamePad motion (gyro aiming) from the host's sensors (motion.cpp)
namespace motion {
// a sensor sample in GamePad axes (x right, y up, z out of the screen): angular velocity in rad/s,
// acceleration in m/s^2 as the host reports it, timestamp in ns
void push(float gx, float gy, float gz, float ax, float ay, float az, int64_t t_ns);
void set_enabled(bool on);  // off: the GamePad reads as lying still (the old behaviour)
void recalibrate();         // the current orientation becomes "straight ahead"
// writes the VPADStatus motion fields: acc 0x1C, gyro 0x38, angle 0x44, direction 0x6C
void fill(uint32_t status);
}  // namespace motion
