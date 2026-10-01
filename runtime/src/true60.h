// True 60 fps (game logic at 60 steps per second): see true60.cpp.
#pragma once
#include <cstdint>

struct Cpu;

namespace true60 {
bool enabled();
void set_enabled(bool v);
float dt();              // step length of the execute in progress: 1 = one original 30 Hz step
uint32_t exec_proc();    // process whose execute is in progress (0 outside)
bool half_pass();        // this pass only runs 60 Hz processes
int32_t split(int32_t v);  // integer per-step amount for this step (half steps add up to v)
void new_pass();
uint64_t pass();
bool runs_60(uint32_t proc);  // the process executed at 60 Hz on this pass (not interpolated)
bool drawing_60();            // the process being drawn runs at 60 Hz on this pass
void force_draw_60(bool on);  // nested scope: drawing_60() is true (drawing built from 60 Hz state)
uint32_t link();              // Link's process once seen (0 before)
void ss_reset();              // a save state was loaded: forget processes and histories
// instruction-level hooks for per-step smoothing (tools/true60/gen_sites.py): the ratio r in
// fR becomes 1-(1-r)^dt for one instruction (begin/end) or for a call argument (arg)
void ratio_begin(Cpu* c, int r);
void ratio_end(Cpu* c, int r);
void ratio_arg(Cpu* c, int r);
}  // namespace true60
