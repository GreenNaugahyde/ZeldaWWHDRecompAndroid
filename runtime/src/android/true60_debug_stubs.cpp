// The true 60 debug dumps (true60_test.cpp in the original project: comparisons against the 30 fps
// game, desktop only) are not part of the Android build; true60.cpp asks them whether to dump.
#include <cstdint>

namespace true60_test {
bool dumping() { return false; }
uint64_t origin_step() { return 0; }
void before_execute_link(uint32_t) {}
void after_execute(uint32_t, uint32_t, bool, float) {}
}  // namespace true60_test
