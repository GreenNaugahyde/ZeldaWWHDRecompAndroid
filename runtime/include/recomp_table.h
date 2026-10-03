/* The recompiled game's function and import tables: emitted by tools/recomp/recomp.py
 * (build/gen/table.c), or filled at startup when the game code was recompiled on the device and
 * is loaded from the code cache (runtime/src/recomp/loader.cpp). */
#pragma once
#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint32_t addr; PpcFunc fn; } RecompEntry;
typedef struct { uint32_t slot; uint32_t addr; const char* lib; const char* name; int is_func; PpcFunc fn; } RecompImport;

extern const RecompEntry* g_recomp_funcs;
extern unsigned g_recomp_func_count;
extern const RecompImport* g_recomp_imports;
extern unsigned g_recomp_import_count;
extern uint32_t g_recomp_entry_point;

#ifdef __cplusplus
}
#endif
