// Menu bar: app menu (Quit) and a Graphics menu to switch fixes and enhancements while playing.
// Each option also has a single-key shortcut in the game window.
#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>  // kVK_* key codes
#include "../input.h"

namespace gfx {
int ao_mode();
void set_ao_mode(int m);
bool aniso_enabled();
void set_aniso(bool v);
void request_capture();
bool ao_hires_enabled();
void set_ao_hires(bool v);
bool drc_window_available();
bool drc_window_shown();
void show_drc_window(bool on);
}  // namespace gfx

static NSWindow* g_tv;
static NSString* const kTitle = @"The Legend of Zelda: The Wind Waker HD (recompiled)";

// the TV title summarises the active options so a key press is visible without opening the menu
static void update_title() {
    static const char* ao[3] = {"original", "centre fix", "centre + noise fix"};
    [g_tv setTitle:[NSString stringWithFormat:@"%@ — AO: %s · AF: %s", kTitle, ao[gfx::ao_mode()],
                                              gfx::aniso_enabled() ? "16x" : "game"]];
}

@interface WWGraphicsMenu : NSObject <NSMenuItemValidation>
@end

@implementation WWGraphicsMenu
- (void)setAO:(NSMenuItem*)item { gfx::set_ao_mode((int)item.tag); update_title(); }
- (void)toggleAniso:(NSMenuItem*)item { gfx::set_aniso(!gfx::aniso_enabled()); update_title(); }
- (void)capture:(NSMenuItem*)item { gfx::request_capture(); }
- (void)setController:(NSMenuItem*)item {
    input::set_pro_controller(item.tag == 1);
    gfx::show_drc_window(item.tag == 0);  // the GamePad window follows the controller choice
}
- (void)toggleDrcWindow:(NSMenuItem*)item { gfx::show_drc_window(!gfx::drc_window_shown()); }
- (void)toggleHires:(NSMenuItem*)item { gfx::set_ao_hires(!gfx::ao_hires_enabled()); update_title(); }
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(setAO:)) item.state = item.tag == gfx::ao_mode() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setController:))
        item.state = (item.tag == 1) == input::pro_controller() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleDrcWindow:)) {
        item.state = gfx::drc_window_shown() ? NSControlStateValueOn : NSControlStateValueOff;
        return gfx::drc_window_available();
    }
    if (item.action == @selector(toggleHires:)) item.state = gfx::ao_hires_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleAniso:)) item.state = gfx::aniso_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static WWGraphicsMenu* g_target;

static NSMenuItem* add(NSMenu* m, NSString* title, SEL action, NSString* key, NSInteger tag = 0) {
    NSMenuItem* it = [m addItemWithTitle:title action:action keyEquivalent:@""];
    it.target = g_target;
    it.tag = tag;
    // shown for reference; the game window's key monitor handles the actual key (no Cmd needed)
    if (key.length) it.toolTip = [NSString stringWithFormat:@"Shortcut in game: %@", key];
    return it;
}

namespace gfx {

void install_menu(NSWindow* tv) {
    g_tv = tv;
    g_target = [WWGraphicsMenu new];
    NSMenu* bar = [NSMenu new];

    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* app = [NSMenu new];
    [app addItemWithTitle:@"Quit Wind Waker HD" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = app;

    NSMenuItem* gfxItem = [bar addItemWithTitle:@"Graphics" action:nil keyEquivalent:@""];
    NSMenu* g = [[NSMenu alloc] initWithTitle:@"Graphics"];
    [g addItemWithTitle:@"Ambient occlusion (O cycles)" action:nil keyEquivalent:@""].enabled = NO;
    add(g, @"    Original (as on Wii U)", @selector(setAO:), @"O", 0);
    add(g, @"    Centre fix", @selector(setAO:), @"O", 1);
    add(g, @"    Centre + noise fix", @selector(setAO:), @"O", 2);
    add(g, @"Full-size occlusion depth (M)", @selector(toggleHires:), @"M");
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"16x anisotropic filtering (N)", @selector(toggleAniso:), @"N");
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"Capture frame for debugging (P)", @selector(capture:), @"P");
    gfxItem.submenu = g;

    NSMenuItem* inItem = [bar addItemWithTitle:@"Input" action:nil keyEquivalent:@""];
    NSMenu* in = [[NSMenu alloc] initWithTitle:@"Input"];
    [in addItemWithTitle:@"Keyboard and controllers act as" action:nil keyEquivalent:@""].enabled = NO;
    add(in, @"    Wii U GamePad", @selector(setController:), @"", 0);
    add(in, @"    Wii U Pro Controller", @selector(setController:), @"", 1);
    [in addItem:[NSMenuItem separatorItem]];
    add(in, @"Show GamePad window", @selector(toggleDrcWindow:), @"");
    inItem.submenu = in;

    NSApp.mainMenu = bar;
    update_title();
}

// single-key shortcuts from the game window; true if the key was used
bool menu_hotkey(uint16_t code) {
    switch (code) {
    case kVK_ANSI_O: set_ao_mode((ao_mode() + 1) % 3); break;
    case kVK_ANSI_N: set_aniso(!aniso_enabled()); break;
    case kVK_ANSI_M: set_ao_hires(!ao_hires_enabled()); break;
    case kVK_ANSI_P: case kVK_F12: request_capture(); return true;
    default: return false;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ update_title(); });
    return true;
}

}  // namespace gfx
