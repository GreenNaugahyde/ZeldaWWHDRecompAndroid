// CoreAudio output (default output AudioUnit) pulling from a single-producer ring buffer.
// WWHD_AUDIO_DUMP=file.wav additionally records everything pushed by the game.
// WWHD_NO_AUDIO=1 skips opening the device (the mix still runs and can be dumped).
#include "audio_out.h"

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "runtime.h"

namespace audio {
namespace {

constexpr int kCapacity = 1 << 15;  // frames (~680 ms)
constexpr int kTarget = kRate * 40 / 1000;

int16_t g_ring[kCapacity * 2];
std::atomic<uint32_t> g_read{0}, g_write{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_flush{false};  // consumer skips everything queued
AudioComponentInstance g_unit = nullptr;

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

OSStatus render(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
    auto* out = (int16_t*)io->mBuffers[0].mData;
    uint32_t r = g_read.load(std::memory_order_relaxed), w = g_write.load(std::memory_order_acquire);
    if (g_flush.exchange(false)) r = w;
    uint32_t avail = w - r, n = std::min<uint32_t>(avail, frames);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (r + i) & (kCapacity - 1);
        out[i * 2] = g_ring[idx * 2];
        out[i * 2 + 1] = g_ring[idx * 2 + 1];
    }
    if (n < frames) {  // underrun: silence
        memset(out + n * 2, 0, (frames - n) * 4);
        g_underrun += frames - n;
    }
    g_read.store(r + n, std::memory_order_release);
    return noErr;
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

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &g_unit) != noErr) {
        LOG("[audio] no output device");
        g_unit = nullptr;
        return;
    }
    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = kRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 16;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 4;
    fmt.mBytesPerPacket = 4;
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
    AURenderCallbackStruct cb{render, nullptr};
    AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    // debug: WWHD_AUDIO_VOLUME=0..1 scales the device volume (e.g. silent tests of the real output path)
    if (const char* v = getenv("WWHD_AUDIO_VOLUME"))
        AudioUnitSetParameter(g_unit, kHALOutputParam_Volume, kAudioUnitScope_Global, 0, (AudioUnitParameterValue)atof(v), 0);
    if (AudioUnitInitialize(g_unit) != noErr || AudioOutputUnitStart(g_unit) != noErr) {
        LOG("[audio] failed to start output unit");
        return;
    }
    LOG("[audio] CoreAudio output started (48 kHz stereo)");
}

void push(const int16_t* stereo, int frames) {
    if (g_dump) {
        fwrite(stereo, 4, frames, g_dump);
        g_dump_frames += frames;
        if (g_dump_frames % kRate < (uint32_t)frames) write_wav_header();  // keep the file valid about once a second
    }
    if (!g_unit) return;
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

int buffered_frames() {
    if (!g_unit) return kTarget;
    return (int)(g_write.load(std::memory_order_acquire) - g_read.load(std::memory_order_acquire));
}

int target_frames() { return kTarget; }

void flush() { g_flush = true; }

void stats(uint64_t& underrun, uint64_t& dropped) {
    underrun = g_underrun;
    dropped = g_dropped;
}

}  // namespace audio
