// Loot swords (hd_sword.h): a port of tools/gcmodels/hd_sword.py.
#include "hd_sword.h"

#include <dirent.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <map>
#include <functional>
#include <cstdlib>

#include "gcm_bfres.h"
#include "gcm_image.h"
#include "gcm_j3d.h"

namespace gcmodel {
namespace {
// the swords (positions measured in the game's files with hd_sword.py; tint from the sword's own material: konst3)
// (weakest first; a mace or a spear held by its handle like a sword: "guard" is where its head or shaft starts)
const std::vector<SwordDef> kSwords = {
    {"boko", "Boko stick", "Boko.szs", "Boko.bfres", "boko", {0, -1}, true, 2, -87, -30, 1.0f, false, "boko", {-1, 0, 0}, 75},
    {"machete", "Machete", "Nata.szs", "Nata.bfres", "bk_nata", {0, -1}, true, 2, -91, -50, 1.0f, false, "bk_nata", {-1, 0, 0}, 100},
    {"spear", "Moblin spear", "Spear.szs", "Spear.bfres", "mo_yari", {0, -1}, true, 2, -176, -150, 1.5f, false, "mo_yari", {-1, 0, 0}, 125},
    {"mace", "Stalfos mace", "Club.szs", "Club.bfres", "st_buki", {0, -1}, true, 2, -119, 15, 1.15f, false, "st_buki", {-1, 0, 0}, 150},
    {"darknut", "Darknut sword", "Tkwn.szs", "Tkwn.bfres", "tn_ken1", {0, -1}, true, 2, -67, 20, 1.3f, false, "tn_ken1", {-1, 0, 0}, 175},
    {"pgsword", "Phantom Ganon's sword", "Pgsw.szs", "Pgsw.bfres", "bpg_ken1", {0, -1}, true, 2, -130, -40, 1.2f, false, "pg_ken1_1",
     {0.10f, 0.36f, 0.61f}, 200},
    {"ganondorf", "Ganondorf's sword", "Gnd.szs", "Gnd.bfres", "gnd", {6, 7, -1}, true, 0, -22.5f, 64, 1.3f, false, "ganon_ken_all", {-1, 0, 0}, 250, 0, {}, true},
    // the funny ones: Link's own things and Grandma's house (her room's mesh: pieces picked by their place)
    {"baton", "Wind Waker", "Link.szs", "Link.bfres", "takt", {0, -1}, true, 0, 2.9f, 17, 0.5f, false, "taktCI4", {-1, 0, 0}, 40},
    {"leaf", "Deku Leaf", "Link.szs", "Link.bfres", "fan", {0, -1}, true, -1, 0.0f, 0.1f, 1.3f, false, "fanBS3TC", {-1, 0, 0}, 50},
    {"hammer", "Skull Hammer", "Link.szs", "Link.bfres", "hammer", {0, -1}, true, 2, -81.8f, 20, 1.3f, false, "hammerCI8", {-1, 0, 0}, 200},
    {"knife", "Kitchen knife", "LinkRM_Room0.szs", "Room0.bfres", "model", {9, -1}, true, 1, -251.1f, -262, 0.8f, false, "Txo_yari_old",
     {-1, 0, 0}, 70, 1, {{-4.1f, -293.0f, -512.6f}}},
    {"smallknife", "Small knife", "LinkRM_Room0.szs", "Room0.bfres", "model", {9, -1}, true, 1, -234.0f, -244, 0.6f, false, "Txo_yari_old",
     {-1, 0, 0}, 50, 1, {{22.8f, -268.6f, -514.2f}}},
    {"ladle", "Ladle", "LinkRM_Room0.szs", "Room0.bfres", "model", {9, -1}, true, 1, -155.2f, -262, 1.0f, false, "Txo_yari_old",
     {-1, 0, 0}, 60, 1, {{331.3f, -222.8f, -590.1f}}},
    {"pot", "Grandma's pot", "LinkRM_Room0.szs", "Room0.bfres", "model", {9, -1}, true, 1, -269.3f, -331, 1.0f, false, "Txo_yari_old",
     {-1, 0, 0}, 90, 2, {{338.0f, -308.7f, -580.5f}, {333.5f, -353.5f, -589.8f}}},
    // Outset and the sea: Orca's training spear, a signpost, a seagull (by a wing)
    {"orcaspear", "Orca's spear", "Ji.szs", "Ji.bfres", "ji_yari", {0, -1}, true, 2, -113.8f, -90, 1.5f, false, "yari", {-1, 0, 0}, 130},
    {"sign", "Signpost", "Kanban.szs", "Kanban.bfres", "kanban", {0, -1}, true, 1, -68.4f, -35, 1.2f, false, "Txo_kanban", {-1, 0, 0}, 80},
    {"seagull", "Seagull", "Kamome.szs", "Kamome.bfres", "ka", {0, -1}, true, 0, -120.0f, -100, 1.3f, false, "ka_tex1", {-1, 0, 0}, 30},
    // every other model of the game that can be held (tools/gcmodels/loot_survey.py)
#include "hd_sword_found.inc"
};
// the loot shields (the same rows: `length` its size against the Hero's Shield, `power` unused; axis / pommel / guard
// unused: placed by its own main directions)
const std::vector<SwordDef> kShields = {
    {"darknut", "Darknut shield", "Tn.szs", "Tn.bfres", "tn_tate1", {0, -1}, true, -1, 0, 0, 1.25f, false, "tn_tate1", {-1, 0, 0}, 100},
    {"plate", "Grandma's plate", "Mshokki.szs", "Mshokki.bfres", "osara", {0, -1}, true, -1, 0, 0, 1.0f, false, "B_Sara", {-1, 0, 0}, 100},
    {"helm", "Ship's wheel", "Kaji.szs", "Kaji.bfres", "asoda", {0, -1}, true, -1, 0, 0, 1.6f, false, "Txa_kaizoku_da", {-1, 0, 0}, 100},
    {"sign", "Signpost", "Kanban.szs", "Kanban.bfres", "kanban", {0, -1}, true, -1, 0, 0, 1.4f, false, "Txo_kanban", {-1, 0, 0}, 100},
    {"bucket", "Bucket", "Okioke.szs", "Okioke.bfres", "okioke", {0, -1}, true, -1, 0, 0, 1.0f, false, "Txo_kioke", {-1, 0, 0}, 100},
    {"pot", "Grandma's pot", "LinkRM_Room0.szs", "Room0.bfres", "model", {9, -1}, true, -1, 0, 0, 1.0f, false, "Txo_yari_old",
     {-1, 0, 0}, 100, 2, {{338.0f, -308.7f, -580.5f}, {333.5f, -353.5f, -589.8f}}},
    {"seagull", "Seagull", "Kamome.szs", "Kamome.bfres", "ka", {0, -1}, true, -1, 0, 0, 1.5f, false, "ka_tex1", {-1, 0, 0}, 100},
    {"leaf", "Deku Leaf", "Link.szs", "Link.bfres", "fan", {0, -1}, true, -1, 0, 0, 1.3f, false, "fanBS3TC", {-1, 0, 0}, 100},
    // every other model of the game that can be held
#include "hd_sword_found.inc"
};

// the loot armour, placed on Link's joints by eye (tools: scratchpad previews; x up, y front, z side)
const std::vector<ArmorDef> kHelmets = {
    {"darknut", "Darknut helmet", "Tn.szs", "Tn.bfres", "tn_kabuto1", {0, -1}, "tn_kabuto1", 1, 1.05f, {8, -9, 0}, true, 15, true, 0.8f},
    {"darknut2", "Crested Darknut helmet", "Tn.szs", "Tn.bfres", "tn_kabuto2", {0, -1}, "tn_kabuto2", 1, 0.95f, {-2, -9, 0}, true, 20, true, 0.8f},
    {"stalfos", "Stalfos helmet", "St.szs", "St.bfres", "st_hat", {0, -1}, "kabuto", 0, 1.5f, {18, -5, 0}, true, 10, false, 0.8f},
    {"wizzrobe", "Wizzrobe mask", "Wz.szs", "Wz.bfres", "wz_kamen", {0, -1}, "kamen", 0, 0.5f, {0, 0, 0}, false, 8, false, 0.8f},
    {"pointed", "Pointed hat", "Gk.szs", "Gk.bfres", "gk_hat", {0, -1}, "gk_body", 0, 1.1f, {-2, -10, 0}, true, 5, false, 0.8f},
    {"wide1", "Wide hat", "Ro.szs", "Ro.bfres", "ro_hat", {0, 1, -1}, "ro_hat", 0, 0.5f, {18, -9, 0}, true, 5, false, 0.8f},
    {"wide2", "Wide hat, second", "Ro.szs", "Ro.bfres", "ro_hat2", {0, 1, -1}, "ro_hat02", 0, 0.5f, {18, -9, 0}, true, 5, false, 0.8f},
    {"wide3", "Wide hat, third", "Ro.szs", "Ro.bfres", "ro_hat3", {0, 1, -1}, "ro_hat03", 0, 0.5f, {18, -9, 0}, true, 5, false, 0.8f},
};
const std::vector<ArmorDef> kChests = {
    {"darknut", "Darknut breastplate", "Tn.szs", "Tn.bfres", "tn_yoroi1", {0, -1}, "tn_yoroi2", 2, 0.32f, {-42, 0, 0}, false, 30},
};
const std::vector<ArmorDef> kOutfits = {
    {"ganon", "Ganondorf's coat", "Gnd.szs", "Gnd.bfres", "gnd", {4, -1}, "ganon_uwagi_all", 0, 1, {0, 0, 0}, false, 25, false, 0},
    {"darknut", "Darknut body", "Tn.szs", "Tn.bfres", "tn_main", {2, 1, 4, -1}, "tn_main1", 0, 1, {0, 0, 0}, false, 40, false, 0},
    // the game's people on the common rig, each a whole character, head and all
#include "hd_outfit_people.inc"
};
// an outfit's bones on Link's (cl's bone numbers); -1: that part left out (Link's own head and hands stay)
struct BoneRow { const char* name; int link; };
const BoneRow kGndBones[] = {
    {"gnd_sklroot", 3}, {"gnd_kosi1", 30}, {"gnd_asi_l1", -1}, {"gnd_asi_l2", -1}, {"gnd_asi_l3", -1}, {"gnd_asi_r1", -1}, {"gnd_asi_r2", -1}, {"gnd_asi_r3", -1},
    {"gnd_mune1", 3}, {"gnd_mune2", 4}, {"gnd_kata_l1", 5}, {"gnd_ude_l1", 6}, {"gnd_sode_l1_1", 6}, {"gnd_sode_l1_2", 6}, {"gnd_ude_l2", 7}, {"gnd_sode_l2_1", 7},
    {"gnd_sode_l2_2", 7}, {"gnd_ude_l3", 7}, {"gnd_kata_r1", 9}, {"gnd_ude_r1", 10}, {"gnd_sode_r1_1", 10}, {"gnd_sode_r1_2", 10}, {"gnd_ude_r2", 11}, {"gnd_sode_r2_1", 11},
    {"gnd_sode_r2_2", 11}, {"gnd_ude_r3", 11}, {"gnd_kubi1", 14}, {"gnd_atama1", 15}, {"gnd_suso_l1", 30}, {"gnd_suso_r1", 30}, {"gnd_suso1", 30}, {nullptr, 0}};
const BoneRow kTnBones[] = {
    {"tn_allroot", 3}, {"tn_sklroot", 3}, {"j_tn_kosi1", 30}, {"j_tn_asi_l1", 32}, {"j_tn_asi_l2", 33}, {"j_tn_asi_l3", 34}, {"j_tn_asi_r1", 37}, {"j_tn_asi_r2", 38},
    {"j_tn_asi_r3", 39}, {"j_tn_nuno1", 30}, {"j_tn_mune1", 4}, {"j_tn_kata_l1", 5}, {"j_tn_ude_l1", 6}, {"j_tn_ude_l2", 7}, {"j_tn_ude_l3", -1}, {"j_tn_kata_r1", 9},
    {"j_tn_ude_r1", 10}, {"j_tn_ude_r2", 11}, {"j_tn_ude_r3", -1}, {"j_tn_kubi1", -1}, {"j_tn_atama1", -1}, {nullptr, 0}};
// (the common rig of the game's people; their hands at Link's wrists, their own, Link's hidden)
const BoneRow kPeopleBones[] = {
    {"world_root", 3}, {"center", 3}, {"backbone0", 3}, {"backbone", 3}, {"backbone1", 3}, {"backbone2", 4}, {"neck", 14}, {"head", 15}, {"head2", 15},
    {"shoulderL", 5}, {"shouldeL", 5}, {"armL1", 6}, {"armL", 6}, {"armL2", 7}, {"handL", 8}, {"shoulderR", 9}, {"armR1", 10}, {"armR", 10}, {"armR2", 11}, {"handR", 12},
    {"waist", 30}, {"legL1", 32}, {"legL2", 33}, {"footL", 34}, {"legR1", 37}, {"legR2", 38}, {"footR", 39}, {nullptr, 0}};
// any other body (monsters, the Japanese-named rigs: kosi, mune, ude, asi...): a bone's name read as one of Link's joints, -2 none (it goes
// with its parent: fingers, tails, ears, weapons)
int auto_bone(const std::string& name, bool hasMune2) {
    std::string n = name;
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    for (const char* w : {"yubi", "finger", "tume", "item", "bero", "ago", "mimi", "kuti", "kuchi", "hana", "mayu", "hoho", "bousi", "hane", "sippo", "shippo", "tail", "acce", "yari", "ken", "dokuro", "earring", "seigyo", "fuku", "nuno", "suso", "sode", "kamen", "tuno", "kiba"})
        if (has(w)) return -2;
    // the side: _l / _r before a digit or the end, or a capital L / R after the word
    int side = 0;  // 1 left, 2 right
    int digit = 0;
    for (size_t i = 0; i < name.size(); i++) {
        const char c = name[i], up = i + 1 < name.size() ? name[i + 1] : 0;
        if (c == '_' && (up == 'l' || up == 'r' || up == 'L' || up == 'R') && (i + 2 == name.size() || !std::isalpha((unsigned char)name[i + 2]) || name[i + 2] == 'e')) side = (up == 'l' || up == 'L') ? 1 : 2;
        if (!side && (c == 'L' || c == 'R') && i > 0 && std::islower((unsigned char)name[i - 1]) && (i + 1 == name.size() || !std::islower((unsigned char)name[i + 1]))) side = c == 'L' ? 1 : 2;
    }
    for (size_t i = name.size(); i-- > 0;)
        if (std::isdigit((unsigned char)name[i])) { digit = name[i] - '0'; break; }
    if (!digit)  // (A, B, C after the side: udeLA, ashiRB)
        for (size_t i = 0; i + 1 < name.size(); i++)
            if ((name[i] == 'L' || name[i] == 'R') && name[i + 1] >= 'A' && name[i + 1] <= 'C' && i > 0 && std::islower((unsigned char)name[i - 1])) digit = name[i + 1] - 'A' + 1;
    const int L = side == 1 ? 0 : 1;  // (left / right joint numbers below)
    if (side) {
        if (has("koka")) return L ? 36 : 31;
        if (has("momo")) return L ? 37 : 32;
        if (has("sune")) return L ? 38 : 33;
        if (has("asi") || has("ashi")) return digit == 1 ? (L ? 37 : 32) : digit == 2 ? (L ? 38 : 33) : (L ? 39 : 34);
        if (has("leg")) return digit == 0 ? (L ? 36 : 31) : digit == 1 ? (L ? 37 : 32) : (L ? 38 : 33);
        if (has("foot")) return L ? 39 : 34;
        if (has("kata") || has("shoulder")) return L ? 9 : 5;
        if (has("ude")) return digit == 2 ? (L ? 11 : 7) : digit == 3 ? (L ? 12 : 8) : (L ? 10 : 6);
        if (has("arm")) return digit == 2 ? (L ? 11 : 7) : (L ? 10 : 6);
        if (has("tekubi") || has("hand") || n.rfind("te", 0) == 0) return L ? 12 : 8;
        return -2;
    }
    if (has("kosi") || has("koshi") || has("hip") || has("waist")) return 30;
    if (has("mune")) return digit == 2 ? 4 : digit == 1 && hasMune2 ? 3 : 4;
    if (has("chest")) return 4;
    if (has("hara") || has("stomach") || has("spin")) return 3;
    if (has("kubi") || has("neck")) return 14;
    if (has("atama") || has("head") || has("kao")) return 15;
    return -2;
}
struct OutfitSpec { const char* id; const BoneRow* bones; bool replace, twoSided; const char* textures[3]; float tint[3][3]; uint8_t base[3]; bool whole; };
const OutfitSpec kOutfitSpecs[] = {
    {"ganon", kGndBones, false, true, {"ganon_uwagi_all"}, {{1.0f, 0.32f, 0.16f}}, {96, 28, 16}},
    {"darknut", kTnBones, true, false, {"tn_main1", "tn_main2", "tn_yoroi1"}, {{0.5f, 0.15f, 0.18f}, {-1, 0, 0}, {0.6f, 0.43f, 0.34f}}},
};

// Link's sword in the hand frame (bind pose of swms + swgripms): its axis in the x/y plane, the blade's flat side
// across it (the guard wings' direction), the thin side; the pommel's and the tip's places along the axis
const double kAx[3] = {1.0 / std::hypot(1.0, 0.84), 0.84 / std::hypot(1.0, 0.84), 0};
const double kAcross[3] = {-0.84 / std::hypot(1.0, 0.84), 1.0 / std::hypot(1.0, 0.84), 0};
const double kThin[3] = {0, 0, 1};
constexpr double kPommel = -17.0, kTip = 86.0;

double dot(const Vec3& a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

struct SwShape {
    std::vector<Vec3> p, n;
    std::vector<std::array<double, 2>> u;
    std::vector<std::array<uint32_t, 3>> tris;
};

struct Attrs {
    size_t vb = 0, bl = 0;
    int stride = 0;
    size_t data = 0;
    uint32_t count = 0;
    std::vector<std::pair<std::string, int>> at;  // name, offset
    int off(const char* n) const {
        for (auto& [k, o] : at)
            if (k == n) return o;
        return -1;
    }
};
Attrs attrs(const Bfres& f, size_t fm, size_t so) {
    Attrs a;
    a.vb = f.rel(fm + 16) + f.u16(so + 0x12) * 0x20;
    const size_t al = f.rel(a.vb + 0x10);
    a.bl = f.rel(a.vb + 0x18);
    for (int k = 0; k < f.u8(a.vb + 4); k++) a.at.push_back({f.str(f.rel(al + k * 12)), f.u16(al + k * 12 + 6)});
    a.stride = f.u16(a.bl + 0xC), a.data = f.rel(a.bl + 0x14), a.count = f.u32(a.vb + 8);
    return a;
}

// a bone's world matrix (rigid shapes keep their vertices in their bone's space)
Mat bone_world(const Bfres& f, size_t fm, int index) {
    const size_t sk = f.rel(fm + 12);
    std::vector<std::pair<int, Mat>> local;  // parent, local
    std::vector<int> idx;
    for (auto& [name, bo] : f.group(f.rel(sk + 0x10))) {
        (void)name;
        const uint32_t flags = f.u32(bo + 0x10);
        Vec3 s{f.f32(bo + 0x14), f.f32(bo + 0x18), f.f32(bo + 0x1C)}, t{f.f32(bo + 0x30), f.f32(bo + 0x34), f.f32(bo + 0x38)};
        const double r[4] = {f.f32(bo + 0x20), f.f32(bo + 0x24), f.f32(bo + 0x28), f.f32(bo + 0x2C)};
        Mat m{};
        if (flags & 0x1000) {
            m = mat_srt(s, {r[0], r[1], r[2]}, t);
        } else {
            const double x = r[0], y = r[1], z = r[2], w = r[3];
            const double rot[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                                      {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                                      {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) m[i][j] = rot[i][j] * s[j];
                m[i][3] = t[i];
            }
        }
        idx.push_back(f.u16(bo + 4));
        local.push_back({f.s16(bo + 6), m});
    }
    auto find = [&](int i) -> int {
        for (size_t k = 0; k < idx.size(); k++)
            if (idx[k] == i) return (int)k;
        return -1;
    };
    Mat w = mat_identity();
    for (int k = find(index), guard = 0; k >= 0 && guard < 64; k = find(local[k].first), guard++) w = mat_mul(local[k].second, w);
    return w;
}

bool read_shape(const Bfres& f, size_t fm, int si, SwShape& s, std::string& error, bool local = false) {
    auto shapes = f.group(f.rel(fm + 20));
    if (si < 0 || si >= (int)shapes.size()) return error = "no such shape", false;
    const size_t so = shapes[si].second;
    const Attrs a = attrs(f, fm, so);
    const int po = a.off("_p0"), no = a.off("_n0"), uo = a.off("_u0");
    if (po < 0 || no < 0 || uo < 0) return error = "the sword has no positions / normals / uvs", false;
    const bool rigid = f.u8(so + 0x16) == 0;
    const Mat w = rigid && !local ? bone_world(f, fm, f.u16(so + 0x10)) : mat_identity();
    const uint32_t first = (uint32_t)s.p.size();  // (shapes added one after the other)
    for (uint32_t i = 0; i < a.count; i++) {
        const size_t o = a.data + (size_t)i * a.stride;
        Vec3 p{f.f32(o + po), f.f32(o + po + 4), f.f32(o + po + 8)}, n{f.f32(o + no), f.f32(o + no + 4), f.f32(o + no + 8)};
        s.p.push_back(mat_apply(w, p));
        s.n.push_back(rot_apply(w, n));
        s.u.push_back({f.f32(o + uo), f.f32(o + uo + 4)});
    }
    const size_t mesh = f.rel(so + 0x24);
    const uint32_t prim = f.u32(mesh), ifmt = f.u32(mesh + 4), cnt = f.u32(mesh + 8);
    if (prim != 4) return error = "the sword's mesh is not a triangle list", false;
    const size_t ib = f.rel(f.rel(mesh + 0x14) + 0x14);
    for (uint32_t i = 0; i + 2 < cnt; i += 3) {
        std::array<uint32_t, 3> t{};
        for (int k = 0; k < 3; k++) t[k] = ifmt == 9 ? f.u32(ib + 4 * (i + k)) : f.u16(ib + 2 * (i + k));
        if (t[0] < a.count && t[1] < a.count && t[2] < a.count) s.tris.push_back({t[0] + first, t[1] + first, t[2] + first});
    }
    return true;
}

// the shape's vertex buffer and first mesh replaced (appended); what isn't position / normal / uv copied from HD's
// vertex `ref` (its joint binding)
void write_shape(Bfres& f, size_t fm, int si, const SwShape& s, uint32_t ref) {
    const size_t so = f.group(f.rel(fm + 20))[si].second;
    const Attrs a = attrs(f, fm, so);
    const int po = a.off("_p0"), no = a.off("_n0"), uo = a.off("_u0");
    const size_t nv = std::max<size_t>(s.p.size(), 1);
    Bytes out;
    for (size_t i = 0; i < nv; i++) out.insert(out.end(), f.d.begin() + a.data + (size_t)ref * a.stride, f.d.begin() + a.data + (size_t)(ref + 1) * a.stride);
    for (size_t i = 0; i < s.p.size(); i++) {
        uint8_t* v = &out[i * a.stride];
        for (int k = 0; k < 3; k++) putf32(v + po + 4 * k, (float)s.p[i][k]), putf32(v + no + 4 * k, (float)s.n[i][k]);
        for (int k = 0; k < 2; k++) putf32(v + uo + 4 * k, (float)s.u[i][k]);
    }
    const size_t at = f.append(out.data(), out.size(), 0x100);
    put32(&f.d[a.vb + 8], (uint32_t)nv);
    put32(&f.d[a.bl + 4], (uint32_t)out.size());
    f.set_rel(a.bl + 0x14, at);
    const size_t mesh = f.rel(so + 0x24);
    const uint32_t ifmt = f.u32(mesh + 4);
    std::vector<uint32_t> flat;
    for (auto& t : s.tris) flat.insert(flat.end(), t.begin(), t.end());
    if (flat.empty()) flat = {0, 0, 0};
    Bytes ib(flat.size() * (ifmt == 9 ? 4 : 2));
    for (size_t i = 0; i < flat.size(); i++)
        if (ifmt == 9) put32(&ib[i * 4], flat[i]);
        else put16(&ib[i * 2], (uint16_t)flat[i]);
    const size_t iat = f.append(ib.data(), ib.size(), 0x100);
    put32(&f.d[mesh + 8], (uint32_t)flat.size());
    const size_t sub = f.rel(mesh + 0x10);
    put32(&f.d[sub], 0), put32(&f.d[sub + 4], (uint32_t)flat.size());
    for (int k = 1; k < f.u16(mesh + 0xC); k++) put32(&f.d[sub + 8 * k], (uint32_t)flat.size()), put32(&f.d[sub + 8 * k + 4], 0);
    const size_t ibuf = f.rel(mesh + 0x14);
    put32(&f.d[ibuf + 4], (uint32_t)ib.size());
    f.set_rel(ibuf + 0x14, iat);
    put32(&f.d[mesh + 0x18], 0);
    if (!s.p.empty()) {
        Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
        for (auto& p : s.p)
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
        double c[3], e[3];
        for (int k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) / 2, e[k] = (hi[k] - lo[k]) / 2;
        f.putf(so + 0x1C, (float)std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]));
        if (f.u16(so + 0x1A) == 0 && f.rel(so + 0x30))
            for (int k = 0; k < 1 + f.u16(mesh + 0xC) + 1; k++)
                for (int j = 0; j < 3; j++) f.putf(f.rel(so + 0x30) + 24 * k + 4 * j, (float)c[j]), f.putf(f.rel(so + 0x30) + 24 * k + 12 + 4 * j, (float)e[j]);
    }
}

// the first vertex bound fully to the shape's first skin bone (first index 0, first weight 1), else vertex 0
uint32_t ref_vertex(const Bfres& f, size_t fm, int si) {
    const size_t so = f.group(f.rel(fm + 20))[si].second;
    const Attrs a = attrs(f, fm, so);
    const int io = a.off("_i0"), wo = a.off("_w0");
    for (uint32_t i = 0; i < a.count; i++) {
        const size_t o = a.data + (size_t)i * a.stride;
        if (io >= 0 && f.u8(o + io) != 0) continue;
        if (wo >= 0 && f.f32(o + wo) < 0.999f) continue;
        return i;
    }
    return 0;
}

// an HD texture's level 0, decoded
bool texture_image(const Bfres& f, const std::string& name, Image& img) {
    for (auto& [n, o] : f.group(f.section(1))) {
        if (n != name) continue;
        const int w = f.u32(o + 8), h = f.u32(o + 0xC);
        const uint32_t fm = f.u32(o + 0x18), sz = f.u32(o + 0x24), tm = f.u32(o + 0x34), sw = f.u32(o + 0x38);
        const Bytes lin = gx2_untile_level0(fm, w, h, tm, sw, &f.d[f.rel(o + 0xB0)], sz);
        img = bc_decode(fm, w, h, lin);
        return img.w > 0;
    }
    return false;
}

// a greyscale (BC4/BC5: red) or colour picture into one of Link's texture slots (its size, format, mips, tiling)
bool put_texture(Bfres& f, const std::string& name, const Image& src, bool grey) {
    for (auto& [n, o] : f.group(f.section(1))) {
        if (n != name) continue;
        const int w = f.u32(o + 8), h = f.u32(o + 0xC), mips = f.u32(o + 0x14);
        const uint32_t fm = f.u32(o + 0x18), sz = f.u32(o + 0x24), msz = f.u32(o + 0x2C), tm = f.u32(o + 0x34), sw = f.u32(o + 0x38);
        const uint32_t ff = fm & 0x3F;
        Image s = src;
        for (size_t i = 0; i < (size_t)s.w * s.h; i++) {
            uint8_t* p = &s.px[i * 4];
            const uint8_t v = grey ? p[0] : (uint8_t)((p[0] * 3 + p[1] * 6 + p[2]) / 10);
            if (ff == 0x35) p[0] = v, p[1] = 255, p[2] = 0, p[3] = 255;       // BC5: intensity, opaque
            else if (ff == 0x34) p[0] = v, p[1] = v, p[2] = 0, p[3] = 255;    // BC4: intensity
            else if (grey) p[0] = v, p[1] = v, p[2] = v, p[3] = 255;          // colour slot: grey
        }
        const Image big = resize_lanczos(s, w, h);
        Bytes linear;
        for (int m = 0; m < mips; m++) {
            const int lw = std::max(w >> m, 1), lh = std::max(h >> m, 1);
            const Bytes b = bc_encode(fm, m ? resize_lanczos(big, lw, lh) : big);
            linear.insert(linear.end(), b.begin(), b.end());
        }
        Bytes image, mip;
        gx2_tile(fm, w, h, mips, tm, sw, linear, image, mip, sz, msz);
        if (image.size() < sz || (msz && mip.size() < msz)) return false;
        memcpy(&f.d[f.rel(o + 0xB0)], image.data(), sz);
        if (msz) memcpy(&f.d[f.rel(o + 0xB4)], mip.data(), msz);
        return true;
    }
    return false;
}

[[maybe_unused]] void tint(Bfres& f, size_t fm, const std::string& material, const float* rgb) {
    for (auto& [mn, mo] : f.group(f.rel(fm + 24))) {
        if (mn != material) continue;
        const size_t pl = f.rel(mo + 0x34), pd = f.rel(mo + 0x3C);
        for (int k = 0; k < f.u16(mo + 0x12); k++) {
            const size_t e = pl + k * 0x14;
            const std::string pn = f.str(f.rel(e + 0x10));
            if ((pn == "mat_color0" || pn == "mat_color1") && f.u8(e) == 0xF)
                for (int c = 0; c < 4; c++) f.putf(pd + f.u16(e + 2) + 4 * c, c < 3 ? rgb[c] : 1.0f);
        }
    }
}

// a file of a SARC on disk (the game's packs, ~46 MB) without reading all of it
bool sarc_file(const std::string& path, const std::string& name, Bytes& out) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    bool ok = false;
    uint8_t h[0x14];
    if (fread(h, 1, sizeof h, fp) == sizeof h && memcmp(h, "SARC", 4) == 0) {
        const uint32_t data = be32(&h[0xC]);
        Bytes t(data);
        fseek(fp, 0, SEEK_SET);
        if (data >= 0x14 && data < (64u << 20) && fread(t.data(), 1, data, fp) == data) {
            const size_t p = be16(&h[4]);
            const uint32_t n = p + 12 <= t.size() ? be16(&t[p + 6]) : 0;
            const size_t nodes = p + 12, names = nodes + 16 * (size_t)n + 8;
            for (uint32_t i = 0; i < n && names <= t.size(); i++) {
                const uint8_t* e = &t[nodes + 16 * i];
                const uint32_t attr = be32(e + 4), start = be32(e + 8), end = be32(e + 12);
                const size_t o = names + (attr & 0xFFFFFF) * 4;
                if (!(attr >> 24) || o >= t.size() || strncmp((const char*)&t[o], name.c_str(), t.size() - o) != 0 || end < start) continue;
                out.resize(end - start);
                ok = fseek(fp, (long)(data + start), SEEK_SET) == 0 && fread(out.data(), 1, out.size(), fp) == out.size();
                break;
            }
        }
    }
    fclose(fp);
    return ok;
}
// pieces of a shared mesh (a room's): its connected pieces (corners at one place joined), those nearest def.parts
bool keep_parts(SwShape& s, const SwordDef& def, std::string& error) {
            std::vector<uint32_t> par(s.p.size());
            for (uint32_t i = 0; i < par.size(); i++) par[i] = i;
            std::function<uint32_t(uint32_t)> find = [&](uint32_t a) { return par[a] == a ? a : par[a] = find(par[a]); };
            std::map<std::array<long long, 3>, uint32_t> at;
            for (uint32_t i = 0; i < s.p.size(); i++) {
                const std::array<long long, 3> key{std::llround(s.p[i][0] * 10), std::llround(s.p[i][1] * 10), std::llround(s.p[i][2] * 10)};
                auto it = at.find(key);
                if (it != at.end()) par[find(i)] = find(it->second);
                else at[key] = i;
            }
            for (auto& t : s.tris) par[find(t[1])] = find(t[0]), par[find(t[2])] = find(t[0]);
            std::map<uint32_t, std::pair<Vec3, Vec3>> box;  // piece: low, high corner
            for (uint32_t i = 0; i < s.p.size(); i++) {
                auto [it, fresh] = box.try_emplace(find(i), s.p[i], s.p[i]);
                if (!fresh)
                    for (int j = 0; j < 3; j++) it->second.first[j] = std::min(it->second.first[j], s.p[i][j]), it->second.second[j] = std::max(it->second.second[j], s.p[i][j]);
            }
            std::set<uint32_t> keep;
            for (int k2 = 0; k2 < def.nparts && k2 < 2; k2++) {
                double best = 1e30;
                uint32_t bestId = 0;
                for (auto& [id, b] : box) {
                    double d = 0;
                    for (int j = 0; j < 3; j++) d += std::pow((b.first[j] + b.second[j]) / 2 - def.parts[k2][j], 2);
                    if (d < best) best = d, bestId = id;
                }
                keep.insert(bestId);
            }
            SwShape kept;
            std::vector<int> map(s.p.size(), -1);
            for (auto& t : s.tris) {
                if (!keep.count(find(t[0]))) continue;
                std::array<uint32_t, 3> nt{};
                for (int j = 0; j < 3; j++) {
                    if (map[t[j]] < 0) map[t[j]] = (int)kept.p.size(), kept.p.push_back(s.p[t[j]]), kept.n.push_back(s.n[t[j]]), kept.u.push_back(s.u[t[j]]);
                    nt[j] = (uint32_t)map[t[j]];
                }
                kept.tris.push_back(nt);
            }
            if (kept.tris.empty()) return error = "the pieces were not found", false;
            s = std::move(kept);
    return true;
}

// the thing's picture: a grey one (brightness only) coloured with its tint (its own material's colour)
bool equip_picture(const Bfres& src, const SwordDef& def, Image& img, bool& grey, std::string& error) {
        // the picture: a grey one (brightness only) coloured with the sword's tint (its own material's colour)
        if (!texture_image(src, def.texture, img)) return error = std::string("no texture ") + def.texture, false;
        uint32_t sfmt = 0;
        for (auto& [n, o] : src.group(src.section(1)))
            if (n == def.texture) sfmt = src.u32(o + 0x18) & 0x3F;
        grey = sfmt == 0x34 || sfmt == 0x35;
        float bladeTint[3] = {1, 1, 1};
        if (grey) {
            for (int c = 0; c < 3; c++) bladeTint[c] = def.tint[0] >= 0 ? def.tint[c] / 0.612f : 1.0f;
            for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
                uint8_t* q = &img.px[i * 4];
                const float v = q[0];
                for (int c = 0; c < 3; c++) q[c] = (uint8_t)std::clamp(v * bladeTint[c], 0.0f, 255.0f);
                q[3] = 255;
            }
        }
    return true;
}

