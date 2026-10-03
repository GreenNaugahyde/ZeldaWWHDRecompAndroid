// Host audio output: 48 kHz stereo s16 (CoreAudio on macOS, AAudio on Android), fed by the AX frame thread.
#pragma once
#include <cstdint>

namespace audio {

constexpr int kRate = 48000;

void init();                                    // opens the default output device (safe to call twice)
void push(const int16_t* stereo, int frames);   // interleaved L/R
int buffered_frames();                          // frames queued for the device
int target_frames();                            // latency the producer should aim for
void stats(uint64_t& underrun, uint64_t& dropped);  // frames of silence inserted / frames discarded
void set_paused(bool paused);                   // app in the background: stop the device (the mix keeps running)
void flush();                                   // drop what is queued (a save state was loaded)

}  // namespace audio
