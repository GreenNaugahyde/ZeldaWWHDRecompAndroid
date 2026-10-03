#pragma once
struct SDL_Window;
namespace gfxvk {
float res_scale();
float requested_res_scale();
void set_res_scale(float scale);
int ao_mode();
void set_ao_mode(int mode);
bool ao_hires_enabled();
void set_ao_hires(bool enabled);
bool aniso_enabled();
void set_aniso(bool enabled);
bool fxaa_enabled();
void set_fxaa(bool enabled);
int scale_filter(); // 0 smooth, 1 sharp, 2 integer
void set_scale_filter(int filter);
enum class GraphicsFeature { AO, AOHires, Anisotropy, FXAA, ScaleFilter, Count };
bool graphics_feature_available(GraphicsFeature feature);
void set_graphics_feature_available(GraphicsFeature feature, bool available = true);
// Returns true for reserved graphics keys, including unavailable effects.
bool graphics_hotkey(char key, bool activate);
#ifdef __APPLE__
void install_graphics_menu(SDL_Window* window);
#else
inline void install_graphics_menu(SDL_Window*) {}
#endif
}