// the picture's slot `slot` (the one picture; the slots `linked` pointed at it in memory) and its colour tiers: 4
// colours, then the rainbow along the thing (vAlong: each corner's place 0..1) in kRainbowSteps steps
void equip_extra(Bfres& f, const Image& img, const char* slot, std::vector<const char*> linked, const SwShape& s,
                 const std::vector<float>& vAlong, SwordExtra* extra) {
        {
            extra->links.clear();
            uint32_t own = 0;
            for (auto& [n, o] : f.group(f.section(1)))
                if (n == slot) own = (uint32_t)o, extra->image = {(uint32_t)f.rel(o + 0xB0), f.u32(o + 0x24), (uint32_t)f.rel(o + 0xB4), f.u32(o + 0x2C)};
            for (auto& [n, o] : f.group(f.section(1)))
                for (const char* l : linked)
                    if (n == l) extra->links.push_back({(uint32_t)o, own});
            // the colour tiers (1..4: green, blue, purple, gold) and 12 steps round the colour wheel (the rainbow): the
            // picture's brightness in that colour, encoded as the slot's image + mips
            extra->tiers.clear();
            auto variant = [&](const float* t) {
                Image v = img;
                for (size_t i = 0; i < (size_t)v.w * v.h; i++) {
                    uint8_t* q = &v.px[i * 4];
                    const float l = (q[0] * 3 + q[1] * 6 + q[2]) / 10.0f * 1.35f;
                    for (int c = 0; c < 3; c++) q[c] = (uint8_t)std::clamp(l * t[c], 0.0f, 255.0f);
                }
                Bfres g{f.d};
                put_texture(g, slot, v, false);
                Bytes b(g.d.begin() + extra->image[0], g.d.begin() + extra->image[0] + extra->image[1]);
                if (extra->image[3]) b.insert(b.end(), g.d.begin() + extra->image[2], g.d.begin() + extra->image[2] + extra->image[3]);
                extra->tiers.push_back(std::move(b));
            };
            static const float kTier[4][3] = {{0.35f, 1.0f, 0.35f}, {0.35f, 0.6f, 1.0f}, {0.8f, 0.4f, 1.0f}, {1.0f, 0.8f, 0.3f}};
            for (auto& t : kTier) variant(t);
            // the rainbow along the sword (user: from the hilt to the tip, slower, fading): each texel's place along it
            // (0 the pommel .. 1 the tip; the triangles drawn into the picture, their corners' places in between), one
            // rainbow along the sword in kRainbowSteps steps drifting towards the tip
            std::vector<float> along((size_t)img.w * img.h, -1.0f);
            {
                std::vector<float> sum(along.size(), 0.0f);
                std::vector<uint16_t> cnt(along.size(), 0);
                for (auto& t : s.tris) {
                    double X[3], Y[3], A[3];
                    for (int j = 0; j < 3; j++) {
                        X[j] = std::fmod(std::fmod(s.u[t[j]][0], 1.0) + 1.0, 1.0) * img.w;
                        Y[j] = std::fmod(std::fmod(s.u[t[j]][1], 1.0) + 1.0, 1.0) * img.h;
                        A[j] = vAlong[t[j]];
                    }
                    const double d = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2]);
                    const int x0 = std::max(0, (int)std::floor(std::min({X[0], X[1], X[2]}))), x1 = std::min(img.w - 1, (int)std::ceil(std::max({X[0], X[1], X[2]})));
                    const int y0 = std::max(0, (int)std::floor(std::min({Y[0], Y[1], Y[2]}))), y1 = std::min(img.h - 1, (int)std::ceil(std::max({Y[0], Y[1], Y[2]})));
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            double a = 1.0 / 3, b = 1.0 / 3;
                            if (std::fabs(d) > 1e-9) {
                                a = ((Y[1] - Y[2]) * (x + 0.5 - X[2]) + (X[2] - X[1]) * (y + 0.5 - Y[2])) / d;
                                b = ((Y[2] - Y[0]) * (x + 0.5 - X[2]) + (X[0] - X[2]) * (y + 0.5 - Y[2])) / d;
                            }
                            const double c = 1 - a - b;
                            if (a < -0.05 || b < -0.05 || c < -0.05) continue;  // (a little over the edges: mips and filtering)
                            const size_t i = (size_t)y * img.w + x;
                            sum[i] += (float)(a * A[0] + b * A[1] + c * A[2]), cnt[i]++;
                        }
                }
                double mean = 0;
                size_t n = 0;
                for (size_t i = 0; i < along.size(); i++)
                    if (cnt[i]) along[i] = sum[i] / cnt[i], mean += along[i], n++;
                for (float& v : along)
                    if (v < 0) v = n ? (float)(mean / n) : 0.5f;
            }
            for (int st = 0; st < kRainbowSteps; st++) {
                Image v = img;
                for (size_t i = 0; i < (size_t)v.w * v.h; i++) {
                    uint8_t* q = &v.px[i * 4];
                    const float l = (q[0] * 3 + q[1] * 6 + q[2]) / 10.0f * 1.35f;
                    const float hue = std::fmod(6.0f * (along[i] - (float)st / kRainbowSteps) + 600.0f, 6.0f), x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
                    const float r[6][3] = {{1, x, 0}, {x, 1, 0}, {0, 1, x}, {0, x, 1}, {x, 0, 1}, {1, 0, x}};
                    const int h6 = std::min(5, (int)hue);
                    for (int c = 0; c < 3; c++) q[c] = (uint8_t)std::clamp(l * (0.3f + 0.7f * r[h6][c]), 0.0f, 255.0f);
                }
                Bfres g{f.d};
                put_texture(g, slot, v, false);
                Bytes b(g.d.begin() + extra->image[0], g.d.begin() + extra->image[0] + extra->image[1]);
                if (extra->image[3]) b.insert(b.end(), g.d.begin() + extra->image[2], g.d.begin() + extra->image[2] + extra->image[3]);
                extra->tiers.push_back(std::move(b));
            }
        }
}

}  // namespace

