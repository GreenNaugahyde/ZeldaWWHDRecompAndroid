// Menu bar: app menu (Quit) and a Graphics menu to switch fixes and enhancements while playing.
// Each option also has a single-key shortcut in the game window.
#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>  // kVK_* key codes
#include "../input.h"
#include <sys/stat.h>
#include <ctime>

#include "../savestate.h"

namespace gfx {
int ao_mode();
void set_ao_mode(int m);
bool aniso_enabled();
void set_aniso(bool v);
void request_capture();
bool ao_hires_enabled();
bool fxaa_enabled();
void set_fxaa(bool v);
void set_ao_hires(bool v);
bool drc_window_available();
bool drc_window_shown();
void show_drc_window(bool on);
float res_scale();
void set_res_scale(float f);
}  // namespace gfx

// internal resolution steps (Graphics menu; R cycles)
static const float kResScales[] = {1.0f, 1.5f, 2.0f, 3.0f};
static float g_res_shown = 0;  // last value set from the UI (res_scale() lags a frame)
static float current_res_scale() { return g_res_shown ? g_res_shown : gfx::res_scale(); }
static void set_res(float f) { g_res_shown = f; gfx::set_res_scale(f); }
static void cycle_res() {
    float cur = current_res_scale();
    size_t n = sizeof kResScales / sizeof *kResScales, i = 0;
    while (i < n && kResScales[i] <= cur + 0.01f) i++;
    set_res(kResScales[i % n]);
}


namespace interp {
int mode();  // 0 off, 1 frame interpolation, 2 true 60 (logic at 60 steps per second)
void set_mode(int m);
}

namespace gx2 { uint64_t flips_presented(); }
#include "../mods/mods.h"
namespace ax { void start_sound_trace(const char* path, double seconds); }
namespace gfx { bool menu_hotkey(uint16_t code); NSMenuItem* controls_menu_item(); /* controls_ui.mm */ }

static NSWindow* g_tv;
static double g_fps = 0;  // frames presented per second, measured over the last half second
static NSString* const kTitle = @"The Legend of Zelda: The Wind Waker HD (recompiled)";

// the TV title summarises the active options so a key press is visible without opening the menu
static void update_title() {
    static const char* ao[3] = {"original", "centre fix", "centre + noise fix"};
    NSString* res = current_res_scale() != 1.0f ? [NSString stringWithFormat:@" \u00b7 %gx res", current_res_scale()] : @"";
    NSString* t = [NSString stringWithFormat:@"%@ \u2014 %.0f fps%@ \u00b7 AO: %s%s \u00b7 AF: %s%s%s", kTitle, g_fps, res, ao[gfx::ao_mode()],
                                             gfx::ao_hires_enabled() ? " + full-size depth" : "",
                                             gfx::aniso_enabled() ? "16x" : "game",
                                             interp::mode() == 2 ? " \u00b7 true 60" : interp::mode() == 1 ? " \u00b7 60 fps" : "", gfx::fxaa_enabled() ? " \u00b7 FXAA" : ""];
    std::string msg = ss::last_message();  // save state confirmations
    if (!msg.empty()) t = [NSString stringWithFormat:@"%@ — %s", kTitle, msg.c_str()];
    [g_tv setTitle:t];
}

// Save States menu: items are rebuilt each time it opens (slot time and area)
@interface WWStateMenu : NSObject <NSMenuDelegate>
@end
@implementation WWStateMenu
- (void)save:(NSMenuItem*)item { ss::request_save((int)item.tag); }
- (void)load:(NSMenuItem*)item { ss::request_load((int)item.tag); }
- (void)menuNeedsUpdate:(NSMenu*)m {
    [m removeAllItems];
    ss::SlotInfo info[ss::kSlots + 1];
    for (int i = 1; i <= ss::kSlots; i++) info[i] = ss::slot_info(i);
    auto label = [&](int i) -> NSString* {
        const ss::SlotInfo& s = info[i];
        if (!s.used) return @"empty";
        NSString* d = [NSString stringWithFormat:@"%s%s%s", s.when.c_str(), s.area.empty() ? "" : " · ", s.area.c_str()];
        return s.compatible ? d : [d stringByAppendingString:@" (incompatible)"];
    };
    for (int i = 1; i <= ss::kSlots; i++) {
        NSMenuItem* it = [m addItemWithTitle:[NSString stringWithFormat:@"Save to slot %d  (%@)", i, label(i)] action:@selector(save:) keyEquivalent:@""];
        it.target = self;
        it.tag = i;
        it.toolTip = [NSString stringWithFormat:@"Shortcut in game: Shift+F%d", i];
    }
    [m addItem:[NSMenuItem separatorItem]];
    for (int i = 1; i <= ss::kSlots; i++) {
        NSMenuItem* it = [m addItemWithTitle:[NSString stringWithFormat:@"Load slot %d  (%@)", i, label(i)] action:@selector(load:) keyEquivalent:@""];
        it.target = self;
        it.tag = i;
        it.enabled = info[i].used && info[i].compatible;
        it.toolTip = [NSString stringWithFormat:@"Shortcut in game: F%d", i];
    }
}
@end
static WWStateMenu* g_state_menu;

