// Calls from native code into the Java activity (android/jni_main.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace jni {
// show the text-entry dialog; the answer arrives through input::prompt_finished. False if it can't be shown.
void rumble(float strength, uint32_t ms);  // the phone vibrates (0 stops it)
bool request_text_input(const std::u16string& initial, int maxLen);
}  // namespace jni

namespace input {
void set_pad(uint32_t buttons, float lx, float ly, float rx, float ry);  // from the Java input mapper
void prompt_finished(bool ok, const std::u16string& text);               // text-entry dialog closed
}  // namespace input
