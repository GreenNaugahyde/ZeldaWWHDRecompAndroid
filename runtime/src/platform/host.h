#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <mach-o/ldsyms.h>
#endif
namespace host {
#if defined(__APPLE__) && defined(WWHD_HAS_VULKAN)
// Render command batches can create Objective-C temporaries inside MoltenVK.
void with_autorelease_pool(void (*fn)());
void with_autorelease_pool(void (*fn)(void*), void* context);
#else
inline void with_autorelease_pool(void (*fn)()) { fn(); }
inline void with_autorelease_pool(void (*fn)(void*), void* context) { fn(context); }
#endif
// The native call is synchronous: captured references remain valid and no heap
// allocation or callable lifetime extension is needed. Exceptions propagate.
template<class Fn> void with_autorelease_pool(Fn&& fn) {
 auto call = [&] { fn(); };
 with_autorelease_pool([](void* context) {
  (*static_cast<decltype(call)*>(context))();
 }, &call);
}
inline thread_local std::string thread_label;
inline void set_thread_name(const char* name) {
 thread_label=name;
#ifdef __APPLE__
 pthread_setname_np(name);
#elif defined(_WIN32)
 using SetDescription=HRESULT(WINAPI*)(HANDLE,PCWSTR);
 auto f=(SetDescription)GetProcAddress(GetModuleHandleW(L"Kernel32.dll"),"SetThreadDescription");
 if(f) { std::wstring text; for(unsigned char c:thread_label)text.push_back(c); f(GetCurrentThread(),text.c_str()); }
#else
 pthread_setname_np(pthread_self(),thread_label.substr(0,15).c_str());
#endif
}
inline void get_thread_name(char* out,size_t size) {
 if(!size)return;
#ifdef __APPLE__
 pthread_getname_np(pthread_self(),out,size);
#else
 snprintf(out,size,"%s",thread_label.empty()?"host":thread_label.c_str());
#endif
}
inline uintptr_t executable_base() {
#ifdef __APPLE__
 return (uintptr_t)&_mh_execute_header;
#elif defined(_WIN32)
 return (uintptr_t)GetModuleHandleW(nullptr);
#else
 static int anchor;
 Dl_info info{}; return dladdr(&anchor,&info)?(uintptr_t)info.dli_fbase:0;
#endif
}
inline std::string executable_path() {
#ifdef _WIN32
 char path[32768]; DWORD n=GetModuleFileNameA(nullptr,path,sizeof path);return std::string(path,n);
#elif !defined(__APPLE__)
 char path[4096];ssize_t n=readlink("/proc/self/exe",path,sizeof path);return n>0?std::string(path,n):std::string();
#else
 return {};
#endif
}
inline size_t page_size() {
#ifdef _WIN32
 SYSTEM_INFO info;GetSystemInfo(&info);return info.dwPageSize;
#else
 return (size_t)getpagesize();
#endif
}
inline bool memory_touched(void* p,size_t bytes) {
#ifdef _WIN32
 // MEM_WRITE_WATCH records written pages even after paging them out. Residency alone
 // cannot distinguish untouched zero pages from modified pages in the swap file.
 size_t n=(bytes+page_size()-1)/page_size(); std::vector<void*> pages(n);
 ULONG_PTR count=n; DWORD granularity=0;
 if(GetWriteWatch(0,p,bytes,pages.data(),&count,&granularity)!=0)return true;
 return count!=0;
#elif defined(__APPLE__)
 std::vector<char> pages((bytes+page_size()-1)/page_size());
 if(mincore(p,bytes,pages.data()))return true;
 for(auto page:pages)if(page!=0)return true;
 return false;
#else
 // Linux mincore reports residency, not whether a swapped-out page contains data.
 // Scan the mapped region to preserve every byte; this is slower than macOS/Windows.
 (void)p;(void)bytes;return true;
#endif
}
inline bool replace_file(const std::string& from,const std::string& to) {
#ifdef _WIN32
 return MoveFileExA(from.c_str(),to.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
 return rename(from.c_str(),to.c_str())==0;
#endif
}
inline std::string config_dir() {
#ifdef __APPLE__
 const char* home=getenv("HOME");return std::string(home?home:".")+"/Library/Application Support/WWHD";
#elif defined(_WIN32)
 const char* root=getenv("APPDATA");return std::string(root?root:".")+"/WWHD";
#else
 if(const char* xdg=getenv("XDG_CONFIG_HOME"))return std::string(xdg)+"/wwhd";
 const char* home=getenv("HOME");return std::string(home?home:".")+"/.config/wwhd";
#endif
}
}
