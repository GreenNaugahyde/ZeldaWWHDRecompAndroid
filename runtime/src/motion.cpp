// GamePad motion from host sensors. The sensor fusion (Mahony, with a gyro bias estimate) and the
// conversion to VPAD's axes and units follow Cemu (src/input/motion/Mahony.h, MotionHandler.h,
// MotionSample.h and the SDL controller provider; MPL-2.0).
#include "motion.h"

#include <chrono>
#include <cmath>
#include <mutex>

namespace motion {
namespace {

constexpr float kPi = 3.14159265f;

struct Quat {
    float w = 1, x = 0, y = 0, z = 0;
    Quat operator*(const Quat& r) const {
        return {w * r.w - x * r.x - y * r.y - z * r.z, w * r.x + x * r.w + y * r.z - z * r.y,
                w * r.y - x * r.z + y * r.w + z * r.x, w * r.z + x * r.y - y * r.x + z * r.w};
    }
    void normalize() {
        float s = 1.0f / std::sqrt(w * w + x * x + y * y + z * z);
        w *= s; x *= s; y *= s; z *= s;
    }
    static Quat angle_axis(float a, float ax, float ay, float az) {  // unit axis
        float s = std::sin(a * 0.5f);
        return {std::cos(a * 0.5f), ax * s, ay * s, az * s};
    }
};

struct State {
    // fusion
    Quat q{std::sqrt(0.5f), std::sqrt(0.5f), 0, 0};  // controller held tilted forward, as Cemu starts
    float roll = 0, pitch = 0, yaw = 0;
    int rollWind = 0, pitchWind = 0, yawWind = 0;
    float bias[3] = {};
    double biasSum[3] = {};
    long biasCount = 0;
    // latest sample, in the fusion's axes
    float gyro[3] = {}, acc[3] = {}, prevAcc[3] = {};
    std::chrono::steady_clock::time_point last{};
    bool valid = false;
};
State g;
std::mutex g_mu;

void update_bias(float gx, float gy, float gz) {
    if (std::fabs(gx) >= 0.35f || std::fabs(gy) >= 0.35f || std::fabs(gz) >= 0.35f) return;  // moving
    g.biasSum[0] += gx;
    g.biasSum[1] += gy;
    g.biasSum[2] += gz;
    if (++g.biasCount >= 200)
        for (int i = 0; i < 3; i++) g.bias[i] = (float)(g.biasSum[i] / (double)g.biasCount);
}

void update_angles() {
    auto wind = [](float prev, float now) {
        if (now > prev && now - prev > kPi) return -1;
        if (now < prev && prev - now > kPi) return 1;
        return 0;
    };
    const Quat& q = g.q;
    float pr = g.roll, pp = g.pitch, py = g.yaw;
    g.roll = std::atan2(2.0f * (q.z * q.w + q.x * q.y), 1.0f - 2.0f * (q.w * q.w + q.x * q.x));
    float sp = 2.0f * (q.z * q.x - q.y * q.w);
    g.pitch = std::fabs(sp) >= 1.0f ? std::copysign(kPi / 2, sp) : std::asin(sp);
    g.yaw = std::atan2(2.0f * (q.z * q.y + q.w * q.x), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    g.rollWind += wind(pr, g.roll);
    g.pitchWind += wind(pp, g.pitch);
    g.yawWind += wind(py, g.yaw);
}

// Mahony update with the gyro in rad/s and the acceleration in any unit
void fuse(float dt, float gx, float gy, float gz, float ax, float ay, float az) {
    if (dt > 0.2f) dt = 0.2f;
    update_bias(gx, gy, gz);
    float v[3] = {gx - g.bias[0], gy - g.bias[1], gz - g.bias[2]};
    for (float& c : v)
        if (std::fabs(c) < 0.015f) c = 0;  // small rates are noise; keeps yaw from drifting
    float n = std::sqrt(ax * ax + ay * ay + az * az);
    if (n > 1e-6f) {
        ax /= n; ay /= n; az /= n;
        const Quat& q = g.q;
        // the gravity direction the current orientation predicts (half scale), crossed with the measured one
        float gr[3] = {(q.x * q.z - q.w * q.y), (q.y * q.z + q.w * q.x), 0.5f * (2.0f * (q.w * q.w + q.z * q.z) - 1.0f)};
        v[0] -= gr[1] * az - gr[2] * ay;
        v[1] -= gr[2] * ax - gr[0] * az;
        v[2] -= gr[0] * ay - gr[1] * ax;
    }
    for (float& c : v) c *= 0.5f * dt;
    Quat d = g.q * Quat{0, v[0], v[1], v[2]};
    g.q.w += d.w; g.q.x += d.x; g.q.y += d.y; g.q.z += d.z;
    g.q.normalize();
    update_angles();
}

// the attitude matrix's row from a quaternion (VPAD's mixed-handedness axes, as Cemu derives them)
void x_vector(const Quat& q, float* o) {
    o[0] = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    o[2] = 2.0f * (q.x * q.y + q.z * q.w);
    o[1] = 2.0f * (q.x * q.z - q.y * q.w);
}

float rev(float rad) { return rad / (2.0f * kPi); }

}  // namespace

void sample(float dt, float gx, float gy, float gz, float ax, float ay, float az) {
    std::lock_guard<std::mutex> lk(g_mu);
    // into the fusion's axes as Cemu's SDL provider passes them (acceleration in g)
    float acc[3] = {-ax / 9.81f, ay / 9.81f, az / 9.81f};
    float gyro[3] = {gx, -gy, -gz};
    for (int i = 0; i < 3; i++) {
        g.prevAcc[i] = g.valid ? g.acc[i] : acc[i];
        g.acc[i] = acc[i];
        g.gyro[i] = gyro[i];
    }
    fuse(dt, gyro[0], gyro[1], gyro[2], acc[0], acc[1], acc[2]);
    g.last = std::chrono::steady_clock::now();
    g.valid = true;
}

void reset() {
    std::lock_guard<std::mutex> lk(g_mu);
    g = State{};
}

bool vpad(Vpad& o) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g.valid || std::chrono::steady_clock::now() - g.last > std::chrono::milliseconds(500)) return false;
    o.acc[0] = -g.acc[0];
    o.acc[1] = -g.acc[1];
    o.acc[2] = g.acc[2];
    o.accMagnitude = std::sqrt(g.acc[0] * g.acc[0] + g.acc[1] * g.acc[1] + g.acc[2] * g.acc[2]);
    float da[3] = {g.acc[0] - g.prevAcc[0], g.acc[1] - g.prevAcc[1], g.acc[2] - g.prevAcc[2]};
    o.accAcceleration = std::sqrt(da[0] * da[0] + da[1] * da[1] + da[2] * da[2]);
    if (o.accMagnitude > 1e-6f) {
        float n[3] = {g.acc[0] / o.accMagnitude, g.acc[1] / o.accMagnitude, g.acc[2] / o.accMagnitude};
        o.accXY[0] = std::sqrt(n[0] * n[0] + n[1] * n[1]);
        // atan2(-X, sqrt(Y²+Z²)) with X = -n.z, Y = n.x, Z = -n.y
        o.accXY[1] = -std::sin(std::atan2(n[2], std::sqrt(n[0] * n[0] + n[1] * n[1])));
    } else {
        o.accXY[0] = 1.0f;
        o.accXY[1] = 0.0f;
    }
    float gb[3] = {g.gyro[0] - g.bias[0], g.gyro[1] - g.bias[1], g.gyro[2] - g.bias[2]};
    o.gyro[0] = rev(-gb[0]);
    o.gyro[1] = rev(-gb[1]);
    o.gyro[2] = rev(gb[2]);
    o.angle[0] = rev(-(g.yaw + g.yawWind * 2.0f * kPi)) - 0.5f;
    o.angle[1] = rev(-(g.pitch + g.pitchWind * 2.0f * kPi)) - 0.5f;
    o.angle[2] = rev(g.roll + g.rollWind * 2.0f * kPi);
    x_vector(g.q, o.dir + 0);
    x_vector(g.q * Quat::angle_axis(kPi / 2, 0, 0, 1), o.dir + 3);
    x_vector(g.q * Quat::angle_axis(kPi / 2, 0, 1, 0), o.dir + 6);
    return true;
}

}  // namespace motion
