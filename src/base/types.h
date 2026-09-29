#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef size_t   usize;

#define MX_ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MX_LIKELY(x)   __builtin_expect(!!(x), 1)
#define MX_UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifdef MX_DEBUG
#define MX_ASSERT(x) do { if (MX_UNLIKELY(!(x))) __builtin_trap(); } while (0)
#else
#define MX_ASSERT(x) ((void)0)
#endif

template <class T> static inline T mx_min(T a, T b) { return a < b ? a : b; }
template <class T> static inline T mx_max(T a, T b) { return a > b ? a : b; }
template <class T> static inline T mx_clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