const std::vector<SwordDef>& sword_list() { return kSwords; }
float sword_reach(const SwordDef& s) {
    // (along the sword's axis: the root joint SWMSroot_jnt at 14.1; the tips as the swords are placed: 1.0 the game's)
    const double root = 14.1, tip = kPommel + (kTip - kPommel) * s.length;
    return (float)std::max(0.5, (tip - root) / (kTip - root));
}
float sword_tilt(const SwordDef& s) {
    // held about 45 degrees down: the tip as low as the Master Sword's (its length 1) at length r when tilted by
    // 45 - asin(sin 45 / r); which way is up in the hand: WWHD_LOOT_TILT_SIGN (+1 / -1) for tests, else +1
    static const double sign = [] {
        const char* e = getenv("WWHD_LOOT_TILT_SIGN");
        return e && atoi(e) < 0 ? -1.0 : 1.0;
    }();
    const double r = (kPommel + (kTip - kPommel) * s.length) / kTip;
    if (r <= 1.0) return 0;
    return (float)(sign * (45.0 - std::asin(std::sin(45.0 * 3.14159265358979 / 180.0) / r) * 180.0 / 3.14159265358979));
}
float sword_reach_hero(const SwordDef& s) {
    // (the Hero's Sword's root joint at the hand, its tip constant nsword_top 54.6 from it)
    const double tip = kPommel + (kTip - kPommel) * s.length;
    return (float)std::max(0.5, tip / 54.6);
}
const std::vector<SwordDef>& shield_list() { return kShields; }
const SwordDef* shield_def(const std::string& id) {
    for (auto& s : kShields)
        if (id == s.id) return &s;
    return nullptr;
}
const SwordDef* sword_def(const std::string& id) {
    for (auto& s : kSwords)
        if (id == s.id) return &s;
    return nullptr;
}

namespace {
// the picture at twice the size, down to its size with a dark outline as the game's icons have
void icon_finish(const std::vector<uint8_t>& big, int W, int size, std::vector<uint8_t>& out) {
    out.assign((size_t)size * size * 4, 0);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int sum[4] = {0, 0, 0, 0};
            for (int j = 0; j < 4; j++) {
                const uint8_t* d = &big[((size_t)(y * 2 + (j >> 1)) * W + x * 2 + (j & 1)) * 4];
                for (int c = 0; c < 3; c++) sum[c] += d[c] * d[3] / 255;
                sum[3] += d[3];
            }
            uint8_t* o = &out[((size_t)y * size + x) * 4];
            if (sum[3]) for (int c = 0; c < 3; c++) o[c] = (uint8_t)std::min(255, sum[c] * 255 / sum[3]);
            o[3] = (uint8_t)(sum[3] / 4);
        }
    // the outline: what lies within two pixels of the thing
    const std::vector<uint8_t> solid = out;
    const int R = std::max(1, size / 48);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            uint8_t* o = &out[((size_t)y * size + x) * 4];
            if (o[3] >= 200) continue;
            int near = 0;
            for (int dy = -R; dy <= R; dy++)
                for (int dx = -R; dx <= R; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < size && ny < size) near = std::max<int>(near, solid[((size_t)ny * size + nx) * 4 + 3]);
                }
            if (near > o[3]) {
                const int keep = o[3];
                for (int c = 0; c < 3; c++) o[c] = (uint8_t)(o[c] * keep / 255 + 0x28 * (255 - keep) / 255);
                o[3] = (uint8_t)near;
            }
        }
}

// the mesh drawn in software with its texture: lit from the upper left, a dark outline as the game's icons have
void render_icon(const SwShape& s, const Image& img, int size, std::vector<uint8_t>& out) {
    Vec3 mean{0, 0, 0};
    for (auto& p : s.p)
        for (int j = 0; j < 3; j++) mean[j] += p[j] / s.p.size();
    double cov[3][3] = {};
    for (auto& p : s.p)
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++) cov[a][b] += (p[a] - mean[a]) * (p[b] - mean[b]);
    auto power = [&](const Vec3* not1) {  // the main direction, off `not1`
        Vec3 v{0.31, 0.77, 0.55};
        for (int it = 0; it < 60; it++) {
            Vec3 n{0, 0, 0};
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) n[a] += cov[a][b] * v[b];
            if (not1) {
                const double d = n[0] * (*not1)[0] + n[1] * (*not1)[1] + n[2] * (*not1)[2];
                for (int a = 0; a < 3; a++) n[a] -= d * (*not1)[a];
            }
            const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l < 1e-9) break;
            for (int a = 0; a < 3; a++) v[a] = n[a] / l;
        }
        return v;
    };
    Vec3 e1 = power(nullptr), e2 = power(&e1);
    auto along = [&](const Vec3& p, const Vec3& e) { return (p[0] - mean[0]) * e[0] + (p[1] - mean[1]) * e[1] + (p[2] - mean[2]) * e[2]; };
    // the wide end up (a handle is the thinner end)
    double spread[2] = {0, 0};
    int count[2] = {0, 0};
    double lo1 = 1e30, hi1 = -1e30, lo2 = 1e30, hi2 = -1e30;
    for (auto& p : s.p) {
        const double a = along(p, e1), b = along(p, e2);
        spread[a > 0] += std::fabs(b), count[a > 0]++;
        lo1 = std::min(lo1, a), hi1 = std::max(hi1, a), lo2 = std::min(lo2, b), hi2 = std::max(hi2, b);
    }
    if (count[0] && count[1] && spread[0] / count[0] > spread[1] / count[1]) {
        for (int a = 0; a < 3; a++) e1[a] = -e1[a];
        std::swap(lo1, hi1), lo1 = -lo1, hi1 = -hi1;
    }
    Vec3 e3{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const bool longThing = hi1 - lo1 > 1.6 * (hi2 - lo2);  // (round things stay upright)
    const double turn = longThing ? 3.14159265358979 / 4 : 0, cs = std::cos(turn), sn = std::sin(turn);
    const int W = size * 2;
    struct P2 { double x, y, z; };
    std::vector<P2> q(s.p.size());
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (size_t i = 0; i < s.p.size(); i++) {
        const double a = along(s.p[i], e1), b = along(s.p[i], e2);
        q[i] = {b * cs + a * sn, a * cs - b * sn, along(s.p[i], e3)};
        x0 = std::min(x0, q[i].x), x1 = std::max(x1, q[i].x), y0 = std::min(y0, q[i].y), y1 = std::max(y1, q[i].y);
    }
    const double k = 0.86 * W / std::max({x1 - x0, y1 - y0, 1e-6});
    for (auto& p : q) p.x = W / 2.0 + (p.x - (x0 + x1) / 2) * k, p.y = W / 2.0 - (p.y - (y0 + y1) / 2) * k;
    std::vector<uint8_t> big((size_t)W * W * 4, 0);
    std::vector<float> depth((size_t)W * W, -1e30f);
    const double L[3] = {-0.42, 0.58, 0.70};
    for (auto& t : s.tris) {
        const P2& a = q[t[0]], &b = q[t[1]], &c = q[t[2]];
        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-9) continue;
        const int xa = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), xb = std::min(W - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        const int ya = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), yb = std::min(W - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        for (int y = ya; y <= yb; y++)
            for (int x = xa; x <= xb; x++) {
                const double px = x + 0.5, py = y + 0.5;
                const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area, w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area, w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = (float)(w0 * a.z + w1 * b.z + w2 * c.z);
                float& zb = depth[(size_t)y * W + x];
                if (z <= zb) continue;
                const double u = w0 * s.u[t[0]][0] + w1 * s.u[t[1]][0] + w2 * s.u[t[2]][0], v = w0 * s.u[t[0]][1] + w1 * s.u[t[1]][1] + w2 * s.u[t[2]][1];
                const int tx = ((int)std::floor(u * img.w) % img.w + img.w) % img.w, ty = ((int)std::floor(v * img.h) % img.h + img.h) % img.h;
                const uint8_t* texel = img.at(tx, ty);
                if (texel[3] < 128) continue;
                zb = z;
                double n[3] = {0, 0, 1};
                if (s.n.size() == s.p.size()) {
                    Vec3 m{0, 0, 0};
                    for (int j = 0; j < 3; j++) m[j] = w0 * s.n[t[0]][j] + w1 * s.n[t[1]][j] + w2 * s.n[t[2]][j];
                    const double db = m[0] * e2[0] + m[1] * e2[1] + m[2] * e2[2], da = m[0] * e1[0] + m[1] * e1[1] + m[2] * e1[2];
                    n[0] = db * cs + da * sn, n[1] = da * cs - db * sn, n[2] = m[0] * e3[0] + m[1] * e3[1] + m[2] * e3[2];
                    if (n[2] < 0) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];  // (both sides)
                }
                const double lit = std::min(1.25, 0.62 + 0.62 * std::max(0.0, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]));
                uint8_t* d = &big[((size_t)y * W + x) * 4];
                for (int j = 0; j < 3; j++) d[j] = (uint8_t)std::min(255.0, texel[j] * lit);
                d[3] = 255;
            }
    }
    icon_finish(big, W, size, out);
}
}  // namespace

bool weapon_icon(const std::string& gameDir, const SwordDef& def, int size, std::vector<uint8_t>& rgba, std::string& error) {
    Bytes bytes;
    if (!sword_source(gameDir, def, bytes, error)) return false;
    Bfres src{bytes};
    if (src.d.size() < 0x40 || memcmp(src.d.data(), "FRES", 4)) return error = "not a BFRES", false;
    size_t sfm = 0;
    for (auto& [n, o] : src.group(src.section(0)))
        if (n == def.model) sfm = o;
    if (!sfm) return error = std::string("no model ") + def.model, false;
    SwShape s;
    for (int si : def.shapes) {
        if (si < 0) break;
        if (!read_shape(src, sfm, si, s, error, def.local)) return false;
    }
    if (s.p.empty()) return error = "no vertices", false;
    if (def.nparts > 0 && !keep_parts(s, def, error)) return false;
    Image img;
    bool grey = false;
    if (!equip_picture(src, def, img, grey, error) || img.w <= 0 || img.h <= 0) return false;
    render_icon(s, img, size, rgba);
    return true;
}

