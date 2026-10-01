// shim
#pragma once
struct LattePerfTimer { void beginMeasuring() {} void endMeasuring() {} };
struct LattePerfMonitor { LattePerfTimer gpuTime_shaderCreate; };
inline LattePerfMonitor performanceMonitor;
