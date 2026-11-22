// Online co-op compat (../README.md): upstream's host settings store, as the co-op code uses it. Here the online
// settings live in <app files>/online/settings.ini, read and written by the app through Native.coopGet / coopSet.
#pragma once
#include <functional>
#include <string>
namespace hostui {
void post(std::function<void()> fn);  // (runs it now: the store is locked)
bool get(const char* key, std::string& value);
void set(const char* key, const std::string& value);
void set_clipboard(const std::string& text);  // (the app copies; unused)
// (fork: Android) the settings' profile: each game or save file that plays online has its own (compat.cpp)
bool set_profile(const std::string& name);
std::string profile();
}  // namespace hostui
