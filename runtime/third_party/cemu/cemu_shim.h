// Minimal replacements for Cemu's common definitions used by the vendored files.
// Force-included when compiling files under third_party/cemu.
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>
#include <atomic>
#include <bitset>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint8 = int8_t;
using sint16 = int16_t;
using sint32 = int32_t;
using sint64 = int64_t;
using MPTR = uint32_t;

#define cemu_assert(x) assert(x)
#define cemu_assert_debug(x) ((void)0)
#define cemu_assert_suspicious() ((void)0)
#define cemu_assert_unimplemented() ((void)0)
#define cemu_assert_error() assert(false)
#define assert_dbg() ((void)0)
#define _CRLF "\n"

inline uint16 _swapEndianU16(uint16 v) { return __builtin_bswap16(v); }
inline uint32 _swapEndianU32(uint32 v) { return __builtin_bswap32(v); }
inline uint64 _swapEndianU64(uint64 v) { return __builtin_bswap64(v); }
inline sint32 _swapEndianS32(sint32 v) { return (sint32)__builtin_bswap32((uint32)v); }

#include "Common/betype.h"
#include "Common/enumFlags.h"
using DWORD = uint32_t;

#define DEFINE_ENUM_FLAG_OPERATORS(T)                                                                                          \
    inline T operator~(T a) { return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a)); }                              \
    inline T operator|(T a, T b) { return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b)); } \
    inline T operator&(T a, T b) { return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b)); } \
    inline T operator^(T a, T b) { return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b)); } \
    inline T& operator|=(T& a, T b) { return a = a | b; }                                                                      \
    inline T& operator&=(T& a, T b) { return a = a & b; }                                                                      \
    inline T& operator^=(T& a, T b) { return a = a ^ b; }

inline unsigned char _BitScanReverse(DWORD* index, uint32_t mask) {
    if (!mask) return 0;
    *index = 31 - __builtin_clz(mask);
    return 1;
}

template <typename T>
inline T GetBits(T value, uint32 index, uint32 numBits) { return (value >> index) & ((1 << numBits) - 1); }

#define FMT_HEADER_ONLY 1
#include <fmt/format.h>
#include <fmt/ranges.h>

#define MPTR_NULL (0)

// logging
enum class LogType { Force, APIErrors, Shader };
void cemu_shim_log(const std::string& msg);
template <typename... Args>
inline void cemuLog_log(LogType, fmt::format_string<Args...> fmtstr, Args&&... args) { cemu_shim_log(fmt::format(fmtstr, std::forward<Args>(args)...)); }
inline void cemuLog_log(LogType, std::string_view msg) { cemu_shim_log(std::string(msg)); }
template <typename... Args>
inline void cemuLog_logDebug(LogType, fmt::format_string<Args...>, Args&&...) {}
#define debugBreakpoint() ((void)0)
#define debug_printf(...) ((void)0)
#define cemuLog_logOnce cemuLog_log
#define cemuLog_logDebugOnce cemuLog_logDebug

// GPU state fields queried by the decompiler (OpenGL-vendor quirks; never AMD here)
enum { GLVENDOR_UNKNOWN = 0, GLVENDOR_AMD = 1, GLVENDOR_NVIDIA = 2, GLVENDOR_INTEL = 3, GLVENDOR_APPLE = 4 };
struct LatteGPUStateShim { int glVendor = GLVENDOR_APPLE; };
inline LatteGPUStateShim LatteGPUState;

// generic formatter for enums (to underlying), as in Cemu's precompiled.h
template <typename Enum>
    requires std::is_enum_v<Enum>
struct fmt::formatter<Enum> : fmt::formatter<std::underlying_type_t<Enum>> {
    auto format(const Enum& e, format_context& ctx) const {
        return fmt::formatter<std::underlying_type_t<Enum>>::format(fmt::underlying(e), ctx);
    }
};
template <typename T>
struct fmt::formatter<betype<T>> : fmt::formatter<T> {
    auto format(const betype<T>& e, format_context& ctx) const { return fmt::formatter<T>::format(static_cast<T>(e), ctx); }
};
