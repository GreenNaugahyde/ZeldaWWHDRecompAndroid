// AAudio output (Android 8+) pulling from a single-producer ring buffer, like the CoreAudio backend.
// WWHD_AUDIO_DUMP=file.wav additionally records everything pushed by the game.
// WWHD_NO_AUDIO=1 skips opening the device (the mix still runs and can be dumped).
#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "audio_out.h"
#include "runtime.h"

namespace audio {
namespace {

constexpr int kCapacity = 1 << 15;  // frames (~680 ms)
// Android output paths buffer more than CoreAudio; a little more headroom avoids underruns when
// the device asks for its larger bursts
constexpr int kTarget = kRate * 60 / 1000;

int16_t g_ring[kCapacity * 2];
std::atomic<uint32_t> g_read{0}, g_write{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_active{false};  // a stream exists and consumes the ring
std::atomic<bool> g_flush{false};   // consumer skips everything queued (a save state was loaded)
std::mutex g_stream_mutex;
AAudioStream* g_stream = nullptr;
float g_volume = 1.0f;

std::atomic<uint64_t> g_underrun{0}, g_dropped{0};

FILE* g_dump = nullptr;
uint32_t g_dump_frames = 0;

void write_wav_header() {
    uint32_t data = g_dump_frames * 4;
    uint8_t h[44];
    auto u32 = [&](int o, uint32_t v) { memcpy(h + o, &v, 4); };
    auto u16 = [&](int o, uint16_t v) { memcpy(h + o, &v, 2); };
    memcpy(h, "RIFF", 4);
    u32(4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    u32(16, 16);
    u16(20, 1);
    u16(22, 2);
    u32(24, kRate);
    u32(28, kRate * 4);
    u16(32, 4);
    u16(34, 16);
    memcpy(h + 36, "data", 4);
    u32(40, data);
    long pos = ftell(g_dump);
    fseek(g_dump, 0, SEEK_SET);
    fwrite(h, 1, 44, g_dump);
    fseek(g_dump, pos, SEEK_SET);
    fflush(g_dump);
}

aaudio_data_callback_result_t render(AAudioStream*, void*, void* data, int32_t frames) {
    auto* out = (int16_t*)data;
    uint32_t r = g_read.load(std::memory_order_relaxed), w = g_write.load(std::memory_order_acquire);
    if (g_flush.exchange(false)) r = w;
    uint32_t avail = w - r, n = std::min<uint32_t>(avail, (uint32_t)frames);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (r + i) & (kCapacity - 1);
        out[i * 2] = g_ring[idx * 2];
        out[i * 2 + 1] = g_ring[idx * 2 + 1];
    }
    if (g_volume != 1.0f)
        for (uint32_t i = 0; i < n * 2; i++) out[i] = (int16_t)(out[i] * g_volume);
    if (n < (uint32_t)frames) {  // underrun: silence
        memset(out + n * 2, 0, (frames - n) * 4);
        g_underrun += frames - n;
    }
    g_read.store(r + n, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void open_stream();

// the device went away (headphones unplugged, Bluetooth switch): reopen on another thread, as
// AAudio requires
void on_error(AAudioStream*, void*, aaudio_result_t err) {
    if (err != AAUDIO_ERROR_DISCONNECTED) return;
    std::thread([] {
        std::lock_guard<std::mutex> lk(g_stream_mutex);
        if (g_stream) {
            AAudioStream_close(g_stream);
            g_stream = nullptr;
        }
        g_active = false;
        LOG("[audio] output disconnected, reopening");
        open_stream();
    }).detach();
}

void open_stream() {  // g_stream_mutex held
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) {
        LOG("[audio] AAudio unavailable");
        return;
    }
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, render, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, on_error, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        LOG("[audio] cannot open output stream: %s", AAudio_convertResultToText(r));
        g_stream = nullptr;
        return;
    }
    // the device may run at another rate (AAudio resamples since Android 10); the burst size tells
    // how much it pulls at a time
    int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
    AAudioStream_setBufferSizeInFrames(g_stream, burst * 2);
    if ((r = AAudioStream_requestStart(g_stream)) != AAUDIO_OK) {
        LOG("[audio] cannot start output stream: %s", AAudio_convertResultToText(r));
        AAudioStream_close(g_stream);
        g_stream = nullptr;
        return;
    }
    g_active = true;
    LOG("[audio] AAudio output started (%d Hz stereo, burst %d frames)", AAudioStream_getSampleRate(g_stream), burst);
}

}  // namespace

void init() {
    if (g_started.exchange(true)) return;
    if (const char* p = getenv("WWHD_AUDIO_DUMP")) {
        g_dump = fopen(p, "wb");
        if (g_dump) {
            write_wav_header();
            fseek(g_dump, 44, SEEK_SET);
        }
    }
    if (getenv("WWHD_NO_AUDIO")) return;
    // debug: WWHD_AUDIO_VOLUME=0..1 scales the output (e.g. silent tests of the real output path)
    if (const char* v = getenv("WWHD_AUDIO_VOLUME")) g_volume = std::clamp((float)atof(v), 0.0f, 1.0f);
    std::lock_guard<std::mutex> lk(g_stream_mutex);
    open_stream();
}

void push(const int16_t* stereo, int frames) {
    if (g_dump) {
        fwrite(stereo, 4, frames, g_dump);
        g_dump_frames += frames;
        if (g_dump_frames % kRate < (uint32_t)frames) write_wav_header();  // keep the file valid about once a second
    }
    if (!g_active) return;
    uint32_t w = g_write.load(std::memory_order_relaxed), r = g_read.load(std::memory_order_acquire);
    if (w - r + frames > kCapacity) {  // device stalled: drop rather than overwrite
        g_dropped += frames;
        return;
    }
    for (int i = 0; i < frames; i++) {
        uint32_t idx = (w + i) & (kCapacity - 1);
        g_ring[idx * 2] = stereo[i * 2];
        g_ring[idx * 2 + 1] = stereo[i * 2 + 1];
    }
    g_write.store(w + frames, std::memory_order_release);
}

// without a device the AX thread paces itself as if the queue sat at its target
int buffered_frames() {
    if (!g_active) return kTarget;
    return (int)(g_write.load(std::memory_order_acquire) - g_read.load(std::memory_order_acquire));
}

int target_frames() { return kTarget; }

void flush() { g_flush = true; }

void stats(uint64_t& underrun, uint64_t& dropped) {
    underrun = g_underrun;
    dropped = g_dropped;
}

void set_paused(bool paused) {
    std::lock_guard<std::mutex> lk(g_stream_mutex);
    if (!g_stream) return;
    if (paused) {
        AAudioStream_requestPause(g_stream);
        g_active = false;
        g_read.store(g_write.load());  // drop what was queued; it would play late on resume
    } else {
        g_read.store(g_write.load());
        AAudioStream_requestStart(g_stream);
        g_active = true;
    }
}

}  // namespace audio
