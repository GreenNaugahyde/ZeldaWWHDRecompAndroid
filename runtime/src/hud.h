// HUD elements the player hid (the context-aware touch controls replace the game's button cluster):
// nw::lyt panes skipped in Pane::Draw (aspect.cpp's hook), by name. WWHD_HUD_HIDE=name,name... adds
// names for testing.
#pragma once
#include <cstdint>
#include <string>

// hide (or show again) the panes with these names: "name,name,..." (empty: none)
void hud_set_hidden(const std::string& names);
// Pane::Draw: skip this pane (and its children)
bool hud_hidden(uint32_t pane);

// a layout root is drawn (Pane::Draw, aspect.cpp): notes the pause menu's layout (its children
// include L_MenuArrowL_00; it is computed also while the menu is closed, but drawn only when open)
void hud_root_drawn(uint32_t root);
// the pause menu (items, map, ...) was on screen in the last moments
bool hud_menu_open();

// HUD elements that fade out when not needed: the hearts (shown with the sword in hand and after
// damage) and the rupees (shown after the count changed). On / off is the app's option; whether
// the game state wants one now comes from mods/touch_state.cpp.
enum HudFade { kFadeHearts, kFadeRupees, kFadeCount };
void hud_set_fade_auto(int which, bool on);
void hud_set_fade_wanted(int which, bool want);
// the element's opacity factor for this frame (fades toward the wanted state; 1 when off); called
// once per frame by its pane's matrix calculation (aspect.cpp)
float hud_fade_alpha(int which);
// fully faded out: the element isn't drawn at all (parts that don't inherit the alpha would stay
// faintly visible)
bool hud_fade_gone(int which);
