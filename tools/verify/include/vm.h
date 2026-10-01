/* Verification harness: guest memory model and call interception.
 *
 * Both sides of a test go through these functions:
 *   - the recompiled original (build/gen code, compiled with vm_gen.h so that every
 *     ld/st/psq/dcbz is a vm_* call and every call to another guest function is vm_call);
 *   - the candidate C++ source (gabi.h: guest-memory field types and guest calls).
 *
 * Memory is sparse and lazily materialised: the first access to a byte takes its value from
 * the input (recorded snapshot, scripted fields, the RPX image, or deterministic random fill),
 * so any pointer the code follows is valid and both sides see identical contents. Every
 * write is tracked so the net effect of a run can be compared byte for byte.
 */
#pragma once
#include <stdint.h>

#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

uint8_t vm_ld8(uint32_t ea);
uint16_t vm_ld16(uint32_t ea);
uint32_t vm_ld32(uint32_t ea);
uint64_t vm_ld64(uint32_t ea);
void vm_st8(uint32_t ea, uint8_t v);
void vm_st16(uint32_t ea, uint16_t v);
void vm_st32(uint32_t ea, uint32_t v);
void vm_st64(uint32_t ea, uint64_t v);

/* a call from the code under test to guest function `target` (direct, indirect or import).
 * Records the call and runs the callee model (mock, recorded result, or real code). */
void vm_call(Cpu* c, uint32_t target, int kind);
enum { VM_CALL_DIRECT = 0, VM_CALL_INDIRECT = 1, VM_CALL_IMPORT = 2 };

/* basic-block coverage of the original (block ids per unit function) */
void vm_cov(uint32_t func, uint32_t block);

/* traps / unimplemented instructions reached by the original */
void vm_trap(Cpu* c, uint32_t addr, uint32_t insn);

#ifdef __cplusplus
}
#endif