namespace {
// a piece of armour or a whole character, each shape with its own picture: from the front, turned a little, standing up (the things held have render_icon)
struct FigurePart { SwShape s; const Image* img; };
void render_figure(const std::vector<FigurePart>& parts, int size, std::vector<uint8_t>& out) {
    const double turn = -0.42, cs = std::cos(turn), sn = std::sin(turn);
    const auto view = [&](const Vec3& v) { return Vec3{v[0] * cs + v[2] * sn, v[1], v[2] * cs - v[0] * sn}; };
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    for (auto& part : parts)
        for (auto& p : part.s.p) {
            const Vec3 v = view(p);
            x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), y0 = std::min(y0, v[1]), y1 = std::max(y1, v[1]);
        }
    const int W = size * 2;
    const double k = 0.9 * W / std::max({x1 - x0, y1 - y0, 1e-6});
    std::vector<uint8_t> big((size_t)W * W * 4, 0);
    std::vector<float> depth((size_t)W * W, -1e30f);
    const double L[3] = {-0.42, 0.58, 0.70};
    struct P2 { double x, y, z; };
    for (auto& part : parts) {
        const SwShape& s = part.s;
        const Image& img = *part.img;
        std::vector<P2> q(s.p.size());
        for (size_t i = 0; i < s.p.size(); i++) {
            const Vec3 v = view(s.p[i]);
            q[i] = {W / 2.0 + (v[0] - (x0 + x1) / 2) * k, W / 2.0 - (v[1] - (y0 + y1) / 2) * k, v[2]};
        }
        for (auto& t : s.tris) {
            const P2& a = q[t[0]], &b = q[t[1]], &c = q[t[2]];
            const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            if (std::fabs(area) < 1e-9) continue;
            const int xa = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), xb = std::min(W - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
            const int ya = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), yb = std::min(W - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
            for (int y = ya; y <= yb; y++)
                for (int x = xa; x <= xb; x++) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area, w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area, w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float z = (float)(w0 * a.z + w1 * b.z + w2 * c.z);
                    float& zb = depth[(size_t)y * W + x];
                    if (z < zb) continue;
                    const double u = w0 * s.u[t[0]][0] + w1 * s.u[t[1]][0] + w2 * s.u[t[2]][0], v = w0 * s.u[t[0]][1] + w1 * s.u[t[1]][1] + w2 * s.u[t[2]][1];
                    const int tx = ((int)std::floor(u * img.w) % img.w + img.w) % img.w, ty = ((int)std::floor(v * img.h) % img.h + img.h) % img.h;
                    const uint8_t* texel = img.at(tx, ty);
                    if (texel[3] < 128) continue;
                    zb = z;
                    Vec3 m{w0 * s.n[t[0]][0] + w1 * s.n[t[1]][0] + w2 * s.n[t[2]][0], w0 * s.n[t[0]][1] + w1 * s.n[t[1]][1] + w2 * s.n[t[2]][1], w0 * s.n[t[0]][2] + w1 * s.n[t[1]][2] + w2 * s.n[t[2]][2]};
                    m = view(m);
                    const double lit = std::min(1.2, 0.72 + 0.48 * std::max(0.0, std::fabs(m[2]) * L[2] + m[0] * L[0] + m[1] * L[1]));
                    uint8_t* d = &big[((size_t)y * W + x) * 4];
                    for (int j = 0; j < 3; j++) d[j] = (uint8_t)std::min(255.0, texel[j] * lit);
                    d[3] = 255;
                }
        }
    }
    icon_finish(big, W, size, out);
}
}  // namespace

bool armor_icon(const std::string& gameDir, const ArmorDef& def, int size, std::vector<uint8_t>& rgba, std::string& error) {
    Bytes bytes;
    if (!armor_source(gameDir, def, bytes, error)) return false;
    Bfres src{bytes};
    if (src.d.size() < 0x40 || memcmp(src.d.data(), "FRES", 4)) return error = "not a BFRES", false;
    size_t sfm = 0;
    for (auto& [n, o] : src.group(src.section(0)))
        if (n == def.model) sfm = o;
    if (!sfm) return error = std::string("no model ") + def.model, false;
    std::map<std::string, Image> pictures;
    const auto picture = [&](const std::string& name) -> const Image* {
        auto it = pictures.find(name);
        if (it != pictures.end()) return it->second.w > 0 ? &it->second : nullptr;
        Image& img = pictures[name];
        if (!texture_image(src, name, img) || img.w <= 0 || img.h <= 0) return img.w = 0, nullptr;
        uint32_t fmt = 0;
        for (auto& [n, o] : src.group(src.section(1)))
            if (n == name) fmt = src.u32(o + 0x18) & 0x3F;
        if (fmt == 0x34 || fmt == 0x35)  // (brightness only)
            for (size_t i = 0; i < (size_t)img.w * img.h; i++) img.px[i * 4 + 1] = img.px[i * 4 + 2] = img.px[i * 4], img.px[i * 4 + 3] = 255;
        return &img;
    };
    const bool whole = def.shapes[0] == -2;
    std::vector<FigurePart> parts;
    const auto add = [&](size_t fm, int si, const Mat* place) {
        const auto shapes = src.group(src.rel(fm + 20));
        const auto mats = src.group(src.rel(fm + 24));
        if (si < 0 || si >= (int)shapes.size()) return;
        const size_t mo = mats[src.u16(shapes[si].second + 0xE)].second;
        const std::string own = src.u8(mo + 0x11) ? src.str(src.rel(src.rel(mo + 0x28))) : std::string();
        const Image* img = picture(!whole && def.texture && def.texture[0] ? std::string(def.texture) : own);
        if (!img) return;
        FigurePart part{{}, img};
        std::string ignored;
        if (!read_shape(src, fm, si, part.s, ignored, false)) return;
        if (place)
            for (size_t i = 0; i < part.s.p.size(); i++) part.s.p[i] = mat_apply(*place, part.s.p[i]), part.s.n[i] = rot_apply(*place, part.s.n[i]);
        parts.push_back(std::move(part));
    };
    if (whole) {
        const int n = (int)src.group(src.rel(sfm + 20)).size();
        for (int si = 0; si < n; si++) add(sfm, si, nullptr);
        const std::string names = def.texture ? def.texture : "", headModel = names.substr(0, names.find('|'));  // (a head that is a model of its own: on its joint)
        int headBone = -1;
        for (auto& [bn, bo] : src.group(src.rel(src.rel(sfm + 12) + 0x10))) if (bn == "head") headBone = src.u16(bo + 4);
        if (!headModel.empty() && headBone >= 0) {
            const Mat hw = bone_world(src, sfm, headBone);
            for (auto& [n2, o] : src.group(src.section(0)))
                if (n2 == headModel) {
                    const int hs = (int)src.group(src.rel(o + 20)).size();
                    for (int si = 0; si < hs; si++) add(o, si, &hw);
                }
        }
    } else
        for (int si : def.shapes) {
            if (si < 0) break;
            add(sfm, si, nullptr);
        }
    if (parts.empty()) return error = "nothing to draw", false;
    if (!whole)  // (a piece lies as its joint's axes: up, front, side, by its `rot`; drawn standing up, its front to us)
        for (auto& part : parts) {
            const auto stand = [&](const Vec3& v) {
                const Vec3 o = def.rot == 1 ? Vec3{v[0], v[2], -v[1]} : def.rot == 2 ? Vec3{v[1], v[2], v[0]} : v;
                return Vec3{o[2], o[0], o[1]};
            };
            for (auto& v : part.s.p) v = stand(v);
            for (auto& v : part.s.n) v = stand(v);
        }
    render_figure(parts, size, rgba);
    return true;
}

bool sword_source(const std::string& gameDir, const SwordDef& s, Bytes& bfres, std::string& error) {
    Bytes szs;
    const std::string obj = gameDir + "/content/Common/Object/" + s.archive;
    FILE* fp = fopen(obj.c_str(), "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        szs.resize((size_t)ftell(fp));
        fseek(fp, 0, SEEK_SET);
        if (fread(szs.data(), 1, szs.size(), fp) != szs.size()) szs.clear();
        fclose(fp);
    }
    if (szs.empty()) {  // (a room's or stage's archive)
        const std::string st = gameDir + "/content/Common/Stage/" + s.archive;
        if (FILE* sp = fopen(st.c_str(), "rb")) {
            fseek(sp, 0, SEEK_END);
            szs.resize((size_t)ftell(sp));
            fseek(sp, 0, SEEK_SET);
            if (fread(szs.data(), 1, szs.size(), sp) != szs.size()) szs.clear();
            fclose(sp);
        }
    }
    if (szs.empty()) {
        const std::string dir = gameDir + "/content/Common/Pack";
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                const std::string n = e->d_name;
                if (n.size() < 6 || n.compare(n.size() - 5, 5, ".pack") != 0 || n.find("permanent") == std::string::npos) continue;
                if (sarc_file(dir + "/" + n, s.archive, szs)) break;
            }
            closedir(d);
        }
    }
    if (szs.empty()) return error = std::string(s.archive) + " not found in the game's files", false;
    try {
        auto files = sarc_read(memcmp(szs.data(), "Yaz0", 4) == 0 ? yaz0_decode(szs) : szs);
        auto it = files.find(s.bfres);
        if (it == files.end()) return error = std::string(s.bfres) + " not in " + s.archive, false;
        bfres = std::move(it->second);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}

