#pragma once
// Loot swords (notes/loot.md): another HD model's sword as Link's Master Sword, built on the device from the player's
// own game files (nothing of the game ships with the app). A port of tools/gcmodels/hd_sword.py: the sword's mesh laid
// along Link's sword axis (the hand frame of swms / swgripms), its grip onto the hand, cut at its cross-guard: the
// blade into swms, cross-guard / grip / pommel into swgripms (also what shows in the scabbard); its texture into both
// models' slots (HD's sizes and formats), the materials coloured like the sword's own.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gcm_io.h"

namespace gcmodel {
struct SwordDef {
    const char* id;       // kept in the app's choices and in memory
    const char* name;     // shown
    const char* archive;  // the game's archive holding it (in content/Common/Pack/*.pack or content/Common/Object)
    const char* bfres;    // its BFRES in there
    const char* model;    // the model in the BFRES
    int shapes[8];        // the shapes with the sword (up to the first -1)
    bool local;           // rigid shapes kept in their bone's space (a sword on a hand bone), not the model's
    int axis;             // its long axis: 0 x, 1 y, 2 z; -1 its own main direction (pommel / guard then fractions 0..1;
                          // pommel -1: the handle at its thinner end, guard that far from it)
    float pommel, guard;  // along that axis: the pommel's end, where blade and cross-guard meet
    float length;         // overall, relative to the Master Sword
    bool flip;            // its flat side the other way
    const char* texture;  // its texture
    float tint[3];        // the blade's colour (the game's: 0.612 grey); negative: from the texture where the blade lies
    int power;            // its strength in percent of the game's sword
    int nparts;           // things of a room's mesh (several share one): only the pieces nearest these places
    float parts[2][3];
    bool byShape;         // the first shape the blade, the others the hilt (else cut at the guard)
};
// (found things: every model of the game that can be held, hd_sword_found.inc; their ids start with 'f')
const std::vector<SwordDef>& sword_list();
// how much further its tip reaches than the Master Sword's, from its root joint (the game's hit shapes)
float sword_reach(const SwordDef& s);
float sword_reach_hero(const SwordDef& s);  // the same over the Hero's Sword's tip
float sword_tilt(const SwordDef& s);        // degrees it is turned up in the hand (round the grip, the blade's plane)
const SwordDef* sword_def(const std::string& id);
// the sword's BFRES out of the game's files (gameDir: the folder with content/)
bool sword_source(const std::string& gameDir, const SwordDef& s, Bytes& bfres, std::string& error);
// what the runtime does in memory besides the file's changes: the sword slots pointed at the one picture (FTEX
// offsets: the slot, the picture's), the picture's image / mip data (offset, size, offset, size) and its colour
// tiers' data (4 tiers, then kRainbowSteps rainbow steps; image + mips each)
constexpr int kRainbowSteps = 36;  // the rainbow's pictures (one round)
struct SwordExtra {
    std::vector<std::pair<uint32_t, uint32_t>> links;
    std::array<uint32_t, 4> image{};
    std::vector<Bytes> tiers;
    // the picture lives in the loot corner, not in the file (armour on a chest: Link's slot there is too small): `links`
    // the slots shown as it, `own` its image + mips, `image` their sizes
    bool surface = false;
    Bytes own;
    std::vector<Bytes> blink;  // an outfit's picture with the eyes in its other frames (half shut, shut)
    Bytes preview;  // (the PC tool's look at an outfit's picture: RGBA)
    int previewW = 0, previewH = 0;
    // a whole character at its own proportions: each of Link's joints' distance from its parent times this, the body raised (the runtime's job)
    std::vector<float> boneRatio, boneOffset;  // (boneOffset: x, y, z per joint, for the drawing)
    float raise = 0;
    std::vector<float> handFrame;  // a whole character: two 3 x 3, wrist to hand joint
    float handScale = 1;
    float armAngle = 0;  // degrees its arms are swung out, away from its torso
};
// the game's Link.bfres with the sword as its Master Sword and Hero's Sword; false and `error` when it can't be made
bool make_sword(const Bytes& link, const Bytes& source, const SwordDef& s, Bytes& out, std::string& error,
                std::string* report = nullptr, SwordExtra* extra = nullptr);
// loot shields: the same rows (length = size against the Hero's Shield), in place of the Hero's and the Mirror Shield
const std::vector<SwordDef>& shield_list();
const SwordDef* shield_def(const std::string& id);
// a picture of the weapon's real model for the app's buttons: `size` x `size` RGBA, its long axis across the diagonal, from the player's own game files
bool weapon_icon(const std::string& gameDir, const SwordDef& def, int size, std::vector<uint8_t>& rgba, std::string& error);
bool make_shield(const Bytes& link, const Bytes& source, const SwordDef& s, Bytes& out, std::string& error,
                 std::string* report = nullptr, SwordExtra* extra = nullptr);
// loot armour (notes/loot.md): a piece of the game's armour on one of Link's spare models that follow his body: the head
// (slot 0: "katsura", his casual clothes' hair, on his head joint: solid, yamu is see-through) or the chest (slot 1: "pring", the runtime moves it to his chest joint)
struct ArmorDef {
    const char* id;
    const char* name;
    const char* archive;
    const char* bfres;
    const char* model;
    int shapes[8];        // up to the first -1
    const char* texture;  // one picture
    int rot;              // its own axes onto Link's joint (x up, y front, z side): 0 the same, 1 (x, z, -y), 2 (y, z, x)
    float scale;
    float off[3];         // on the joint: up, front, side
    bool hideCap;         // Link's cap hidden and his casual-clothes hair made part of the piece
    int defence;          // percent of each hit taken off
    bool hideHair;        // a full helmet: only the back of the casual-clothes hair shown (the head has no back of its own; the fringe stays: no forehead either)
    float flatten;        // 0..1: its normals turned away from straight up (less of the sky's blue on its shaded side)
};
const std::vector<ArmorDef>& armor_list(int slot);
const ArmorDef* armor_def(int slot, const std::string& id);
// (slot 2: outfits, a whole body on Link's own bones; make_outfit)
bool armor_source(const std::string& gameDir, const ArmorDef& a, Bytes& bfres, std::string& error);
bool armor_icon(const std::string& gameDir, const ArmorDef& def, int size, std::vector<uint8_t>& rgba, std::string& error);  // RGBA, size x size: from the front
bool make_armor(const Bytes& link, const Bytes& source, const ArmorDef& def, int slot, Bytes& out, std::string& error,
                std::string* report = nullptr, SwordExtra* extra = nullptr);
// an outfit (notes/loot.md): another character's skinned mesh bound to Link's bones in his body model, its picture beside his
// own in one larger picture kept in the corner (Link's other shapes' texture coordinates squeezed to make room)
bool make_outfit(const Bytes& link, const Bytes& source, const ArmorDef& def, Bytes& out, std::string& error, std::string* report = nullptr,
                 SwordExtra* extra = nullptr, bool real = false);
}  // namespace gcmodel