@interface WWGraphicsMenu : NSObject <NSMenuItemValidation>
@end

@implementation WWGraphicsMenu
- (void)setAO:(NSMenuItem*)item { gfx::set_ao_mode((int)item.tag); update_title(); }
- (void)toggleAniso:(NSMenuItem*)item { gfx::set_aniso(!gfx::aniso_enabled()); update_title(); }
- (void)capture:(NSMenuItem*)item { gfx::request_capture(); }
- (void)recordSound:(NSMenuItem*)item { gfx::menu_hotkey(kVK_ANSI_9); }
- (void)setController:(NSMenuItem*)item {
    input::set_pro_controller(item.tag == 1);
    gfx::show_drc_window(item.tag == 0);  // the GamePad window follows the controller choice
}
- (void)toggleInterp:(NSMenuItem*)item { interp::set_mode(interp::mode() == item.tag ? 0 : (int)item.tag); update_title(); }
- (void)toggleDrcWindow:(NSMenuItem*)item { gfx::show_drc_window(!gfx::drc_window_shown()); }
- (void)toggleFxaa:(NSMenuItem*)item { gfx::set_fxaa(!gfx::fxaa_enabled()); update_title(); }
- (void)toggleHires:(NSMenuItem*)item { gfx::set_ao_hires(!gfx::ao_hires_enabled()); update_title(); }
- (void)setRes:(NSMenuItem*)item { set_res(kResScales[item.tag]); update_title(); }
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(setRes:))
        item.state = fabsf(kResScales[item.tag] - current_res_scale()) < 0.01f ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setAO:)) item.state = item.tag == gfx::ao_mode() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(setController:))
        item.state = (item.tag == 1) == input::pro_controller() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleInterp:)) item.state = interp::mode() == item.tag ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleDrcWindow:)) {
        item.state = gfx::drc_window_shown() ? NSControlStateValueOn : NSControlStateValueOff;
        return gfx::drc_window_available();
    }
    if (item.action == @selector(toggleFxaa:)) item.state = gfx::fxaa_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleHires:)) item.state = gfx::ao_hires_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    if (item.action == @selector(toggleAniso:)) item.state = gfx::aniso_enabled() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static WWGraphicsMenu* g_target;

