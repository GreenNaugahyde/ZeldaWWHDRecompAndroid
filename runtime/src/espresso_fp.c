/* Bit-exact Espresso fres / frsqrte estimates.
 *
 * Lookup tables and algorithm from Cemu (src/Cafe/HW/Espresso/Interpreter/PPCInterpreterFPU.cpp),
 * Copyright (c) Cemu contributors, licensed under the Mozilla Public License 2.0.
 */
#include <stdint.h>
#include <string.h>

#include "ppc.h"

typedef struct { uint32_t offset, step; } Entry;

static const Entry frsqrte_table[32] = {
    {0x1a7e800, 0x568}, {0x17cb800, 0x4f3}, {0x1552800, 0x48d}, {0x130c000, 0x435},
    {0x10f2000, 0x3e7}, {0xeff000, 0x3a2},  {0xd2e000, 0x365},  {0xb7c000, 0x32e},
    {0x9e5000, 0x2fc},  {0x867000, 0x2d0},  {0x6ff000, 0x2a8},  {0x5ab800, 0x283},
    {0x46a000, 0x261},  {0x339800, 0x243},  {0x218800, 0x226},  {0x105800, 0x20b},
    {0x3ffa000, 0x7a4}, {0x3c29000, 0x700}, {0x38aa000, 0x670}, {0x3572000, 0x5f2},
    {0x3279000, 0x584}, {0x2fb7000, 0x524}, {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e},
    {0x2881000, 0x43a}, {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e},
    {0x20c1000, 0x35e}, {0x1f12000, 0x332}, {0x1d79000, 0x30a}, {0x1bf4000, 0x2e6},
};

static const Entry fres_table[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
    {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
    {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
    {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0xfbe00, 0x143},  {0xd3800, 0x12d},  {0xade00, 0x12d},
    {0x88400, 0x11a},  {0x65000, 0x11a},  {0x41c00, 0x108},  {0x20c00, 0x106},
};

double ppc_frsqrte(double input) {
    uint64_t x = f64_as_u64(input);
    if ((x << 1) == 0) return u64_as_f64((x & 0x8000000000000000ull) | 0x7FF0000000000000ull);
    uint32_t e = (uint32_t)(x >> 52) & 0x7FF;
    if (e == 0x7FF) {
        if ((x & 0xFFFFFFFFFFFFFull) == 0) return (int64_t)x < 0 ? u64_as_f64(0x7FF8000000000000ull) : 0.0;
        return input;
    }
    if ((int64_t)x < 0) return u64_as_f64(0x7FF8000000000000ull);
    uint32_t idx = (uint32_t)(x >> 48) & 0x1F;
    uint32_t step_mul = (uint32_t)(x >> 37) & 0x7FF;
    int32_t sum = (int32_t)(frsqrte_table[idx].offset - frsqrte_table[idx].step * step_mul);
    e = 1023 - ((e - 1021) >> 1);
    x &= ~(0x7FFull << 52);
    x |= (uint64_t)e << 52;
    x &= ~0xFFFFFFFFFFFFFull;
    x += (uint64_t)(uint32_t)sum << 26;
    return u64_as_f64(x);
}

double ppc_fres(double input) {
    uint64_t x = f64_as_u64(input);
    uint32_t idx = (uint32_t)(x >> 47) & 0x1F;
    uint32_t step_mul = (uint32_t)(x >> 37) & 0x3FF;
    uint32_t sum = fres_table[idx].offset - (fres_table[idx].step * step_mul + 1) / 2;
    uint32_t e = (uint32_t)(x >> 52) & 0x7FF;
    if (e == 0) return u64_as_f64(x | 0x7FF0000000000000ull);
    if (e == 0x7FF) {
        if ((x & 0xFFFFFFFFFFFFFull) == 0) return (int64_t)x < 0 ? u64_as_f64(0x8000000000000000ull) : 0.0;
        return input;
    }
    e = 2045 - e;
    x &= ~(0x7FFull << 52);
    x |= (uint64_t)e << 52;
    x &= ~0xFFFFFFFFFFFFFull;
    x += (uint64_t)sum << 29;
    return u64_as_f64(x);
}
