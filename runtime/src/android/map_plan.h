#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The mini map indoors (MiniMap.java), from the player's game files (map_plan.cpp): a stage's own map data, and
// floor plans drawn from its rooms' collision where the game has no map pictures (houses, caves, boss rooms).
namespace map_plan {
// <stage>_Stage.szs's stage.dzs: DMAP {origin x, origin z, scale} (0, 0, 0 without one), then FLOR {height, floor}
// pairs (floor: 0 = 1F, -1 = B1, ...; heights where each floor starts, lowest first)
bool stage_info(const std::string& gameDir, const std::string& stage, std::vector<float>& out);
// the stage's chests (TRES of stage.dzs and its rooms' room.dzr) and doors (TGDR), 7 floats each: {0, x, y, z, chest
// number (dSv_memBit_c mTbox bit), room, 0} / {1, x, y, z, turn about y (radians), the rooms on its two sides}
bool marks(const std::string& gameDir, const std::string& stage, std::vector<float>& out);
// the floors (collision faces turned up) of all the stage's rooms whose middle is between yMin and yMax, seen from
// above, in the dungeon map's colours (yellow, mottled; a brown edge; 70 % opaque): ARGB pixels, w x h, world x / z
// of the top left corner and world units per pixel; false without any
struct Plan {
    std::vector<uint32_t> argb;
    int w = 0, h = 0;
    float x0 = 0, z0 = 0, upp = 1;
};
// style: PLAN_CLASSIC as above; PLAN_TERRAIN: the highest floor per pixel coloured by its ground (as sea_terrain; water
// and lava too), shaded by its slope, ledges dark, a dark edge, rooms in `dim` but not `lit` grey and faint;
// PLAN_MASK: the floor white, nothing else (MiniMap.redraw). Only rooms (bit per room number) in lit or dim.
// window {x0, z0, x1, z1} (null: all of it): only that part, at fineUpp world units a pixel (the full map zoomed in).
enum { PLAN_CLASSIC = 0, PLAN_TERRAIN = 1, PLAN_MASK = 2 };
bool room_plan(const std::string& gameDir, const std::string& stage, float yMin, float yMax, Plan& out,
               int style = PLAN_CLASSIC, uint64_t lit = ~0ull, uint64_t dim = 0, const float* window = nullptr,
               float fineUpp = 0);
// a sea room's island seen from above, coloured by its ground (sea room 1..49; its collision room.dzb from
// Stage/sea_Room<n>.szs or the szs_permanent packs): grass, sand, rock, wood, roofs ..., shaded by the slope, a dark
// coast, shallow water light blue, rock with no ground on top (only its cliffs collide) filled in; the land's box
bool sea_terrain(const std::string& gameDir, int room, Plan& out);
// the charts' list (content/Common/Misc/Misc.szs -> Misc.bfres -> cmapdat.bin, as the GamePad's chart menu reads it;
// the randomizer rewrites it): a u32 count, then 0x26 bytes each {grid (sea room), chart bit + 1, chart number, kind,
// grid x, grid y, then per salvage point set {picture centre x, z, salvage x, z} (s16, from the square's middle)}
bool chart_list(const std::string& gameDir, std::vector<uint8_t>& out);
}  // namespace map_plan