// Menu items backed by two blocks: `get` gives the check mark, `set` is called with !get() on a click.
// Used by the Gameplay menu: one line per option (see install_menu).
@interface WWBlockItem : NSObject <NSMenuItemValidation>
@property(copy) BOOL (^get)(void);
@property(copy) void (^set)(BOOL);
@end
@implementation WWBlockItem
- (void)act:(NSMenuItem*)item { self.set(!self.get()); update_title(); }
- (BOOL)validateMenuItem:(NSMenuItem*)item {
    item.state = self.get() ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end
static NSMutableArray<WWBlockItem*>* g_block_items;  // menu items hold their targets weakly

// add a checkable item: toggle(menu, @"Title", ^{ return state(); }, ^(BOOL on) { set_state(on); })
static NSMenuItem* toggle(NSMenu* m, NSString* title, BOOL (^get)(void), void (^set)(BOOL), NSString* tip = @"") {
    WWBlockItem* t = [WWBlockItem new];
    t.get = get;
    t.set = set;
    if (!g_block_items) g_block_items = [NSMutableArray new];
    [g_block_items addObject:t];
    NSMenuItem* it = [m addItemWithTitle:title action:@selector(act:) keyEquivalent:@""];
    it.target = t;
    if (tip.length) it.toolTip = tip;
    return it;
}

static NSMenuItem* add(NSMenu* m, NSString* title, SEL action, NSString* key, NSInteger tag = 0) {
    NSMenuItem* it = [m addItemWithTitle:title action:action keyEquivalent:@""];
    it.target = g_target;
    it.tag = tag;
    // shown for reference; the game window's key monitor handles the actual key (no Cmd needed)
    if (key.length) it.toolTip = [NSString stringWithFormat:@"Shortcut in game: %@", key];
    return it;
}

namespace mods { bool climb_enabled(); void set_climb_enabled(bool on); }  // mods/climb.cpp

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
    [g addItemWithTitle:@"Internal resolution (R cycles)" action:nil keyEquivalent:@""].enabled = NO;
    add(g, @"    1x (1280x720, as on Wii U)", @selector(setRes:), @"R", 0);
    add(g, @"    1.5x (1920x1080)", @selector(setRes:), @"R", 1);
    add(g, @"    2x (2560x1440)", @selector(setRes:), @"R", 2);
    add(g, @"    3x (3840x2160)", @selector(setRes:), @"R", 3);
    [g addItem:[NSMenuItem separatorItem]];
    [g addItemWithTitle:@"Ambient occlusion (O cycles)" action:nil keyEquivalent:@""].enabled = NO;
    add(g, @"    Original (as on Wii U)", @selector(setAO:), @"O", 0);
    add(g, @"    Centre fix", @selector(setAO:), @"O", 1);
    add(g, @"    Centre + noise fix", @selector(setAO:), @"O", 2);
    add(g, @"Full-size occlusion depth (M)", @selector(toggleHires:), @"M");
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"16x anisotropic filtering (N)", @selector(toggleAniso:), @"N");
    add(g, @"Edge smoothing, FXAA (8)", @selector(toggleFxaa:), @"8");
    add(g, @"60 fps: frame interpolation (6)", @selector(toggleInterp:), @"6", 1);
    add(g, @"60 fps: true 60, game logic at 60 steps/s (7, experimental)", @selector(toggleInterp:), @"7", 2);
    [g addItem:[NSMenuItem separatorItem]];
    add(g, @"Capture frame for debugging (P)", @selector(capture:), @"P");
    add(g, @"Record 3 s of sound activity (9)", @selector(recordSound:), @"9");
    gfxItem.submenu = g;

    NSMenuItem* inItem = [bar addItemWithTitle:@"Input" action:nil keyEquivalent:@""];
    NSMenu* in = [[NSMenu alloc] initWithTitle:@"Input"];
    [in addItemWithTitle:@"Keyboard and controllers act as" action:nil keyEquivalent:@""].enabled = NO;
    add(in, @"    Wii U GamePad", @selector(setController:), @"", 0);
    add(in, @"    Wii U Pro Controller", @selector(setController:), @"", 1);
    [in addItem:[NSMenuItem separatorItem]];
    add(in, @"Show GamePad window", @selector(toggleDrcWindow:), @"");
    [in addItem:gfx::controls_menu_item()];
    inItem.submenu = in;

    // Gameplay: optional mods, all off by default (runtime/src/mods/). One line per option.
    NSMenuItem* gpItem = [bar addItemWithTitle:@"Gameplay" action:nil keyEquivalent:@""];
    NSMenu* gp = [[NSMenu alloc] initWithTitle:@"Gameplay"];
    [gp addItemWithTitle:@"Camera" action:nil keyEquivalent:@""].enabled = NO;
    toggle(gp, @"    Direct right-stick camera (no easing)", ^BOOL { return mods::direct_camera(); }, ^(BOOL on) { mods::set_direct_camera(on); },
           @"The right stick turns the camera at a constant rate as soon as it is pushed");
    for (float sp : {0.5f, 1.0f, 1.5f, 2.0f})
        toggle(gp, [NSString stringWithFormat:@"        Speed %gx", sp], ^BOOL { return mods::camera_speed() == sp; }, ^(BOOL) { mods::set_camera_speed(sp); });
    toggle(gp, @"    Mouse camera (click the picture to capture, Esc releases)", ^BOOL { return mods::mouse_camera(); },
           ^(BOOL on) { mods::set_mouse_camera(on); }, @"Mouse turns the camera; middle click or Esc releases the pointer; left click fires an aimed item");
    for (float se : {0.08f, 0.15f, 0.3f})
        toggle(gp, [NSString stringWithFormat:@"        Sensitivity %s", se < 0.1f ? "low" : se < 0.2f ? "medium" : "high"],
               ^BOOL { return mods::mouse_sensitivity() == se; }, ^(BOOL) { mods::set_mouse_sensitivity(se); });
    toggle(gp, @"    First person on R3 / mouse wheel", ^BOOL { return mods::first_person_wheel(); }, ^(BOOL on) { mods::set_first_person_wheel(on); },
           @"Right-stick click (keyboard V) or wheel forward enters the first-person view, wheel back leaves it");
    [gp addItem:[NSMenuItem separatorItem]];
    toggle(gp, @"Climb any wall", ^BOOL { return mods::climb_enabled(); }, ^(BOOL on) { mods::set_climb_enabled(on); },
           @"Grab and climb any wall (stamina wheel; B or A lets go)");
    toggle(gp, @"Quick doors", ^BOOL { return mods::quick_doors(); }, ^(BOOL on) { mods::set_quick_doors(on); },
           @"Door events (walk-in, opening, closing) run at 4x speed");
    toggle(gp, @"Fast scene changes", ^BOOL { return mods::fast_scenes(); }, ^(BOOL on) { mods::set_fast_scenes(on); },
           @"Fades and loading between areas run at 4x speed; the scenes themselves are not sped up");
    gpItem.submenu = gp;
    mods::mouse_init((__bridge void*)tv);

    // Save States: 5 slots (savestate.cpp); Shift+F1..F5 save, F1..F5 load
    NSMenuItem* ssItem = [bar addItemWithTitle:@"Save States" action:nil keyEquivalent:@""];
    NSMenu* sm = [[NSMenu alloc] initWithTitle:@"Save States"];
    sm.autoenablesItems = NO;
    g_state_menu = [WWStateMenu new];
    sm.delegate = g_state_menu;
    ssItem.submenu = sm;

    NSApp.mainMenu = bar;
    update_title();
    // live frame rate: presented frames over the last half second
    [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer*) {
        static uint64_t last = gx2::flips_presented();
        static CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        uint64_t n = gx2::flips_presented();
        CFAbsoluteTime t = CFAbsoluteTimeGetCurrent();
        if (t > t0) g_fps = (double)(n - last) / (t - t0);
        last = n;
        t0 = t;
        update_title();
    }];
}

