/* Tables emitted by tools/recomp/recomp.py (build/gen/table.c). */
#pragma once
#include "ppc.h"

typedef struct { uint32_t addr; PpcFunc fn; } RecompEntry;
typedef struct { uint32_t slot; uint32_t addr; const char* lib; const char* name; int is_func; PpcFunc fn; } RecompImport;

extern const RecompEntry g_recomp_funcs[];
extern const unsigned g_recomp_func_count;
extern const RecompImport g_recomp_imports[];
extern const unsigned g_recomp_import_count;
extern const uint32_t g_recomp_entry_point;
