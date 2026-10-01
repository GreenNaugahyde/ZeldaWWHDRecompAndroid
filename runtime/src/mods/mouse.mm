// Mouse capture for the mouse camera (macOS). With the mod on, a click into the TV window captures
// the pointer: it is hidden and detached from the cursor position, and its movement goes to the
// camera (camera.cpp). Esc, a middle click, switching the mod off or leaving the window releases it.
// The mouse wheel (first person mod) works whenever the TV window is the key window.
// WWHD_NO_HOST_INPUT ignores the real mouse (test runs; WWHD_TEST_MOUSE injects movement).
#import <AppKit/AppKit.h>
#include <Carbon/Carbon.h>  // kVK_Escape

#include <atomic>

#include "mods.h"
#include "runtime.h"

namespace mods {
namespace {
NSWindow* __weak g_tv;
std::atomic<bool> g_captured{false};

bool in_tv(NSEvent* e) { return g_tv && e.window == g_tv; }

void capture() {
    if (g_captured.exchange(true)) return;
    CGAssociateMouseAndMouseCursorPosition(false);
    [NSCursor hide];
    LOG("[mods] mouse captured (Esc or middle click releases)");
}
void release_now() {
    if (!g_captured.exchange(false)) return;
    CGAssociateMouseAndMouseCursorPosition(true);
    [NSCursor unhide];
    mouse_button(0, false);
    mouse_button(1, false);
    LOG("[mods] mouse released");
}
}  // namespace

bool mouse_captured() { return g_captured.load(std::memory_order_relaxed); }

void mouse_release() {
    if ([NSThread isMainThread]) release_now();
    else dispatch_async(dispatch_get_main_queue(), ^{ release_now(); });
}

bool host_key_down(uint16_t code) {
    if (code == kVK_Escape && mouse_captured()) {
        mouse_release();
        return true;
    }
    return false;
}

void mouse_init(void* tv_window) {
    g_tv = (__bridge NSWindow*)tv_window;
    if (getenv("WWHD_NO_HOST_INPUT")) return;
    g_tv.acceptsMouseMovedEvents = YES;
    NSEventMask mask = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskRightMouseDragged |
                       NSEventMaskOtherMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
                       NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskOtherMouseDown | NSEventMaskScrollWheel;
    [NSEvent addLocalMonitorForEventsMatchingMask:mask handler:^NSEvent*(NSEvent* e) {
        if (e.type == NSEventTypeScrollWheel) {
            if (first_person_wheel() && (in_tv(e) || mouse_captured())) {
                // a wheel notch is about 1 line; trackpads send many small precise deltas
                float dy = (float)e.scrollingDeltaY;
                if (e.hasPreciseScrollingDeltas) dy /= 10.0f;
                mouse_wheel(dy);
                return nil;
            }
            return e;
        }
        if (!mouse_camera()) return e;
        if (!mouse_captured()) {
            // a click into the game picture captures the pointer (the title bar stays usable)
            if (e.type == NSEventTypeLeftMouseDown && in_tv(e) && NSPointInRect(e.locationInWindow, g_tv.contentView.frame)) {
                capture();
                return nil;
            }
            return e;
        }
        switch (e.type) {
        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged:
            mouse_add((float)e.deltaX, (float)e.deltaY);  // deltaY > 0: down
            break;
        case NSEventTypeLeftMouseDown: mouse_button(0, true); break;
        case NSEventTypeLeftMouseUp: mouse_button(0, false); break;
        case NSEventTypeRightMouseDown: mouse_button(1, true); break;
        case NSEventTypeRightMouseUp: mouse_button(1, false); break;
        case NSEventTypeOtherMouseDown: release_now(); break;
        default: break;
        }
        return nil;
    }];
    // leaving the game (other app, other window) releases the pointer
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification
                                                      object:nil queue:nil usingBlock:^(NSNotification*) { release_now(); }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResignKeyNotification
                                                      object:g_tv queue:nil usingBlock:^(NSNotification*) { release_now(); }];
}

}  // namespace mods
