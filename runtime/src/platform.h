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
// (macOS QoS user-interactive; Android: a raised nice value). WWHD_NO_QOS=1 skips it.
void set_thread_high_priority();
// Android: run the calling thread on the CPU cluster with the highest clock (the "prime" core of
// big.LITTLE phones), for the render thread, the usual bottleneck. Call again from time to time: it
// restores the affinity if the system changed it, and gives inherited affinities back to other
// threads. WWHD_NO_PRIME_CORE=1 skips it.
void set_thread_fastest_cores();
// bounds of the calling thread's stack: [lo, hi)
void thread_stack_bounds(uintptr_t& lo, uintptr_t& hi);
// CPU time consumed by a thread so far, in microseconds (0 if unavailable)
uint64_t thread_cpu_us(pthread_t t);

// reserve `size` bytes of zero-filled read/write memory at exactly `addr`; false if that range is taken
bool map_fixed(void* addr, size_t size);

// write a line to the platform log (stderr; Android: logcat as well)
void log_line(const char* line);

// print a host backtrace of the calling thread to stderr (async-signal-safe enough for the crash handler)
void print_backtrace();

}  // namespace platform