bool make_sword(const Bytes& link, const Bytes& source, const SwordDef& def, Bytes& out, std::string& error, std::string* report,
                SwordExtra* extra) {
    try {
        Bfres f{link}, src{source};
        if (f.d.size() < 0x40 || memcmp(f.d.data(), "FRES", 4) || src.d.size() < 0x40 || memcmp(src.d.data(), "FRES", 4))
            return error = "not a BFRES", false;
        size_t sfm = 0, swms = 0, grip = 0, swa = 0, gripa = 0;
        for (auto& [n, o] : src.group(src.section(0)))
            if (n == def.model) sfm = o;
        for (auto& [n, o] : f.group(f.section(0))) {
            if (n == "swms") swms = o;
            if (n == "swgripms") grip = o;
            if (n == "swa") swa = o;
            if (n == "swgripa") gripa = o;
        }
        if (!sfm) return error = std::string("no model ") + def.model, false;
        if (!swms || !grip) return error = "Link's Master Sword not found", false;
        SwShape s;
        size_t firstTris = 0;  // (two shapes: the first one's triangles are the blade, the second's the hilt)
        for (int si : def.shapes) {
            if (si < 0) break;
            if (!read_shape(src, sfm, si, s, error, def.local)) return false;
            if (!firstTris) firstTris = s.tris.size();
        }
        const bool byShape = def.byShape;
        if (s.p.empty()) return error = "the sword has no vertices", false;
        if (def.nparts > 0) {
            if (!keep_parts(s, def, error)) return false;
            firstTris = s.tris.size();
        }
        // its own main direction: the points turned so that it lies along x (pommel / guard: fractions along it)
        SwordDef dd = def;
        if (def.axis < 0) {
            Vec3 mean{0, 0, 0};
            for (auto& p : s.p)
                for (int j = 0; j < 3; j++) mean[j] += p[j] / s.p.size();
            double cov[3][3] = {};
            for (auto& p : s.p)
                for (int a2 = 0; a2 < 3; a2++)
                    for (int b2 = 0; b2 < 3; b2++) cov[a2][b2] += (p[a2] - mean[a2]) * (p[b2] - mean[b2]);
            // (power iterations: the main direction, then the next one across it)
            auto main_dir = [&](const double* not1) {
                double v[3] = {1, 0.7, 0.3};
                for (int it = 0; it < 64; it++) {
                    double w[3] = {0, 0, 0};
                    for (int a2 = 0; a2 < 3; a2++)
                        for (int b2 = 0; b2 < 3; b2++) w[a2] += cov[a2][b2] * v[b2];
                    if (not1) {
                        const double d = w[0] * not1[0] + w[1] * not1[1] + w[2] * not1[2];
                        for (int j = 0; j < 3; j++) w[j] -= d * not1[j];
                    }
                    const double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                    if (l < 1e-12) break;
                    for (int j = 0; j < 3; j++) v[j] = w[j] / l;
                }
                return std::array<double, 3>{v[0], v[1], v[2]};
            };
            auto u = main_dir(nullptr);
            if (u[0] + u[1] + u[2] < 0) for (double& x : u) x = -x;
            auto v2 = main_dir(u.data());
            const std::array<double, 3> w3{u[1] * v2[2] - u[2] * v2[1], u[2] * v2[0] - u[0] * v2[2], u[0] * v2[1] - u[1] * v2[0]};
            double lo2 = 1e30, hi2 = -1e30;
            for (auto& p : s.p) {
                const Vec3 q = p;
                p = {q[0] * u[0] + q[1] * u[1] + q[2] * u[2], q[0] * v2[0] + q[1] * v2[1] + q[2] * v2[2], q[0] * w3[0] + q[1] * w3[1] + q[2] * w3[2]};
                lo2 = std::min(lo2, p[0]), hi2 = std::max(hi2, p[0]);
            }
            for (auto& n : s.n) {
                const Vec3 q = n;
                n = {q[0] * u[0] + q[1] * u[1] + q[2] * u[2], q[0] * v2[0] + q[1] * v2[1] + q[2] * v2[2], q[0] * w3[0] + q[1] * w3[1] + q[2] * w3[2]};
            }
            dd.axis = 0, dd.pommel = (float)(lo2 + (hi2 - lo2) * def.pommel), dd.guard = (float)(lo2 + (hi2 - lo2) * def.guard);
            if (def.pommel < 0) {  // (the handle at its thinner end: the spread across near each end)
                const double len = hi2 - lo2, edge = len * 0.15;
                double a0 = 1e30, a1 = -1e30, b0 = 1e30, b1 = -1e30, c0 = 1e30, c1 = -1e30, d0 = 1e30, d1 = -1e30;
                for (auto& p : s.p) {
                    if (p[0] <= lo2 + edge) a0 = std::min(a0, p[1]), a1 = std::max(a1, p[1]), b0 = std::min(b0, p[2]), b1 = std::max(b1, p[2]);
                    if (p[0] >= hi2 - edge) c0 = std::min(c0, p[1]), c1 = std::max(c1, p[1]), d0 = std::min(d0, p[2]), d1 = std::max(d1, p[2]);
                }
                const double wLow = std::max(a1 - a0, b1 - b0), wHigh = std::max(c1 - c0, d1 - d0);
                const bool lowThin = wLow <= wHigh;
                dd.pommel = (float)(lowThin ? lo2 : hi2);
                dd.guard = (float)(lowThin ? lo2 + len * def.guard : hi2 - len * def.guard);
            }
        }
        // the source's axes: long (given), wide (the larger spread across), thin
        const int k = dd.axis;
        int o1 = (k + 1) % 3, o2 = (k + 2) % 3;
        double lo[3] = {1e9, 1e9, 1e9}, hi[3] = {-1e9, -1e9, -1e9};
        for (auto& p : s.p)
            for (int j = 0; j < 3; j++) lo[j] = std::min(lo[j], p[j]), hi[j] = std::max(hi[j], p[j]);
        const int wide = hi[o1] - lo[o1] >= hi[o2] - lo[o2] ? o1 : o2, thin = wide == o1 ? o2 : o1;
        const double tipAt = std::fabs(hi[k] - dd.pommel) > std::fabs(lo[k] - dd.pommel) ? hi[k] : lo[k];
        const double sign = tipAt > dd.pommel ? 1 : -1, srcLen = std::fabs(tipAt - dd.pommel);
        if (srcLen < 1) return error = "the sword has no length", false;
        const double sc = def.length * (kTip - kPommel) / srcLen, flip = def.flip ? -1 : 1;
        const double guardAlong = (dd.guard - dd.pommel) * sign;
        // the grip's middle across (between pommel and 70 % to the guard)
        double gl[2] = {1e9, 1e9}, gh[2] = {-1e9, -1e9};
        for (auto& p : s.p) {
            const double a = (p[k] - dd.pommel) * sign;
            if (a < 0 || a > guardAlong * 0.7) continue;
            gl[0] = std::min(gl[0], p[wide]), gh[0] = std::max(gh[0], p[wide]);
            gl[1] = std::min(gl[1], p[thin]), gh[1] = std::max(gh[1], p[thin]);
        }
        if (gl[0] > gh[0]) gl[0] = lo[wide], gh[0] = hi[wide], gl[1] = lo[thin], gh[1] = hi[thin];
        const double cw = (gl[0] + gh[0]) / 2, ct = (gl[1] + gh[1]) / 2;
        // a mirror (the axes' order) turns the triangles over
        double basis[3][3] = {};
        for (int r = 0; r < 3; r++) basis[r][k] = kAx[r] * sign, basis[r][wide] = kAcross[r] * flip, basis[r][thin] = kThin[r];
        const double det = basis[0][0] * (basis[1][1] * basis[2][2] - basis[1][2] * basis[2][1]) -
                           basis[0][1] * (basis[1][0] * basis[2][2] - basis[1][2] * basis[2][0]) +
                           basis[0][2] * (basis[1][0] * basis[2][1] - basis[1][1] * basis[2][0]);
        std::vector<Vec3> P(s.p.size()), N(s.n.size());
        for (size_t i = 0; i < s.p.size(); i++) {
            const auto& p = s.p[i];
            const double along = (p[k] - dd.pommel) * sign * sc + kPommel, ac = (p[wide] - cw) * sc * flip, th = (p[thin] - ct) * sc;
            for (int r = 0; r < 3; r++) P[i][r] = kAx[r] * along + kAcross[r] * ac + kThin[r] * th;
            const auto& n = s.n[i];
            Vec3 v;
            for (int r = 0; r < 3; r++) v[r] = kAx[r] * n[k] * sign + kAcross[r] * n[wide] * flip + kThin[r] * n[thin];
            const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (int r = 0; r < 3; r++) N[i][r] = l > 0 ? v[r] / l : 0;
        }
        // the grip onto the hand: HD's hilt's middle where the hand holds it (along -6..6) and the new one's there
        auto hand_middle = [](const std::vector<Vec3>& Q, std::array<double, 2>& m) {
            double a0 = 1e9, a1 = -1e9, t0 = 1e9, t1 = -1e9;
            int n = 0;
            for (int pass = 0; pass < 2 && n < 3; pass++) {
                a0 = 1e9, a1 = -1e9, t0 = 1e9, t1 = -1e9, n = 0;
                for (auto& q : Q) {
                    if (pass == 0 && std::fabs(dot(q, kAx)) >= 6) continue;
                    const double a = dot(q, kAcross), t = dot(q, kThin);
                    a0 = std::min(a0, a), a1 = std::max(a1, a), t0 = std::min(t0, t), t1 = std::max(t1, t), n++;
                }
            }
            m = {(a0 + a1) / 2, (t0 + t1) / 2};
        };
        std::vector<Vec3> hdHilt, newHilt;
        for (int si = 0; si < (int)f.group(f.rel(grip + 20)).size(); si++) {
            SwShape h;
            std::string e;
            if (read_shape(f, grip, si, h, e)) hdHilt.insert(hdHilt.end(), h.p.begin(), h.p.end());
        }
        for (size_t i = 0; i < P.size(); i++)
            if ((s.p[i][k] - dd.pommel) * sign < guardAlong) newHilt.push_back(P[i]);
        std::array<double, 2> want{}, have{};
        hand_middle(hdHilt, want), hand_middle(newHilt, have);
        for (auto& p : P)
            for (int r = 0; r < 3; r++) p[r] += kAcross[r] * (want[0] - have[0]) + kThin[r] * (want[1] - have[1]);
        // a longer sword tilted up in the hand (turned round the grip in the blade's plane): its tip no lower than the
        // Master Sword's (user: the long ones went into the ground)
        const double tilt = sword_tilt(def) * 3.14159265358979 / 180.0;
        if (tilt != 0) {
            const double c[3] = {kAcross[0] * want[0] + kThin[0] * want[1], kAcross[1] * want[0] + kThin[1] * want[1], 0};
            const double cs = std::cos(tilt), sn = std::sin(tilt);
            for (auto& p : P) {
                const double x = p[0] - c[0], y = p[1] - c[1];
                p[0] = c[0] + x * cs - y * sn, p[1] = c[1] + x * sn + y * cs;
            }
            for (auto& n : N) {
                const double x = n[0], y = n[1];
                n[0] = x * cs - y * sn, n[1] = x * sn + y * cs;
            }
        }
        // cut at the guard: each triangle by its middle
        SwShape blade, hilt;
        std::vector<int> mapB(P.size(), -1), mapH(P.size(), -1);
        for (size_t ti = 0; ti < s.tris.size(); ti++) {
            auto t = s.tris[ti];
            const double mid = ((s.p[t[0]][k] + s.p[t[1]][k] + s.p[t[2]][k]) / 3 - dd.pommel) * sign;
            const bool isBlade = byShape ? ti < firstTris : mid >= guardAlong;
            SwShape& dst = isBlade ? blade : hilt;
            std::vector<int>& map = isBlade ? mapB : mapH;
            if (det < 0) std::swap(t[1], t[2]);
            std::array<uint32_t, 3> nt{};
            for (int j = 0; j < 3; j++) {
                if (map[t[j]] < 0) {
                    map[t[j]] = (int)dst.p.size();
                    dst.p.push_back(P[t[j]]), dst.n.push_back(N[t[j]]), dst.u.push_back(s.u[t[j]]);
                }
                nt[j] = (uint32_t)map[t[j]];
            }
            dst.tris.push_back(nt);
        }
        const SwShape none;
        // One colour texture for every part (the game copies material colours once at load: only textures change live,
        // user: colours didn't show): the weapon's picture in SWgripAMSCI4 (the Master Sword hilt's, 256x256 BC3); in
        // memory the other sword slots are pointed at it (SwordExtra::links, done by the runtime with the loaded
        // texture's own pointers). Blade -> swms Polygon1 (material B: a colour texture, SWMSBS3TC), its other shapes
        // emptied; hilt -> swgripms Polygon0
        const auto bshapes = f.group(f.rel(swms + 20));
        int bmain = std::min<int>(1, (int)bshapes.size() - 1);
        for (int si = 0; si < (int)bshapes.size(); si++)
            if (bshapes[si].first == "Polygon1") bmain = si;
        write_shape(f, swms, bmain, blade, ref_vertex(f, swms, bmain));
        for (int si = 0; si < (int)bshapes.size(); si++)
            if (si != bmain) write_shape(f, swms, si, none, 0);
        const auto gshapes = f.group(f.rel(grip + 20));
        int gmain = 0;
        for (int si = 0; si < (int)gshapes.size(); si++)
            if (gshapes[si].first == "Polygon0") gmain = si;
        write_shape(f, grip, gmain, hilt, ref_vertex(f, grip, gmain));
        for (int si = 0; si < (int)gshapes.size(); si++)
            if (si != gmain) write_shape(f, grip, si, none, 0);
        // the same sword as the Hero's Sword (swa / swgripa: the same hand frame and grip, the same place and size)
        if (swa && gripa) {
            write_shape(f, swa, 0, blade, ref_vertex(f, swa, 0));
            for (int si = 1; si < (int)f.group(f.rel(swa + 20)).size(); si++) write_shape(f, swa, si, none, 0);
            write_shape(f, gripa, 0, hilt, 0);
            for (int si = 1; si < (int)f.group(f.rel(gripa + 20)).size(); si++) write_shape(f, gripa, si, none, 0);
        }
        Image img;
        bool grey = false;
        if (!equip_picture(src, def, img, grey, error)) return false;
        if (!put_texture(f, "SWgripAMSCI4", img, false)) return error = "texture slot", false;
        if (extra) {
            std::vector<float> vAlong(s.p.size());
            for (size_t i = 0; i < s.p.size(); i++) vAlong[i] = (float)std::clamp((s.p[i][k] - dd.pommel) * sign / srcLen, 0.0, 1.0);
            equip_extra(f, img, "SWgripAMSCI4", {"SWMSBS3TC", "SWAS3TC", "SWgripA"}, s, vAlong, extra);
        }
        if (report) {
            char b[200];
            snprintf(b, sizeof b, "%s: %zu vertices (blade %zu, hilt %zu), scale %.3f%s%s", def.id, s.p.size(), blade.p.size(),
                     hilt.p.size(), sc, det < 0 ? ", mirrored" : "", grey ? ", grey picture coloured" : "");
            *report = b;
        }
        out = std::move(f.d);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}
// ---- loot shields: the thing in place of the Hero's Shield (sha: its face Polygon1, the inside emptied, the arm's grip
// kept) and the Mirror Shield (shms: Polygon2, the others emptied); its longest direction upright, the next one across,
// the thinnest the face's normal; sized to the Hero's Shield (44 tall, 32 wide) times def.length, its back just in front
// of the arm; one picture (SHA, the Mirror Shield's slot pointed at it), the rainbow from bottom to top
bool make_shield(const Bytes& link, const Bytes& source, const SwordDef& def, Bytes& out, std::string& error, std::string* report,
                 SwordExtra* extra) {
    try {
        Bfres f{link}, src{source};
        if (f.d.size() < 0x40 || memcmp(f.d.data(), "FRES", 4) || src.d.size() < 0x40 || memcmp(src.d.data(), "FRES", 4))
            return error = "not a BFRES", false;
        size_t sfm = 0, sha = 0, shms = 0;
        for (auto& [n, o] : src.group(src.section(0)))
            if (n == def.model) sfm = o;
        for (auto& [n, o] : f.group(f.section(0))) {
            if (n == "sha") sha = o;
            if (n == "shms") shms = o;
        }
        if (!sfm) return error = std::string("no model ") + def.model, false;
        if (!sha || !shms) return error = "Link's shields not found", false;
        SwShape s;
        for (int si : def.shapes) {
            if (si < 0) break;
            if (!read_shape(src, sfm, si, s, error, def.local)) return false;
        }
        if (s.p.empty()) return error = "the shield has no vertices", false;
        if (def.nparts > 0 && !keep_parts(s, def, error)) return false;
        // its main directions
        Vec3 mean{0, 0, 0};
        for (auto& p : s.p)
            for (int j = 0; j < 3; j++) mean[j] += p[j] / s.p.size();
        double cov[3][3] = {};
        for (auto& p : s.p)
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) cov[a][b] += (p[a] - mean[a]) * (p[b] - mean[b]);
        auto main_dir = [&](const double* not1) {
            double v[3] = {1, 0.7, 0.3};
            for (int it = 0; it < 64; it++) {
                double w[3] = {0, 0, 0};
                for (int a = 0; a < 3; a++)
                    for (int b = 0; b < 3; b++) w[a] += cov[a][b] * v[b];
                if (not1) {
                    const double d = w[0] * not1[0] + w[1] * not1[1] + w[2] * not1[2];
                    for (int j = 0; j < 3; j++) w[j] -= d * not1[j];
                }
                const double l = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                if (l < 1e-12) break;
                for (int j = 0; j < 3; j++) v[j] = w[j] / l;
            }
            return std::array<double, 3>{v[0], v[1], v[2]};
        };
        const auto u = main_dir(nullptr), v = main_dir(u.data());
        const std::array<double, 3> w{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        auto dot3 = [](const Vec3& p, const std::array<double, 3>& d) { return p[0] * d[0] + p[1] * d[1] + p[2] * d[2]; };
        double lo[3] = {1e30, 1e30, 1e30}, hi[3] = {-1e30, -1e30, -1e30};
        std::vector<std::array<double, 3>> q(s.p.size());
        for (size_t i = 0; i < s.p.size(); i++) {
            const Vec3 d{s.p[i][0] - mean[0], s.p[i][1] - mean[1], s.p[i][2] - mean[2]};
            q[i] = {dot3(d, u), dot3(d, v), dot3(d, w)};
            for (int j = 0; j < 3; j++) lo[j] = std::min(lo[j], q[i][j]), hi[j] = std::max(hi[j], q[i][j]);
        }
        const double ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
        if (ext[0] < 1e-3) return error = "the shield has no size", false;
        const double sc = def.length * std::min(44.0 / ext[0], 31.6 / std::max(ext[1], 1e-3));
        const double fl = def.flip ? -1 : 1;
        // (x across from v, y up from u, z the normal from -w: the front to -z)
        const double det = -fl;  // det of (v*fl, u, -w) with (u, v, w) right-handed: -fl
        auto place = [&](double cx, double cy, double back, SwShape& o) {
            o = SwShape{};
            for (size_t i = 0; i < s.p.size(); i++) {
                const double a = q[i][0] - (lo[0] + hi[0]) / 2, b = q[i][1] - (lo[1] + hi[1]) / 2;
                o.p.push_back({cx + b * sc * fl, cy + a * sc, back - (hi[2] - q[i][2]) * sc});
                const Vec3& n = s.n[i];
                Vec3 m{dot3(n, v) * fl, dot3(n, u), -dot3(n, w)};
                const double l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                for (int j = 0; j < 3; j++) m[j] = l > 0 ? m[j] / l : 0;
                o.n.push_back(m);
                o.u.push_back(s.u[i]);
            }
            for (auto t : s.tris) {
                if (det < 0) std::swap(t[1], t[2]);
                o.tris.push_back(t);
            }
        };
        const SwShape none;
        SwShape hero, mirror;
        place(3.8, 7.55, -1.8, hero);
        place(3.95, 5.35, -1.0, mirror);
        const auto hs = f.group(f.rel(sha + 20));
        for (int si = 0; si < (int)hs.size(); si++) {
            if (hs[si].first == "Polygon1") write_shape(f, sha, si, hero, ref_vertex(f, sha, si));
            else if (hs[si].first == "Polygon2") write_shape(f, sha, si, none, 0);
        }
        const auto ms = f.group(f.rel(shms + 20));
        for (int si = 0; si < (int)ms.size(); si++)
            write_shape(f, shms, si, ms[si].first == "Polygon2" ? mirror : none, ms[si].first == "Polygon2" ? ref_vertex(f, shms, si) : 0);
        Image img;
        bool grey = false;
        if (!equip_picture(src, def, img, grey, error)) return false;
        if (!put_texture(f, "SHA", img, false)) return error = "texture slot", false;
        if (extra) {
            std::vector<float> vAlong(s.p.size());
            for (size_t i = 0; i < s.p.size(); i++) vAlong[i] = (float)((q[i][0] - lo[0]) / ext[0]);
            equip_extra(f, img, "SHA", {"SHMSACI4"}, s, vAlong, extra);
        }
        if (report) {
            char b[160];
            snprintf(b, sizeof b, "%s: %zu vertices, %.0f x %.0f x %.0f, scale %.3f", def.id, s.p.size(), ext[0], ext[1], ext[2], sc);
            *report = b;
        }
        out = std::move(f.d);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}

const std::vector<ArmorDef>& armor_list(int slot) { return slot == 2 ? kOutfits : slot ? kChests : kHelmets; }
const ArmorDef* armor_def(int slot, const std::string& id) {
    for (auto& a : armor_list(slot))
        if (id == a.id) return &a;
    return nullptr;
}
bool armor_source(const std::string& gameDir, const ArmorDef& a, Bytes& bfres, std::string& error) {
    SwordDef s{};
    s.archive = a.archive, s.bfres = a.bfres;
    return sword_source(gameDir, s, bfres, error);
}

namespace { void set_indices(Bfres& f, size_t fm, int si, std::vector<uint32_t> flat); }
// a piece of armour in place of "yamu" (head) or "pring" (chest): every corner moved into the model's space (a
// one-bone-per-corner skin keeps its corners in their bone's space), turned and sized onto Link's joint
bool make_armor(const Bytes& link, const Bytes& source, const ArmorDef& def, int slot, Bytes& out, std::string& error, std::string* report,
                SwordExtra* extra) {
    try {
        Bfres f{link}, src{source};
        if (f.d.size() < 0x40 || memcmp(f.d.data(), "FRES", 4) || src.d.size() < 0x40 || memcmp(src.d.data(), "FRES", 4))
            return error = "not a BFRES", false;
        size_t sfm = 0, target = 0, cl = 0;
        for (auto& [n, o] : src.group(src.section(0)))
            if (n == def.model) sfm = o;
        for (auto& [n, o] : f.group(f.section(0))) {
            if (n == (slot ? "pring" : "katsura")) target = o;
            if (n == "cl") cl = o;
        }
        if (!sfm) return error = std::string("no model ") + def.model, false;
        if (!target || !cl) return error = "Link's models not found", false;
        SwShape s;
        for (int si : def.shapes) {
            if (si < 0) break;
            const size_t first = s.p.size();
            if (!read_shape(src, sfm, si, s, error)) return false;
            const size_t so = src.group(src.rel(sfm + 20))[si].second;
            if (src.u8(so + 0x16) != 1) continue;  // (0: read_shape did it; 2 and more: already in the model's space)
            const Attrs a = attrs(src, sfm, so);
            const int io = a.off("_i0");
            if (io < 0) continue;
            uint32_t fmt = 0;
            const size_t al = src.rel(a.vb + 0x10);
            for (int k = 0; k < src.u8(a.vb + 4); k++)
                if (src.str(src.rel(al + k * 12)) == "_i0") fmt = src.u32(al + k * 12 + 8);
            // (a corner's index: the skeleton's rigid matrix table, each bone's number in it at bone +0xA)
            std::map<int, int> rigid;
            for (auto& [bn, bo] : src.group(src.rel(src.rel(sfm + 12) + 0x10))) {
                (void)bn;
                rigid[src.s16(bo + 0xA)] = src.u16(bo + 4);
            }
            std::map<int, Mat> world;
            for (uint32_t i = 0; i < a.count; i++) {
                const size_t o = a.data + (size_t)i * a.stride + io;
                const int k = (fmt & 0xFF) == 5 ? (int)src.u32(o) : src.u8(o);
                const auto r = rigid.find(k);
                if (r == rigid.end()) return error = "a corner on an unknown bone", false;
                const int bone = r->second;
                if (!world.count(bone)) world[bone] = bone_world(src, sfm, bone);
                s.p[first + i] = mat_apply(world[bone], s.p[first + i]);
                s.n[first + i] = rot_apply(world[bone], s.n[first + i]);
            }
        }
        if (s.p.empty()) return error = "the armour has no vertices", false;
        auto turn = [&](const Vec3& p) -> Vec3 {
            if (def.rot == 1) return {p[0], p[2], -p[1]};
            if (def.rot == 2) return {p[1], p[2], p[0]};
            return p;
        };
        double lo = 1e30, hi = -1e30;
        for (size_t i = 0; i < s.p.size(); i++) {
            Vec3 q = turn(s.p[i]);
            for (int j = 0; j < 3; j++) q[j] = q[j] * def.scale + def.off[j];
            s.p[i] = q, s.n[i] = turn(s.n[i]);
            if (def.flatten > 0) {
                Vec3& m = s.n[i];
                m[0] *= 1.0 - def.flatten;
                const double l = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                if (l > 1e-6) m = {m[0] / l, m[1] / l, m[2] / l};
            }
            lo = std::min(lo, q[0]), hi = std::max(hi, q[0]);
        }
        // (the hair's material draws one side only: the piece's inside and underside added, a hat seen from below is not open)
        if (slot == 0) {
            const uint32_t n = (uint32_t)s.p.size();
            const size_t nt = s.tris.size();
            for (uint32_t i = 0; i < n; i++) {
                const Vec3 p = s.p[i], m = s.n[i];
                const auto uv = s.u[i];
                s.p.push_back(p), s.n.push_back({-m[0], -m[1], -m[2]}), s.u.push_back(uv);
            }
            for (size_t i = 0; i < nt; i++) {
                const auto t = s.tris[i];
                s.tris.push_back({t[2] + n, t[1] + n, t[0] + n});
            }
        }
        const SwShape none;
        Image img;
        bool grey = false;
        SwordDef pic{};
        pic.texture = def.texture, pic.tint[0] = -1;
        if (!equip_picture(src, pic, img, grey, error)) return false;
        // under a helmet that takes the cap's place: his casual-clothes hair (the head has no top or back of its own), part of
        // this mesh (its model is the slot), its corners on a patch of the hair's colour in a free place of the picture;
        // a full helmet only the back of it (the fringe stays: the face has no forehead)
        if (def.hideCap) {
            SwShape hair;
            if (!read_shape(f, target, 0, hair, error, true)) return false;
            Image hairPic;
            uint8_t col[3] = {214, 178, 40};
            if (texture_image(f, "katsuraS3TC", hairPic)) {
                uint64_t sum[3] = {};
                for (size_t i = 0; i < (size_t)hairPic.w * hairPic.h; i++)
                    for (int c = 0; c < 3; c++) sum[c] += hairPic.px[i * 4 + c];
                for (int c = 0; c < 3; c++) col[c] = (uint8_t)(sum[c] / std::max<size_t>((size_t)hairPic.w * hairPic.h, 1));
            }
            constexpr int kGrid = 64, kPatch = 3;
            std::vector<uint8_t> used(kGrid * kGrid, 0);
            for (auto& t : s.tris) {
                double lo2[2] = {1e9, 1e9}, hi2[2] = {-1e9, -1e9};
                for (int j = 0; j < 3; j++)
                    for (int k = 0; k < 2; k++) {
                        const double v = std::fmod(std::fmod(s.u[t[j]][k], 1.0) + 1.0, 1.0);
                        lo2[k] = std::min(lo2[k], v), hi2[k] = std::max(hi2[k], v);
                    }
                for (int y = (int)(lo2[1] * kGrid); y <= std::min(kGrid - 1, (int)(hi2[1] * kGrid)); y++)
                    for (int x = (int)(lo2[0] * kGrid); x <= std::min(kGrid - 1, (int)(hi2[0] * kGrid)); x++) used[y * kGrid + x] = 1;
            }
            int px = kGrid - kPatch, py = kGrid - kPatch;
            for (int y = 0, found = 0; y + kPatch <= kGrid && !found; y++)
                for (int x = 0; x + kPatch <= kGrid && !found; x++) {
                    int n = 0;
                    for (int j = 0; j < kPatch; j++)
                        for (int k = 0; k < kPatch; k++) n += used[(y + j) * kGrid + x + k];
                    if (!n) px = x, py = y, found = 1;
                }
            for (int y = py * img.h / kGrid; y < (py + kPatch) * img.h / kGrid; y++)
                for (int x = px * img.w / kGrid; x < (px + kPatch) * img.w / kGrid; x++) {
                    uint8_t* q = &img.px[((size_t)y * img.w + x) * 4];
                    q[0] = col[0], q[1] = col[1], q[2] = col[2], q[3] = 255;
                }
            const std::array<double, 2> uv{(px + kPatch / 2.0) / kGrid, (py + kPatch / 2.0) / kGrid};
            const uint32_t base = (uint32_t)s.p.size();
            for (size_t i = 0; i < hair.p.size(); i++) s.p.push_back(hair.p[i]), s.n.push_back(hair.n[i]), s.u.push_back(uv);
            for (auto& t : hair.tris)
                if (!def.hideHair || (hair.p[t[0]][1] + hair.p[t[1]][1] + hair.p[t[2]][1]) / 3 < 2.0) s.tris.push_back({t[0] + base, t[1] + base, t[2] + base});
            const auto cs = f.group(f.rel(cl + 20));
            for (int si = 0; si < (int)cs.size(); si++)
                if (cs[si].first == "link_root__ear_3_") set_indices(f, cl, si, {});  // (only its triangles: an outfit changes its corners)
        }
        const auto ts = f.group(f.rel(target + 20));
        for (int si = 0; si < (int)ts.size(); si++) write_shape(f, target, si, si == 0 ? s : none, 0);
        std::vector<float> vAlong(s.p.size());
        for (size_t i = 0; i < s.p.size(); i++) vAlong[i] = (float)std::clamp((s.p[i][0] - lo) / std::max(hi - lo, 1e-3), 0.0, 1.0);
        if (extra) {
            // (the slots' own pictures are 32 x 32: shown as a picture of VamuY's size kept in the corner)
            Bfres g{f.d};
            if (!put_texture(g, "VamuY", img, false)) return error = "texture slot", false;
            equip_extra(g, img, "VamuY", {slot ? "PRingS3TC" : "katsuraS3TC"}, s, vAlong, extra);
            extra->surface = true;
            extra->own.assign(g.d.begin() + extra->image[0], g.d.begin() + extra->image[0] + extra->image[1]);
            if (extra->image[3]) extra->own.insert(extra->own.end(), g.d.begin() + extra->image[2], g.d.begin() + extra->image[2] + extra->image[3]);
        }
        if (report) {
            char b[160];
            snprintf(b, sizeof b, "%s: %zu vertices, %.0f up, defence %d %%", def.id, s.p.size(), hi - lo, def.defence);
            *report = b;
        }
        out = std::move(f.d);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}

namespace {
// a shape's triangles replaced (appended), its 32-bit list
void set_indices(Bfres& f, size_t fm, int si, std::vector<uint32_t> flat) {
    const size_t so = f.group(f.rel(fm + 20))[si].second;
    if (flat.empty()) flat = {0, 0, 0};
    const size_t mesh = f.rel(so + 0x24);
    Bytes ib(flat.size() * 4);
    for (size_t i = 0; i < flat.size(); i++) put32(&ib[i * 4], flat[i]);
    const size_t iat = f.append(ib.data(), ib.size(), 0x100);
    put32(&f.d[mesh + 8], (uint32_t)flat.size());
    const size_t sub = f.rel(mesh + 0x10);
    put32(&f.d[sub], 0), put32(&f.d[sub + 4], (uint32_t)flat.size());
    for (int k = 1; k < f.u16(mesh + 0xC); k++) put32(&f.d[sub + 8 * k], (uint32_t)flat.size()), put32(&f.d[sub + 8 * k + 4], 0);
    const size_t ibuf = f.rel(mesh + 0x14);
    put32(&f.d[ibuf + 4], (uint32_t)ib.size());
    f.set_rel(ibuf + 0x14, iat);
    put32(&f.d[mesh + 0x18], 0);
}
struct BoneInfo { std::string name; int id, parent, smooth; Vec3 at; };
std::vector<BoneInfo> bone_infos(const Bfres& f, size_t fm) {
    std::vector<BoneInfo> out;
    for (auto& [n, bo] : f.group(f.rel(f.rel(fm + 12) + 0x10))) {
        const int id = f.u16(bo + 4);
        const Mat w = bone_world(f, fm, id);
        out.push_back({n, id, f.s16(bo + 6), f.s16(bo + 8), {w[0][3], w[1][3], w[2][3]}});
    }
    return out;
}
constexpr int kAtlasW = 768, kAtlasH = 384, kLinkW = 512;  // (the size of Link's mouth pictures: their surface is the template)
}  // namespace

bool make_outfit(const Bytes& link, const Bytes& source, const ArmorDef& def, Bytes& out, std::string& error, std::string* report, SwordExtra* extra, bool real0) {
    try {
        const OutfitSpec* spec = nullptr;
        for (auto& o : kOutfitSpecs)
            if (!strcmp(o.id, def.id)) spec = &o;
        // (the generated rows: a whole character on the common rig, its textures found from its shapes)
        static const OutfitSpec kPerson{"", kPeopleBones, true, false, {}, {}, {}, true};
        if (!spec) spec = &kPerson;
        Bfres f{link}, src{source};
        size_t sfm = 0, cl = 0;
        for (auto& [n, o] : src.group(src.section(0)))
            if (n == def.model) sfm = o;
        for (auto& [n, o] : f.group(f.section(0)))
            if (n == "cl") cl = o;
        if (!sfm || !cl) return error = "model not found", false;
        const auto lb = bone_infos(f, cl);
        auto sb = bone_infos(src, sfm);
        // a whole character's arms that are a model of their own (the Rito's: def.texture is "head|arms"): its bones added to the body's, placed
        // on the body's arm joints
        const std::string partNames = def.texture ? def.texture : "";
        const std::string headModel = partNames.substr(0, partNames.find('|')), armModel = partNames.find('|') == std::string::npos ? "" : partNames.substr(partNames.find('|') + 1);
        size_t armFm = 0;
        int bodyArm[2] = {-1, -1};
        std::map<int, int> armSmooth;
        std::map<int, int> armSide;
        if (spec->whole && !armModel.empty()) {
            for (auto& [n, o] : src.group(src.section(0))) if (n == armModel) armFm = o;
            for (auto& b : sb) { if (b.name == "armL") bodyArm[0] = b.id; if (b.name == "armR") bodyArm[1] = b.id; }
            if (armFm && bodyArm[0] >= 0 && bodyArm[1] >= 0) {
                const auto own = bone_infos(src, armFm);
                for (auto& b : own) {
                    const int side = b.name.find('R') != std::string::npos ? 1 : 0;
                    const Mat w = bone_world(src, sfm, bodyArm[side]);
                    sb.push_back({b.name, 1000 + b.id, b.parent >= 0 ? 1000 + b.parent : bodyArm[side], -1, mat_apply(w, b.at)});
                    armSide[1000 + b.id] = side;
                    if (b.smooth >= 0) armSmooth[b.smooth] = 1000 + b.id;
                }
            } else armFm = 0;
        }        std::map<int, const BoneInfo*> lById, sById;
        std::map<int, int> sBySmooth;
        for (auto& b : lb) lById[b.id] = &b;
        for (auto& b : sb) {
            sById[b.id] = &b;
            if (b.smooth >= 0 && b.id < 1000) sBySmooth[b.smooth] = b.id;
        }
        // each of its bones: Link's bone, left out or not (an unlisted one goes with its parent)
        std::map<int, int> mapped;
        std::map<int, bool> dropped;
        for (auto& b : sb) {
            int cur = b.id, link2 = 3;
            bool drop = false, found = false;
            bool mune2 = false;
            for (auto& x : sb) if (x.name.find("mune2") != std::string::npos) mune2 = true;
            for (int guard = 0; cur >= 0 && guard < 64 && !found; guard++) {
                for (const BoneRow* r = spec->bones; r->name; r++)
                    if (sById[cur]->name == r->name) link2 = r->link, found = true;
                if (!found && spec->whole) {
                    const int a = auto_bone(sById[cur]->name, mune2);
                    if (a >= 0) link2 = a, found = true;
                }
                if (!found) cur = sById[cur]->parent;
            }
            // (a head bone of another name, on a model's origin: sv_head: as the head)
            if (spec->whole && b.name != "head" && b.name.find("head") != std::string::npos) link2 = 15, drop = false;
            // (what the character carries is not part of it)
            if (spec->whole)
                for (const char* w : {"yari", "ken", "buki", "tate", "stick", "dagger", "item"})
                    if (b.name.find(w) != std::string::npos) drop = true;
            if (link2 < 0) {
                drop = true;
                // (its place still from the nearest kept parent's bone)
                link2 = 15;
            }
            mapped[b.id] = link2, dropped[b.id] = drop;
        }
        // each bone's size against Link's: the bone to its first child on another of Link's bones
        std::map<int, double> sizeOf;
        auto dist = [](const Vec3& a, const Vec3& c) { return std::sqrt(std::pow(a[0] - c[0], 2) + std::pow(a[1] - c[1], 2) + std::pow(a[2] - c[2], 2)); };
        for (auto& b : sb) {
            double size = 0;
            for (auto& c : sb)
                if (c.parent == b.id && mapped[c.id] != mapped[b.id] && !dropped[c.id]) {
                    const double ds = dist(c.at, b.at), dl = dist(lById[mapped[c.id]]->at, lById[mapped[b.id]]->at);
                    if (ds > 1e-3 && dl > 1e-3) size = dl / ds;
                    break;
                }
            sizeOf[b.id] = size;
        }
        for (auto& b : sb) {
            int cur = b.id;
            for (int guard = 0; sizeOf[cur] <= 0 && guard < 64; guard++) {
                if (sById[cur]->parent < 0) break;
                cur = sById[cur]->parent;
            }
            sizeOf[b.id] = std::clamp(sizeOf[cur] > 0 ? sizeOf[cur] : 0.31, 0.2, 0.5);
        }
        // the mesh on Link's bones, up to three of them each corner
        struct V { Vec3 p, n; std::array<double, 2> u; uint32_t idx[3]; float w[3]; };
        struct Raw { Vec3 p, n; std::array<double, 2> u; std::vector<std::pair<float, int>> ws; };
        std::vector<V> vs;
        std::vector<std::array<uint32_t, 3>> tris;
        std::set<int> usedBones;
        const auto sShapes = src.group(src.rel(sfm + 20));
        const auto mats = src.group(src.rel(sfm + 24));
        std::map<int, int> sByRigid;
        for (auto& [bn, bo] : src.group(src.rel(src.rel(sfm + 12) + 0x10))) {
            (void)bn;
            if (src.s16(bo + 0xA) >= 0) sByRigid[src.s16(bo + 0xA)] = src.u16(bo + 4);
        }
        // (its shapes: of the body model, and of the model that is its head, if that is one of its own: def.texture names it)
        struct Item { size_t fm; int si; size_t so; bool head; bool arm = false; };
        std::vector<Item> items;
        auto shapeTexture = [&](const Item& it) -> std::string {
            const auto ms = src.group(src.rel(it.fm + 24));
            const size_t mo = ms[src.u16(it.so + 0xE)].second;
            return src.u8(mo + 0x11) ? src.str(src.rel(src.rel(mo + 0x28))) : std::string();
        };
        // which shapes, which textures (up to three, the biggest first: one row each in the picture)
        if (def.shapes[0] == -2) for (int si = 0; si < (int)sShapes.size(); si++) items.push_back({sfm, si, sShapes[si].second, false});
        else for (int si : def.shapes) { if (si < 0) break; items.push_back({sfm, si, sShapes[si].second, false}); }
        int headBone = -1;
        for (auto& b : sb) if (b.name == "head") headBone = b.id;
        if (armFm) {
            const auto as = src.group(src.rel(armFm + 20));
            for (int si = 0; si < (int)as.size(); si++) items.push_back({armFm, si, as[si].second, false, true});
        }
        if (spec->whole && !headModel.empty() && headBone >= 0)
            for (auto& [n, o] : src.group(src.section(0)))
                if (n == headModel) {
                    const auto hs = src.group(src.rel(o + 20));
                    for (int si = 0; si < (int)hs.size(); si++) items.push_back({o, si, hs[si].second, true});
                }
        std::vector<std::string> texNames;
        std::vector<std::array<float, 3>> texTint;
        if (spec->textures[0]) {
            for (int k = 0; k < 3 && spec->textures[k]; k++) texNames.push_back(spec->textures[k]), texTint.push_back({spec->tint[k][0], spec->tint[k][1], spec->tint[k][2]});
        } else {
            std::map<std::string, uint32_t> count;
            for (auto& it : items) {
                const std::string t = shapeTexture(it);
                if (!t.empty()) count[t] += attrs(src, it.fm, it.so).count;
            }
            std::vector<std::pair<uint32_t, std::string>> order;
            for (auto& [t, n] : count) order.push_back({n, t});
            std::sort(order.rbegin(), order.rend());
            for (size_t k = 0; k < order.size() && k < 6; k++) texNames.push_back(order[k].second), texTint.push_back({-1, 0, 0});  // (eyes, brows and mouth are the small ones: all kept)
        }
        if (texNames.empty()) return error = "the outfit has no texture", false;
        const int nameCount = (int)texNames.size();
        std::vector<int> rowY(nameCount), rowHs(nameCount);
        {
            const int small = nameCount > 1 ? std::min(64, kAtlasH / (2 * (nameCount - 1))) : 64;
            const bool named = spec->textures[0] != nullptr;
            for (int k = 0, y = 0; k < nameCount; k++) {
                rowHs[k] = named ? kAtlasH / nameCount : (k == 0 ? kAtlasH - (nameCount - 1) * small : small);
                rowY[k] = y, y += rowHs[k];
            }
        }
        struct Part { std::vector<Raw> v; std::vector<std::array<uint32_t, 3>> tris; int row; };
        std::vector<Part> parts;
        for (auto& it : items) {
            const int si = it.si;
            const std::string tn = shapeTexture(it);
            int row = -1;
            for (int k = 0; k < nameCount; k++)
                if (tn == texNames[k]) row = k;
            if (row < 0) {
                if (spec->textures[0]) return error = "the outfit's texture is not listed: " + tn, false;
                continue;
            }
            SwShape t;
            if (!read_shape(src, it.fm, si, t, error, true)) return false;
            const size_t so = it.so;
            const Attrs a = attrs(src, it.fm, so);
            const int io = a.off("_i0"), wo = a.off("_w0"), skin = src.u8(so + 0x16);
            if (it.arm) {
                if (skin < 2 || io < 0 || wo < 0) continue;
                Part part;
                part.row = row, part.tris = t.tris, part.v.resize(a.count);
                for (uint32_t i = 0; i < a.count; i++) {
                    const size_t o = a.data + (size_t)i * a.stride;
                    Raw& r = part.v[i];
                    for (int k = 0; k < std::min(skin, 4); k++) {
                        const float w = src.f32(o + wo + 4 * k);
                        const auto bi = armSmooth.find((int)src.u32(o + io + 4 * k));
                        if (w > 0 && bi != armSmooth.end()) r.ws.push_back({w, bi->second});
                    }
                    if (r.ws.empty()) r.ws.push_back({1.0f, bodyArm[0]});
                    std::sort(r.ws.begin(), r.ws.end(), [](auto& x, auto& y) { return x.first > y.first; });
                    const auto sd = armSide.find(r.ws[0].second);
                    const Mat w = bone_world(src, sfm, bodyArm[sd != armSide.end() ? sd->second : 0]);
                    r.p = mat_apply(w, t.p[i]), r.n = rot_apply(w, t.n[i]), r.u = t.u[i];
                }
                parts.push_back(std::move(part));
                continue;
            }
            if (it.head) {
                // (a head model: in its joint's space; all of it on that joint)
                const Mat hw = bone_world(src, sfm, headBone);
                const Mat own = skin == 0 ? bone_world(src, it.fm, src.u16(so + 0x10)) : mat_identity();
                Part part;
                part.row = row, part.tris = t.tris, part.v.resize(a.count);
                for (uint32_t i = 0; i < a.count; i++) {
                    Raw& r = part.v[i];
                    r.p = mat_apply(hw, mat_apply(own, t.p[i])), r.n = rot_apply(hw, rot_apply(own, t.n[i])), r.u = t.u[i];
                    r.ws.push_back({1.0f, headBone});
                }
                parts.push_back(std::move(part));
                continue;
            }
            if (skin >= 1 && io < 0) continue;
            if (skin >= 2 && wo < 0) continue;
            Part part;
            part.row = row, part.tris = t.tris, part.v.resize(a.count);
            const int rigidBone = src.u16(so + 0x10);
            for (uint32_t i = 0; i < a.count; i++) {
                const size_t o = a.data + (size_t)i * a.stride;
                Raw& r = part.v[i];
                r.p = t.p[i], r.n = t.n[i], r.u = t.u[i];
                int bone = -1;
                if (skin == 0) bone = rigidBone;
                else if (skin == 1) {
                    const auto it = sByRigid.find((int)src.u32(o + io));
                    bone = it != sByRigid.end() ? it->second : sb.front().id;
                }
                if (bone >= 0) {
                    const Mat w = bone_world(src, sfm, bone);
                    r.p = mat_apply(w, r.p), r.n = rot_apply(w, r.n);
                    r.ws.push_back({1.0f, bone});
                    continue;
                }
                for (int k = 0; k < std::min(skin, 4); k++) {
                    const float w = src.f32(o + wo + 4 * k);
                    if (w <= 0) continue;
                    const auto it = sBySmooth.find((int)src.u32(o + io + 4 * k));
                    if (it != sBySmooth.end()) r.ws.push_back({w, it->second});
                }
                if (r.ws.empty()) r.ws.push_back({1.0f, sb.front().id});
                std::sort(r.ws.begin(), r.ws.end(), [](auto& x, auto& y) { return x.first > y.first; });
            }
            parts.push_back(std::move(part));
        }
        // at its own proportions (a whole character only): nothing shrunk, Link's joints stretched by the runtime instead
        const bool real = real0 && spec->whole;
        if (real)
            for (auto& b : sb) sizeOf[b.id] = 1.0;
        // a whole character's head sized to Link's (his is big: about 52 across)
        if (spec->whole && !real) {
            double lo0 = 1e9, hi0 = -1e9;
            for (auto& part : parts)
                for (auto& r : part.v)
                    if (mapped[r.ws[0].second] == 15 && !dropped[r.ws[0].second]) lo0 = std::min(lo0, r.p[0]), hi0 = std::max(hi0, r.p[0]);
            if (hi0 > lo0) {
                const double sc = std::clamp(52.0 / (hi0 - lo0), 0.15, 1.5);
                for (auto& b : sb)
                    if (mapped[b.id] == 15) sizeOf[b.id] = sc;
            }
        }
        // eyes, brows and mouths: decals bigger than their picture (it ends inside them, see-through beyond): each row the whole
        // window of its corners, its corners in that window
        std::vector<std::array<double, 4>> window(nameCount, {1e9, 1e9, -1e9, -1e9});
        const bool decals = spec->textures[0] == nullptr;
        for (auto& part : parts)
            for (auto& r : part.v) {
                auto& w = window[part.row];
                w[0] = std::min(w[0], r.u[0]), w[1] = std::min(w[1], r.u[1]), w[2] = std::max(w[2], r.u[0]), w[3] = std::max(w[3], r.u[1]);
            }
        int waistBone = -1;
        for (auto& b : sb) if (b.name == "waist") waistBone = b.id;
        // (at its own proportions every corner measured from the one bone of its own that stands for that joint of Link's: a smaller
        // bone hanging off it, a toe, a heel, is not where the joint is, and the foot would end up that much off the ground)
        std::map<int, const BoneInfo*> canon;
        for (auto& b : sb)
            if (!dropped[b.id] && !canon.count(mapped[b.id]) && b.name.find("world_root") == std::string::npos && b.name != "center") canon[mapped[b.id]] = &b;
        std::vector<float> ratio(lb.size(), 1.0f), boneOffset;
        float raise = 0, armAngle = 0;
        std::map<int, Vec3> rest;  // (at its own proportions: each of Link's joints where it stands on the character at rest)
        if (real) {
            // each of Link's joints with a bone of the character on it: the stretch from the nearest such joint above it
            std::map<int, const BoneInfo*> srcOf;
            for (auto& b : sb)
                if (!dropped[b.id] && !srcOf.count(mapped[b.id]) && b.name.find("world_root") == std::string::npos && b.name != "center") srcOf[mapped[b.id]] = &b;
            for (auto& j : lb) {
                if (!srcOf.count(j.id)) continue;
                std::vector<int> path{j.id};
                int a = j.parent;
                for (int guard = 0; a >= 0 && !srcOf.count(a) && guard < 64; guard++) path.push_back(a), a = lById[a]->parent;
                if (a < 0) continue;
                const double dl = dist(j.at, lById[a]->at), ds = dist(srcOf[j.id]->at, srcOf[a]->at);
                if (dl < 1e-3 || ds < 1e-3) continue;
                for (int id : path)
                    if (id < (int)ratio.size()) ratio[id] = (float)std::clamp(ds / dl, 0.2, 6.0);
            }
            // (Link's own joints without a bone of the character: as the one above them)
            // the feet back on the ground: how far the longer legs reach below his
            for (auto& j : lb) {
                if (j.parent < 0) { rest[j.id] = j.at; continue; }
                const BoneInfo& p2 = *lById[j.parent];
                Vec3 at2 = rest[j.parent];
                for (int c = 0; c < 3; c++) at2[c] += ratio[j.id] * (j.at[c] - p2.at[c]);
                rest[j.id] = at2;
            }
            // (the sole, not the ankle: the foot bone as high above the lowest corner as on its owner; Link's soles are on the ground)
            // (the lowest corner on its feet or lower legs: a tail or a hem below the ground does not count)
            double lowest = 1e9;
            for (auto& part : parts)
                for (auto& r : part.v) {
                    const int on = mapped[r.ws[0].second];
                    if (on == 34 || on == 39 || on == 33 || on == 38) lowest = std::min(lowest, r.p[1]);
                }
            if (lowest > 1e8) lowest = 0;
            // (no foot bone, the small ones: its shin's end is on the ground, Link's foot joint as far below that as on him)
            const double ankle = srcOf.count(34) ? srcOf[34]->at[1] - lowest
                                 : srcOf.count(33) ? srcOf[33]->at[1] - lowest - ratio[34] * (lById[33]->at[1] - lById[34]->at[1])
                                                   : lById[34]->at[1];
            raise = (float)(ankle - lById[34]->at[1]);  // (how much higher its ankle is than Link's; the runtime keeps the feet down each frame)
            // the runtime, for the drawing only, moves each skinned joint by this in the joint's own axes: the mesh then bends about the
            // character's joints, not Link's, exactly as on its owner
            // the arms swung out by what the character's own torso needs (Link's pose keeps them close to his narrow body)
            if (srcOf.count(6) && srcOf.count(7) && srcOf.count(30)) {
                const double armX = std::fabs(srcOf[6]->at[0]), upper = std::max(dist(srcOf[6]->at, srcOf[7]->at), 1.0), top = srcOf[6]->at[1], bottom = srcOf[30]->at[1];
                double torso = 0, thick = 0;
                for (auto& part : parts)
                    for (auto& r : part.v) {
                        const int on = mapped[r.ws[0].second];
                        if ((on == 3 || on == 4 || on == 30) && r.p[1] > bottom && r.p[1] < top) torso = std::max(torso, std::fabs(r.p[0]));
                        if (on == 6) thick = std::max(thick, std::fabs(r.p[1] - top));
                    }
                armAngle = (float)std::clamp(0.65 * std::atan(std::max(0.0, torso + thick - armX) / upper) * 180.0 / 3.14159265 - 8.0, 0.0, 20.0);  // (Link's pose already holds his arms a little out; beside the real Orca the full amount was too much)
            }
            boneOffset.resize(lb.size() * 3);
            for (auto& j : lb) {
                const Mat w = bone_world(f, cl, j.id);
                const double d[3] = {j.at[0] - rest[j.id][0], j.at[1] - rest[j.id][1], j.at[2] - rest[j.id][2]};
                for (int c = 0; c < 3; c++) boneOffset[(size_t)j.id * 3 + c] = (float)(w[0][c] * d[0] + w[1][c] * d[1] + w[2][c] * d[2]);
            }
        }
        // (a bone that counts as the head but is far from it, at the model's origin: its corners either stand where the head is, sv_head, or lie
        // round that origin, a head built apart: those stay measured from their own bone)
        std::set<int> ownOrigin;
        {
            std::map<int, std::array<double, 4>> mean;
            for (auto& part : parts)
                for (auto& r : part.v) {
                    auto& m = mean[r.ws[0].second];
                    for (int j = 0; j < 3; j++) m[j] += r.p[j];
                    m[3] += 1;
                }
            for (auto& [id, m] : mean) {
                if (mapped[id] != 15 || headBone < 0 || id == headBone || !m[3] || dist(sById[id]->at, sById[headBone]->at) <= 20) continue;
                const Vec3 c{m[0] / m[3], m[1] / m[3], m[2] / m[3]};
                if (dist(c, sById[id]->at) < dist(c, sById[headBone]->at)) ownOrigin.insert(id);
            }
        }
        if (spec->whole && !real) boneOffset.assign(lb.size() * 3, 0.0f);
        // the fists the game shows while something is held: as big as the character's hands
        float handScale = 1;
        if (spec->whole && canon.count(8)) {
            double far = 0;
            for (auto& part : parts)
                for (auto& r : part.v)
                    if (mapped[r.ws[0].second] == 8) far = std::max(far, dist(r.p, canon[8]->at) * sizeOf[canon[8]->id]);
            if (far > 0) handScale = (float)std::clamp(far / 12.5, 0.5, 3.0);
        }
        for (auto& part : parts) {
            const uint32_t base = (uint32_t)vs.size();
            std::vector<char> gone(part.v.size(), 0), onHand(part.v.size(), 0);
            for (size_t i = 0; i < part.v.size(); i++) {
                const Raw& r = part.v[i];
                if (dropped[r.ws[0].second]) gone[i] = 1;
                if (mapped[r.ws[0].second] == 8 || mapped[r.ws[0].second] == 12) onHand[i] = 1;
                double sum = 0;
                for (size_t k = 0; k < r.ws.size() && k < 3; k++) sum += r.ws[k].first;
                V v{};
                v.p = {0, 0, 0};
                for (size_t k = 0; k < r.ws.size() && k < 3; k++) {
                    const double w = r.ws[k].first / sum;
                    const BoneInfo* fromP = sById[r.ws[k].second];
                    const bool own = ownOrigin.count(fromP->id) != 0;
                    if (!own && headBone >= 0 && fromP->id != headBone && mapped[fromP->id] == 15 && dist(fromP->at, sById[headBone]->at) > 20) fromP = sById[headBone];
                    int to0 = mapped[r.ws[k].second];
                    // (at its own proportions a hand goes with its forearm: Link's wrist has no matrix of its own)
                    if (spec->whole && !own)
                        if (const auto c = canon.find(to0); c != canon.end()) fromP = c->second;
                    // (many models tie the whole forearm to the hand bone: only what lies beyond the wrist counts as the hand, the rest goes with the forearm)
                    if (spec->whole && (to0 == 8 || to0 == 12)) {
                        const int fore = to0 == 8 ? 7 : 11;
                        const auto cw = canon.find(to0), cf = canon.find(fore);
                        if (cw != canon.end() && cf != canon.end()) {
                            const Vec3 a = cw->second->at, b = cf->second->at;
                            const double d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
                            const double along = (r.p[0] - a[0]) * d[0] + (r.p[1] - a[1]) * d[1] + (r.p[2] - a[2]) * d[2];
                            if (along < 0) to0 = fore, fromP = cf->second;
                        }
                    }
                    const BoneInfo& from = *fromP;
                    const int to = to0;
                    const BoneInfo& at = *lById[to];
                    const double sc = sizeOf[from.id];
                    // (a hand on a joint of its own that nothing else uses, two of the hair's: the runtime puts them on his wrists, and shrinks them
                    // away while the game shows his fist there)
                    const bool isHand = spec->whole && (to == 8 || to == 12);
                    const BoneInfo& spare = *lById[to == 8 ? 22 : 24];
                    const Vec3 place = isHand ? spare.at : real ? rest[to] : at.at;
                    for (int j = 0; j < 3; j++) v.p[j] += w * (place[j] + sc * (r.p[j] - from.at[j]));
                    // (Link's wrists have no skin matrix: on the forearm's)
                    const BoneInfo* sk = isHand ? &spare : &at;
                    for (int guard = 0; sk->smooth < 0 && sk->parent >= 0 && guard < 64; guard++) sk = lById[sk->parent];
                    v.idx[k] = (uint32_t)std::max(sk->smooth, 0), v.w[k] = (float)w;
                    usedBones.insert(sk->id);
                }
                // (a robe or skirt below the waist hangs from the body's bones, which are sized like the torso: its hem as high above
                // the ground, in proportion to the waist's height, as on its owner, so it still meets the legs)
                if (spec->whole && !real && waistBone >= 0 && r.p[1] < sById[waistBone]->at[1]) {
                    const int on = mapped[r.ws[0].second];
                    if (on == 30 || on == 3 || on == 4) v.p[1] = lById[30]->at[1] * std::max(r.p[1], 0.0) / std::max(sById[waistBone]->at[1], 1.0);
                }
                // (turned away from straight up, as the helmets': less of the sky's blue on the shaded side)
                v.n = r.n;
                v.n[1] *= 0.2;
                {
                    const double l = std::sqrt(v.n[0] * v.n[0] + v.n[1] * v.n[1] + v.n[2] * v.n[2]);
                    if (l > 1e-6) v.n = {v.n[0] / l, v.n[1] / l, v.n[2] / l};
                }
                const double uu = std::fmod(std::fmod(r.u[0], 1.0) + 1.0, 1.0), vv = std::min(1.0, std::max(0.0, r.u[1]));
                v.u = {(kLinkW + uu * (kAtlasW - kLinkW)) / kAtlasW, (rowY[part.row] + vv * rowHs[part.row]) / (double)kAtlasH};
                if (decals && part.row > 0) {
                    const auto& w = window[part.row];
                    const double a = (r.u[0] - w[0]) / std::max(w[2] - w[0], 1e-6), b = (r.u[1] - w[1]) / std::max(w[3] - w[1], 1e-6);
                    v.u = {(kLinkW + a * (kAtlasW - kLinkW)) / kAtlasW, (rowY[part.row] + b * rowHs[part.row]) / (double)kAtlasH};
                }
                vs.push_back(v);
            }
            for (auto& tr : part.tris)
                if (tr[0] < gone.size() && tr[1] < gone.size() && tr[2] < gone.size() && !gone[tr[0]] && !gone[tr[1]] && !gone[tr[2]] && (spec->whole || !(onHand[tr[0]] && onHand[tr[1]] && onHand[tr[2]])))
                    tris.push_back({tr[0] + base, tr[1] + base, tr[2] + base});
        }
        if (vs.empty() || tris.empty()) return error = "the outfit has no triangles", false;
        if (spec->twoSided) {
            const uint32_t n = (uint32_t)vs.size();
            const size_t nt = tris.size();
            for (uint32_t i = 0; i < n; i++) {
                V v = vs[i];
                v.n = {-v.n[0], -v.n[1], -v.n[2]};
                vs.push_back(v);
            }
            for (size_t i = 0; i < nt; i++) tris.push_back({tris[i][2] + n, tris[i][1] + n, tris[i][0] + n});
        }
        // the picture: Link's on the left, squeezed; the outfit's textures one under the other on the right
        Image atlas, own;
        atlas.w = kAtlasW, atlas.h = kAtlasH, atlas.px.assign((size_t)kAtlasW * kAtlasH * 4, 255);
        if (!texture_image(f, "linktexS3TC", own)) return error = "Link's picture not found", false;
        auto blit = [&](const Image& im, int x0, int y0) {
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) memcpy(&atlas.px[((size_t)(y + y0) * kAtlasW + x + x0) * 4], &im.px[((size_t)y * im.w + x) * 4], 4);
        };
        blit(resize_lanczos(own, kLinkW, kAtlasH), 0, 0);
        Image mainIm;
        // a row of eyes, brows or a mouth: `tex` over the skin (the row's own picture, or one of its other frames)
        auto bakeRow = [&](int k, const std::string& tex) -> Image {
                // a decal laid over the skin of the head where it sits: the main picture's colour at the nearest corner of the main mesh
                Vec3 mid{0, 0, 0};
                size_t n = 0;
                for (auto& part : parts)
                    if (part.row == k) for (auto& r : part.v) { for (int j = 0; j < 3; j++) mid[j] += r.p[j]; n++; }
                for (int j = 0; j < 3 && n; j++) mid[j] /= n;
                uint8_t skin[3] = {200, 160, 120};
                double best = 1e30;
                for (auto& part : parts)
                    if (part.row == 0)
                        for (auto& r : part.v) {
                            const double d = dist(r.p, mid);
                            if (d >= best || mainIm.w == 0) continue;
                            best = d;
                            const int x = std::clamp((int)(std::fmod(std::fmod(r.u[0], 1.0) + 1.0, 1.0) * mainIm.w), 0, mainIm.w - 1), y = std::clamp((int)(r.u[1] * mainIm.h), 0, mainIm.h - 1);
                            memcpy(skin, &mainIm.px[((size_t)y * mainIm.w + x) * 4], 3);
                        }
                Image raw;
                if (!texture_image(src, tex, raw)) return Image{};
                uint32_t fmt = 0;
                for (auto& [n2, o] : src.group(src.section(1)))
                    if (n2 == tex) fmt = src.u32(o + 0x18) & 0x3F;
                // (eyes in two layers: the eye, and the pupil as the material's second picture, the same corners shifted a little)
                Image pupil;
                bool hasPupil = false;
                float shift[2] = {0, 0};
                for (auto& it : items) {
                    if (shapeTexture(it) != texNames[k]) continue;
                    const auto ms2 = src.group(src.rel(it.fm + 24));
                    const size_t mo = ms2[src.u16(it.so + 0xE)].second;
                    if (src.u8(mo + 0x11) < 2) continue;
                    const std::string second = src.str(src.rel(src.rel(mo + 0x28) + 8));
                    if (second.find("hitomi") == std::string::npos || !texture_image(src, second, pupil)) continue;
                    hasPupil = true;
                    const size_t pl = src.rel(mo + 0x34), pd = src.rel(mo + 0x3C);
                    for (int q = 0; q < src.u16(mo + 0x12); q++) {
                        const size_t e = pl + q * 0x14;
                        if (src.str(src.rel(e + 0x10)) == "texmtx1") shift[0] = (float)src.f32(pd + src.u16(e + 2) + 12), shift[1] = (float)src.f32(pd + src.u16(e + 2) + 16);
                    }
                    break;
                }
                Image row;
                row.w = kAtlasW - kLinkW, row.h = rowHs[k], row.px.resize((size_t)row.w * row.h * 4);
                const auto& w = window[k];
                for (int y = 0; y < row.h; y++)
                    for (int x = 0; x < row.w; x++) {
                        const double u = w[0] + (x + 0.5) / row.w * (w[2] - w[0]), v = w[1] + (y + 0.5) / row.h * (w[3] - w[1]);
                        uint8_t* q = &row.px[((size_t)y * row.w + x) * 4];
                        double a = 0;
                        uint8_t c[3] = {0, 0, 0};
                        if (u >= 0 && u < 1 && v >= 0 && v < 1) {
                            const uint8_t* t = &raw.px[((size_t)(v * raw.h) * raw.w + (size_t)(u * raw.w)) * 4];
                            if (fmt == 0x35) c[0] = c[1] = c[2] = t[0], a = t[1] / 255.0;
                            else if (fmt == 0x34) c[0] = c[1] = c[2] = t[0], a = 1;
                            else c[0] = t[0], c[1] = t[1], c[2] = t[2], a = t[3] / 255.0;
                            if (hasPupil) {
                                const double pu = u + shift[0], pv = v + shift[1];
                                if (pu >= 0 && pu < 1 && pv >= 0 && pv < 1) {
                                    const uint8_t* e = &pupil.px[((size_t)(pv * pupil.h) * pupil.w + (size_t)(pu * pupil.w)) * 4];
                                    const double ap = e[3] / 255.0;
                                    for (int j = 0; j < 3; j++) c[j] = (uint8_t)(c[j] * (1 - ap) + e[j] * (c[j] / 255.0) * ap);
                                }
                            }
                        }
                        for (int j = 0; j < 3; j++) q[j] = (uint8_t)(skin[j] * (1 - a) + c[j] * a);
                        q[3] = 255;
                    }
                return row;
        };
        for (int k = 0; k < nameCount; k++) {
            SwordDef pic{};
            pic.texture = texNames[k].c_str();
            for (int c = 0; c < 3; c++) pic.tint[c] = texTint[k][c];
            Image im;
            bool grey = false;
            if (decals && k > 0) {
                blit(bakeRow(k, texNames[k]), kLinkW, rowY[k]);
                continue;
            }
            if (!equip_picture(src, pic, im, grey, error)) return false;
            if (k == 0) mainIm = im;
            // (a picture that is pattern on black: its black the outfit's own colour; black would only show the sky's light)
            if (spec->base[0] | spec->base[1] | spec->base[2])
                for (size_t i = 0; i < (size_t)im.w * im.h; i++) {
                    uint8_t* q = &im.px[i * 4];
                    const float a2 = std::min(1.0f, std::max({q[0], q[1], q[2]}) / 96.0f);
                    for (int c = 0; c < 3; c++) q[c] = (uint8_t)(spec->base[c] * (1 - a2) + q[c] * a2);
                }
            blit(resize_lanczos(im, kAtlasW - kLinkW, rowHs[k]), kLinkW, rowY[k]);
        }
        // his fists in the character's skin colour (the main picture under its hands)
        if (spec->whole && mainIm.w) {
            double sum[3] = {0, 0, 0};
            size_t count = 0;
            for (auto& part : parts) {
                if (part.row != 0) continue;
                for (auto& r : part.v) {
                    const int on = mapped[r.ws[0].second];
                    if (on != 8 && on != 12) continue;
                    const int x = std::clamp((int)(std::fmod(std::fmod(r.u[0], 1.0) + 1.0, 1.0) * mainIm.w), 0, mainIm.w - 1), y = std::clamp((int)(r.u[1] * mainIm.h), 0, mainIm.h - 1);
                    for (int c = 0; c < 3; c++) sum[c] += mainIm.px[((size_t)y * mainIm.w + x) * 4 + c];
                    count++;
                }
            }
            Image fist;
            if (count && texture_image(f, "handsS3TC", fist)) {
                double mean = 0;
                for (size_t i = 0; i < (size_t)fist.w * fist.h; i++) mean += (fist.px[i * 4] * 3 + fist.px[i * 4 + 1] * 6 + fist.px[i * 4 + 2]) / 10.0;
                mean /= std::max<size_t>((size_t)fist.w * fist.h, 1);
                for (size_t i = 0; i < (size_t)fist.w * fist.h; i++) {
                    uint8_t* q = &fist.px[i * 4];
                    const double l = (q[0] * 3 + q[1] * 6 + q[2]) / 10.0 / std::max(mean, 1.0);
                    for (int c = 0; c < 3; c++) q[c] = (uint8_t)std::clamp(sum[c] / count * l, 0.0, 255.0);
                }
                put_texture(f, "handsS3TC", fist, false);
            }
        }
        // blinking: the eyes' next frames (name.2, name.3), the whole picture for each
        std::vector<Image> blinks;
        if (decals) {
            auto frameOf = [&](const std::string& n, int f) -> std::string {
                if (n.size() < 3 || n.compare(n.size() - 2, 2, ".1") != 0 || n.find("eye") == std::string::npos) return "";
                const std::string name2 = n.substr(0, n.size() - 1) + std::to_string(f);
                for (auto& [tn2, o] : src.group(src.section(1))) { (void)o; if (tn2 == name2) return name2; }
                return "";
            };
            for (int fr = 2; fr <= 3; fr++) {
                Image v = atlas;
                bool any = false;
                const Image keep = atlas;
                for (int k = 1; k < nameCount; k++) {
                    const std::string other = frameOf(texNames[k], fr);
                    if (other.empty()) continue;
                    const Image row = bakeRow(k, other);
                    if (row.w) blit(row, kLinkW, rowY[k]), any = true;
                }
                if (any) blinks.push_back(atlas);
                atlas = keep;
                (void)v;
            }
        }
        // every shape that shows Link's picture squeezed to its left part
        for (auto& [mn, fm] : f.group(f.section(0))) {
            (void)mn;
            const auto ms = f.group(f.rel(fm + 24));
            const auto ss = f.group(f.rel(fm + 20));
            for (int si = 0; si < (int)ss.size(); si++) {
                const size_t so = ss[si].second, mo = ms[f.u16(so + 0xE)].second;
                if (!f.u8(mo + 0x11) || f.str(f.rel(f.rel(mo + 0x28))) != "linktexS3TC") continue;
                const Attrs a = attrs(f, fm, so);
                const int uo = a.off("_u0");
                Bytes vb(f.d.begin() + a.data, f.d.begin() + a.data + (size_t)a.count * a.stride);
                for (uint32_t i = 0; i < a.count; i++) putf32(&vb[(size_t)i * a.stride + uo], (float)(f.f32(a.data + (size_t)i * a.stride + uo) * kLinkW / kAtlasW));
                f.set_rel(a.bl + 0x14, f.append(vb.data(), vb.size(), 0x100));
            }
        }
        // the mesh into "sleeve" (arms and the tunic's bottom), the same layout; a replacing outfit: instead of his body
        const auto cs = f.group(f.rel(cl + 20));
        int sleeve = -1;
        for (int si = 0; si < (int)cs.size(); si++) if (cs[si].first == "link_root__sleeve") sleeve = si;
        if (sleeve < 0) return error = "Link's body shape not found", false;
        const size_t so = cs[sleeve].second;
        const Attrs a = attrs(f, cl, so);
        const int po = a.off("_p0"), no = a.off("_n0"), uo = a.off("_u0"), io = a.off("_i0"), wo = a.off("_w0");
        SwShape old;
        if (!read_shape(f, cl, sleeve, old, error, true)) return false;
        Bytes vbuf;
        if (!spec->replace) vbuf.assign(f.d.begin() + a.data, f.d.begin() + a.data + (size_t)a.count * a.stride);
        const uint32_t vbase = spec->replace ? 0 : a.count;
        vbuf.resize((size_t)(vbase + vs.size()) * a.stride, 0);
        const Bytes ref(f.d.begin() + a.data, f.d.begin() + a.data + a.stride);
        for (size_t i = 0; i < vs.size(); i++) {
            uint8_t* v = &vbuf[((size_t)vbase + i) * a.stride];
            memcpy(v, ref.data(), a.stride);
            for (int k = 0; k < 3; k++) putf32(v + po + 4 * k, (float)vs[i].p[k]), putf32(v + no + 4 * k, (float)vs[i].n[k]);
            for (int k = 0; k < 2; k++) putf32(v + uo + 4 * k, (float)vs[i].u[k]);
            for (int k = 0; k < 3; k++) put32(v + io + 4 * k, vs[i].idx[k]), putf32(v + wo + 4 * k, vs[i].w[k]);
        }
        f.set_rel(a.bl + 0x14, f.append(vbuf.data(), vbuf.size(), 0x100));
        put32(&f.d[a.vb + 8], (uint32_t)(vbase + vs.size()));
        put32(&f.d[a.bl + 4], (uint32_t)vbuf.size());
        std::vector<uint32_t> flat;
        if (!spec->replace)
            for (auto& t : old.tris) flat.insert(flat.end(), t.begin(), t.end());
        for (auto& t : tris) for (uint32_t x : t) flat.push_back(x + vbase);
        set_indices(f, cl, sleeve, flat);
        // the bones its corners use (the list the shape keeps)
        std::set<int> list(usedBones);
        if (!spec->replace) {
            const size_t l0 = f.rel(so + 0x28);
            for (int k = 0; k < f.u16(so + 0x14); k++) list.insert(f.u16(l0 + 2 * k));
        }
        Bytes lbuf(list.size() * 2);
        { size_t i = 0; for (int b : list) put16(&lbuf[2 * i++], (uint16_t)b); }
        f.set_rel(so + 0x28, f.append(lbuf.data(), lbuf.size(), 4));
        put16(&f.d[so + 0x14], (uint16_t)list.size());
        {
            Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
            for (auto& v : vs) for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], v.p[k]), hi[k] = std::max(hi[k], v.p[k]);
            for (auto& p : old.p) for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
            f.putf(so + 0x1C, (float)(dist(lo, hi) / 2 + 20));
        }
        if (spec->replace) {
            for (int si = 0; si < (int)cs.size(); si++) {
                const std::string& n = cs[si].first;
                // (a whole character: all of his head goes too)
                const bool head = n == "link_root__ear_2_" || n == "link_root__ear_3_" || n == "link_root__face" || n == "link_root__mouth" || n == "cl_hana__ear_7_" || n == "cl_LhandA__ear_5_" || n == "cl_RhandA__ear_6_" ||
                                  n.rfind("cl_eye__", 0) == 0 || n.rfind("cl_mayu__", 0) == 0;
                if (n == "link_root__ear_4_" || n == "cl_back__ear_8_" || (spec->whole && (head || n == "link_root__ear"))) set_indices(f, cl, si, {});
                else if (n == "link_root__ear") {
                    // (only what is on his head: the ears)
                    SwShape t;
                    if (!read_shape(f, cl, si, t, error, true)) return false;
                    const Attrs ea = attrs(f, cl, cs[si].second);
                    const int ei = ea.off("_i0"), ew = ea.off("_w0");
                    auto head = [&](uint32_t i) {
                        const size_t o = ea.data + (size_t)i * ea.stride;
                        const int b = f.f32(o + ew) >= f.f32(o + ew + 4) ? (int)f.u32(o + ei) : (int)f.u32(o + ei + 4);
                        return b >= 10 && b <= 20;
                    };
                    std::vector<uint32_t> keep;
                    for (auto& tr : t.tris)
                        if (head(tr[0]) && head(tr[1]) && head(tr[2])) keep.insert(keep.end(), tr.begin(), tr.end());
                    set_indices(f, cl, si, keep);
                }
            }
        }
        if (spec->whole)
            for (auto& [mn, fm] : f.group(f.section(0)))
                if (mn == "katsura") set_indices(f, fm, 0, {});
        if (extra) {
            Bfres g{f.d};
            if (!put_texture(g, "mouthS3TC.1", atlas, false)) return error = "texture slot", false;
            uint32_t tpl = 0;
            extra->links.clear();
            for (auto& [n, o] : g.group(g.section(1)))
                if (n == "mouthS3TC.1") tpl = (uint32_t)o, extra->image = {(uint32_t)g.rel(o + 0xB0), g.u32(o + 0x24), (uint32_t)g.rel(o + 0xB4), g.u32(o + 0x2C)};
            for (auto& [n, o] : g.group(g.section(1)))
                if (n == "linktexS3TC" || n == "linktexbci4") extra->links.push_back({(uint32_t)o, tpl});
            extra->surface = true;
            extra->tiers.clear();
            extra->blink.clear();
            for (const Image& b : blinks) {
                Bfres h{f.d};
                if (!put_texture(h, "mouthS3TC.1", b, false)) break;
                Bytes d;
                uint32_t off = 0, sz = 0, mo2 = 0, msz = 0;
                for (auto& [n, o] : h.group(h.section(1)))
                    if (n == "mouthS3TC.1") off = (uint32_t)h.rel(o + 0xB0), sz = h.u32(o + 0x24), mo2 = (uint32_t)h.rel(o + 0xB4), msz = h.u32(o + 0x2C);
                d.assign(h.d.begin() + off, h.d.begin() + off + sz);
                if (msz) d.insert(d.end(), h.d.begin() + mo2, h.d.begin() + mo2 + msz);
                extra->blink.push_back(std::move(d));
            }
            if (spec->whole) extra->boneRatio = ratio, extra->raise = raise, extra->boneOffset = boneOffset, extra->armAngle = armAngle, extra->handScale = handScale;
            if (spec->whole) {
                // (his wrists' axes turned into those of the two joints the hands hang on)
                extra->handFrame.resize(18);
                for (int side = 0; side < 2; side++) {
                    const Mat a = bone_world(f, cl, side ? 12 : 8), b = bone_world(f, cl, side ? 24 : 22);
                    for (int r = 0; r < 3; r++)
                        for (int c = 0; c < 3; c++) extra->handFrame[side * 9 + r * 3 + c] = (float)(a[0][r] * b[0][c] + a[1][r] * b[1][c] + a[2][r] * b[2][c]);
                }
            }  // (at Link's size all 1: for the folding away when looking through his eyes)
            extra->preview = atlas.px, extra->previewW = atlas.w, extra->previewH = atlas.h;
            extra->own.assign(g.d.begin() + extra->image[0], g.d.begin() + extra->image[0] + extra->image[1]);
            if (extra->image[3]) extra->own.insert(extra->own.end(), g.d.begin() + extra->image[2], g.d.begin() + extra->image[2] + extra->image[3]);
        }
        if (report) {
            char b[160];
            snprintf(b, sizeof b, "%s: %zu vertices, %zu triangles, defence %d %%", def.id, vs.size(), tris.size(), def.defence);
            *report = b;
        }
        out = std::move(f.d);
        return true;
    } catch (const std::exception& e) {
        return error = e.what(), false;
    }
}
}  // namespace gcmodel
