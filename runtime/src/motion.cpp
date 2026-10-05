// GamePad motion for the game's gyro aiming: the host's gyroscope is integrated into the GamePad's
// orientation (VPADStatus direction matrix) and accumulated angle; the angular rate and the
// acceleration are passed through. Units as the VPAD library reports them: rate in revolutions
// per second, angle in revolutions, acceleration in g.
// Recalibrating makes the current orientation the identity ("held straight ahead"); there is no
// drift correction, so a long session may need a recalibration.
#include "motion.h"

#include <atomic>
#include <cmath>
#include <mutex>

#include "runtime.h"

namespace motion {
namespace {
std::mutex g_m;
std::atomic<bool> g_on{false};
float g_q[4] = {1, 0, 0, 0};  // orientation (w, x, y, z) relative to the calibration
float g_rate[3], g_acc[3] = {0, 0, 0}, g_angle[3];
int64_t g_last_t = 0;
constexpr float kTwoPi = 6.28318530718f, kG = 9.80665f;

void normalize(float q[4]) {
    float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n <= 0) { q[0] = 1; q[1] = q[2] = q[3] = 0; return; }
    for (int i = 0; i < 4; i++) q[i] /= n;
}
}  // namespace

void push(float gx, float gy, float gz, float ax, float ay, float az, int64_t t_ns) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_m);
    float dt = g_last_t ? (float)((t_ns - g_last_t) * 1e-9) : 0.f;
    g_last_t = t_ns;
    if (dt < 0 || dt > 0.1f) dt = 0;  // first sample, or a gap (paused): don't jump
    g_rate[0] = gx; g_rate[1] = gy; g_rate[2] = gz;
    g_acc[0] = ax / kG; g_acc[1] = ay / kG; g_acc[2] = az / kG;
    for (int i = 0; i < 3; i++) g_angle[i] += g_rate[i] * dt / kTwoPi;
    // q = q * (rotation by w*dt), in the GamePad's own frame
    float hx = gx * dt * 0.5f, hy = gy * dt * 0.5f, hz = gz * dt * 0.5f;
    float w = g_q[0], x = g_q[1], y = g_q[2], z = g_q[3];
    g_q[0] = w - x * hx - y * hy - z * hz;
    g_q[1] = x + w * hx + y * hz - z * hy;
    g_q[2] = y + w * hy - x * hz + z * hx;
    g_q[3] = z + w * hz + x * hy - y * hx;
    normalize(g_q);
}

void set_enabled(bool on) {
    g_on = on;
    recalibrate();
    LOG("[motion] gyro %s", on ? "on" : "off");
}

void recalibrate() {
    std::lock_guard<std::mutex> lk(g_m);
    g_q[0] = 1; g_q[1] = g_q[2] = g_q[3] = 0;
    for (int i = 0; i < 3; i++) g_rate[i] = g_angle[i] = 0;
    g_acc[0] = g_acc[1] = g_acc[2] = 0;
    g_last_t = 0;
}

void fill(uint32_t st) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    float q[4], rate[3], acc[3], angle[3];
    {
        std::lock_guard<std::mutex> lk(g_m);
        for (int i = 0; i < 4; i++) q[i] = g_q[i];
        for (int i = 0; i < 3; i++) { rate[i] = g_rate[i]; acc[i] = g_acc[i]; angle[i] = g_angle[i]; }
    }
    float mag = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    for (int i = 0; i < 3; i++) stf32(st + 0x1C + i * 4, acc[i]);
    stf32(st + 0x28, mag);
    for (int i = 0; i < 3; i++) stf32(st + 0x38 + i * 4, rate[i] / kTwoPi);
    for (int i = 0; i < 3; i++) stf32(st + 0x44 + i * 4, angle[i]);
    // direction: the GamePad's x, y and z axes in the calibrated frame (rows of the rotation matrix)
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                     {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                     {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) stf32(st + 0x6C + r * 0xC + k * 4, m[r][k]);
}
}  // namespace motion
