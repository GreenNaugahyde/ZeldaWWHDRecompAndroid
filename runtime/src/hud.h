// HUD elements the player hid (the context-aware touch controls replace the game's button cluster):
// nw::lyt panes skipped in Pane::Draw (aspect.cpp's hook), by name. WWHD_HUD_HIDE=name,name... adds
// names for testing.
#pragma once
#include <cstdint>
#include <string>

// hide (or show again) the panes with these names: "name,name,..." (empty: none)
void hud_set_hidden(const std::string& names);
// the item buttons' letters on the HUD and the GamePad's drop slots: "X,Y,R" order, ASCII, up to 6 characters each
// ("" or a missing one: the game's letter)
void hud_set_slot_labels(const std::string& csv);
// Pane::Draw: skip this pane (and its children)
bool hud_hidden(uint32_t pane);

// the game's own button cluster panes (hidden for the context buttons, but still computed by the
// game): X, Y, R, B, ZR. Their alpha (0..255, 0 also when invisible) as the game set it, read when
// Pane::Draw reached them; -1 until seen. WWHD_TOUCHSTATE_LOG=1 logs changes, with their children.
enum HudCluster { kClusterX, kClusterY, kClusterR, kClusterB, kClusterZR, kClusterCount };
int hud_cluster_alpha(int which);
// X / Y / R: the game shows a command text on the button instead of the item (T_CommandX_00 visible,
// e.g. Delete in the Picto Box's album); -1 until seen
int hud_cluster_command(int which);

// the zoom bar of the Picto Box / the telescope (N_Zoom_00 with the handle N_Scale_00), in layout
// units (1280x720 around the screen centre, y up), for dragging the handle on the touch screen:
// out[0] shown (drawn in the last moments), [1] handle y, [2] / [3] the handle's lowest / highest y
// (the zoom animation's ends), [4] / [5] the touch zone's left / right x
void hud_zoom_bar(int32_t out[6]);
// the message window's choices, in layout units: out[0] shown (drawn in the last moments), [1] how many
// (2 or 3), [2] / [3] their window's centre, [4] the cursor's y (on the chosen line)
void hud_choice(int32_t out[5]);
// the message's number entry (P_SelNumber_00: the auction's bid) is shown
bool hud_number_entry();
// the message window's box (P_MsgWindow_00) in layout units: out[0] shown (drawn in the last moments), [1] / [2] its
// centre, [3] / [4] its half width / height
void hud_msg_window(int32_t out[5]);
// the wind compass after the Wind's Requiem (ControlWind_00) is shown: out[0] / [1] its centre, layout units
bool hud_wind_compass(int32_t out[2]);
// the Picto Box's own screen (aiming, the photo's yes / no): its finder was drawn in the last moments
bool hud_picto_screen();
// the boot layout shows a message with one button (the first start's "save data created")
bool hud_boot_message();

// a layout root is drawn (Pane::Draw, aspect.cpp): notes the pause menu's layout (its children
// include L_MenuArrowL_00; it is computed also while the menu is closed, but drawn only when open)
void hud_root_drawn(uint32_t root);
// the pause menu (items, map, ...) was on screen in the last moments
bool hud_menu_open();

// HUD elements that fade out when not needed: the hearts (shown with the sword in hand and after
// damage), the rupees (shown after the count changed) and the magic bar (with the hearts' option:
// shown while magic is used or refilled, without the hearts). On / off is the app's option; whether
// the game state wants one now comes from mods/touch_state.cpp.
enum HudFade { kFadeHearts, kFadeRupees, kFadeMagic, kFadeCount };
void hud_set_fade_auto(int which, bool on);
void hud_set_fade_wanted(int which, bool want);
// the element's opacity factor for this frame (fades toward the wanted state; 1 when off); called
// once per frame by its pane's matrix calculation (aspect.cpp)
float hud_fade_alpha(int which);
// fully faded out: the element isn't drawn at all (parts that don't inherit the alpha would stay
// faintly visible)
bool hud_fade_gone(int which);
