// Native host helpers used by the portable Vulkan renderer on macOS.
#import <Foundation/Foundation.h>
#include "host.h"
#include <exception>

namespace host {
void with_autorelease_pool(void (*fn)(void*), void* context) {
  // ARC does not necessarily drain @autoreleasepool on C++ exception unwind.
  // Catch inside the pool and propagate only after its normal scope exit.
  std::exception_ptr error;
  @autoreleasepool {
    try { fn(context); }
    catch (...) { error = std::current_exception(); }
  }
  if (error) std::rethrow_exception(error);
}
void with_autorelease_pool(void (*fn)()) {
  with_autorelease_pool([](void* context) {
    (*static_cast<void (**)()>(context))();
  }, &fn);
}
} // namespace host
