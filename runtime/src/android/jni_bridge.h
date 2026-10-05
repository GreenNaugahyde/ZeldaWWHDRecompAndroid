// Calls from native code into the Java activity (android/jni_main.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace jni {
// show the text-entry dialog; the answer arrives through input::prompt_finished. False if it can't be shown.
bool request_text_input(const std::u16string& initial, int maxLen);
// the vibration motor (input::rumble): a pattern of `bits` 1/120 s steps, or stop (bits == 0)
void rumble(const uint8_t* pattern, int bits);
void rumble_hold(bool on);
}  // namespace jni

namespace input {
void set_pad(uint32_t buttons, float lx, float ly, float rx, float ry);  // from the Java input mapper
void prompt_finished(bool ok, const std::u16string& text);               // text-entry dialog closed
}  // namespace input
