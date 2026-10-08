// What the crash log (main.cpp) says besides the crash itself: the app and device, the game
// release, the settings and the options changed while playing. Kept as prepared text, since the
// crash handler runs in a signal handler and may not allocate. No personal data: paths are cut to
// their last part.
#pragma once
#include <string>

namespace crash_info {

// sets one section ("app", "game", "options", ...), replacing its earlier text (any thread; not
// from a signal handler). Lines in `text` are separated by '\n'.
void set(const std::string& section, const std::string& text);
// one option of the "options" section (setOption from the app's menu)
void option(const std::string& name, int value);
// WWHD_*, TU_*, MESA_* and FD_* environment variables into the "env" section
void capture_env();

// all sections, ready to write (signal safe; empty before the first set)
const char* text();

}  // namespace crash_info