// single-key shortcuts from the game window; true if the key was used
bool menu_hotkey(uint16_t code) {
    switch (code) {
    case kVK_ANSI_O: set_ao_mode((ao_mode() + 1) % 3); break;
    case kVK_ANSI_N: set_aniso(!aniso_enabled()); break;
    case kVK_ANSI_M: set_ao_hires(!ao_hires_enabled()); break;
    case kVK_ANSI_6: interp::set_mode(interp::mode() == 1 ? 0 : 1); break;
    case kVK_ANSI_7: interp::set_mode(interp::mode() == 2 ? 0 : 2); break;
    case kVK_ANSI_8: set_fxaa(!fxaa_enabled()); break;
    case kVK_ANSI_R: cycle_res(); break;
    case kVK_ANSI_P: case kVK_F12: request_capture(); return true;
    case kVK_F1: case kVK_F2: case kVK_F3: case kVK_F4: case kVK_F5: {
        int slot = code == kVK_F1 ? 1 : code == kVK_F2 ? 2 : code == kVK_F3 ? 3 : code == kVK_F4 ? 4 : 5;
        if ([NSEvent modifierFlags] & NSEventModifierFlagShift) ss::request_save(slot);
        else ss::request_load(slot);
        return true;
    }
    case kVK_ANSI_9: {
        // 3 s of sound activity (voice starts), without the frame capture's stall
        char path[96];
        time_t t = time(nullptr);
        mkdir("captures", 0755);
        strftime(path, sizeof path, "captures/%Y%m%d-%H%M%S-sound.log", localtime(&t));
        ax::start_sound_trace(path, 3.0);
        return true;
    }
    default: return false;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ update_title(); });
    return true;
}

}  // namespace gfx
