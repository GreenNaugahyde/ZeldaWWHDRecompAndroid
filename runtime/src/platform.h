// Host OS services that differ between macOS and Android (Linux).
#pragma once
#include <pthread.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace platform {

// name of the calling thread, as shown by debuggers and profilers (truncated to 15 characters on Linux)
void set_thread_name(const char* name);
// game, render and service threads: ask the OS to keep them on fast cores
// (macOS QoS user-interactive; Android: a raised nice value and the performance hint session).
// WWHD_NO_QOS=1 skips the priority.
void set_thread_high_priority();
// game and render thread: kept on the big cores where the scheduler places threads by load alone
// (no performance hint session: Android 12 and older, or the device refuses one). Without the
// session's guidance, a lighter render thread or the game thread drifts to the little cores (Snapdragon 855:
// render thread 10.6 ms a frame on big cores, 21 ms as scheduled). WWHD_BIG_CORES=0 never, 1 only
// without a session (default), 2 always.
// (game: the game's main thread, which frame interpolation also keeps on the fastest cores; see
// game_fast_cores in platform.cpp)
void prefer_big_cores(bool game);
// once per frame shown (render thread): reports the frame's CPU work of the game, render and present
// threads to Android's performance hint session (no-op elsewhere); framesPerStep: frames shown per
// 30 Hz game step (frame interpolation: 2 at 60 fps), which divides the target
void perf_hint_frame(int framesPerStep = 1);
// bounds of the calling thread's stack: [lo, hi)
void thread_stack_bounds(uintptr_t& lo, uintptr_t& hi);
// CPU time consumed by a thread so far, in microseconds (0 if unavailable)
uint64_t thread_cpu_us(pthread_t t);

// reserve `size` bytes of zero-filled read/write memory at exactly `addr`; false if that range is taken
bool map_fixed(void* addr, size_t size);

// write a line to the platform log (stderr; Android: logcat as well)
void log_line(const char* line);

// the process's memory mappings (and the system's limit), threads and memory, as one line into buf;
// async-signal-safe (reads /proc with plain syscalls). Issue #30: a GPU driver failed to mprotect a
// new thread's stack with "out of memory", which may be the mapping limit rather than RAM.
int process_numbers(char* buf, int size);

// print a host backtrace of the calling thread to stderr (async-signal-safe enough for the crash handler)
void print_backtrace(int fd = -1);  // also into fd (a crash log) when >= 0

}  // namespace platform
