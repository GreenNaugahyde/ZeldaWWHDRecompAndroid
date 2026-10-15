// Warp catalog for the developer Warp menu (mods/warp.cpp, OptionsMenu.java), made on the device from
// the player's game files at the end of the extraction (or when the menu first needs it):
// <game>/warps.json, places by category, each with the spawn points that exist in it.
// The game walks off the end of its spawn list for a spawn number that isn't in the room's list
// (dStage_playerInit), so the menu only offers spawns read from the game files.
//
// WWHD keeps the stage data (stage.dzs, room.dzr of the GameCube game) inside each
// content/Common/Stage/<name>_Stage.szs / _Room<n>.szs bfres; the sea rooms of the start are in
// Pack/*.pack (a SARC of szs files). The DZx chunk tables are found by their layout: u32 count,
// count x (fourcc, u32 count, u32 offset), the first chunk's data right behind the table. PLYR
// entries (0x20 bytes): params at +8 (room in bits 0-5, Link's start mode in bits 12-15), the spawn
// number in the low byte of angle.z (+0x1C). SCLS entries (0xC bytes): stage[8], spawn, room.
//
// Place names follow the Wind Waker Randomizer's tables (github.com/LagoLunatic/wwrando, MIT:
// data/stage_names.txt, island_names.txt, randomizers/entrances.py). Unpacking the ~2.3 GB of
// stage archives is most of the time (several threads).
#include "warp_catalog.h"

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <thread>
#include <tuple>

#include "game_archive.h"

namespace warp_catalog {
namespace {
using game_archive::be16;
using game_archive::be32;
using game_archive::Bytes;

const std::map<std::string, std::string> kStages = {
    {"Abesso", "Cabana"}, {"Abship", "Submarines"}, {"Adanmae", "Dragon Roost Cavern entrance"},
    {"ADMumi", "Tower of the Gods rising (cutscene)"}, {"Asoko", "Pirate ship interior"}, {"Atorizk", "Rito Aerie"},
    {"A_mori", "Outset Island: Forest of Fairies"}, {"A_nami", "Unused (A_nami)"},
    {"A_umikz", "Sea: pirate ship ride to Forsaken Fortress"}, {"Cave01", "Bomb Island cave"}, {"Cave02", "Star Island cave"},
    {"Cave03", "Cliff Plateau Isles cave"}, {"Cave04", "Rock Spire Isle cave"}, {"Cave05", "Horseshoe Island cave"},
    {"Cave06", "Unused cave (Cave06)"}, {"Cave07", "Pawprint Isle Wizzrobe cave"}, {"Cave08", "Unused cave (Cave08)"},
    {"Cave09", "Savage Labyrinth (floors 1-30)"}, {"Cave10", "Savage Labyrinth (floors 31-40)"},
    {"Cave11", "Savage Labyrinth (floors 41-50)"}, {"Comori", "Komali's room"}, {"DmSpot0", "Unused (DmSpot0)"},
    {"E3ROOP", "Unused (E3ROOP)"}, {"Ebesso", "Unused (Ebesso)"}, {"Edaichi", "Headstone Island interior"},
    {"Ekaze", "Gale Isle interior"}, {"ENDumi", "Ending (sea)"}, {"Fairy01", "Northern Fairy Fountain"},
    {"Fairy02", "Eastern Fairy Fountain"}, {"Fairy03", "Western Fairy Fountain"}, {"Fairy04", "Outset Fairy Fountain"},
    {"Fairy05", "Thorned Fairy Fountain"}, {"Fairy06", "Southern Fairy Fountain"},
    {"figureA", "Nintendo Gallery hall A"}, {"figureB", "Nintendo Gallery hall B"}, {"figureC", "Nintendo Gallery hall C"},
    {"figureD", "Nintendo Gallery hall D"}, {"figureE", "Nintendo Gallery hall E"}, {"figureF", "Nintendo Gallery hall F"},
    {"figureG", "Nintendo Gallery hall G"}, {"GanonA", "Ganon's Tower entrance"},
    {"GanonB", "Ganon's Tower: before Gohma rematch"}, {"GanonC", "Ganon's Tower: before Molgera rematch"},
    {"GanonD", "Ganon's Tower: before Kalle Demos rematch"}, {"GanonE", "Ganon's Tower: before Jalhalla rematch"},
    {"GanonJ", "Ganon's Tower: Phantom Ganon maze"}, {"GanonK", "Ganon's Tower: Puppet Ganon"},
    {"GanonL", "Ganon's Tower: stairs to Puppet Ganon"}, {"GanonM", "Ganon's Tower: Phantom Ganon"},
    {"GanonN", "Ganon's Tower: stairs to Phantom Ganon"}, {"GTower", "Ganon's Tower: rooftop (Ganondorf)"},
    {"Hyroom", "Hyrule Castle interior"}, {"Hyrule", "Hyrule Castle"}, {"ITest61", "Unused (ITest61)"},
    {"ITest62", "Ice Ring Isle inner cave"}, {"ITest63", "Shark Island cave"}, {"I_SubAN", "Unused (I_SubAN)"},
    {"Kaisen", "Sploosh Kaboom"}, {"kazan", "Unused (kazan)"}, {"kaze", "Wind Temple"}, {"kazeB", "Molgera"},
    {"kazeMB", "Wind Temple: Wizzrobe miniboss"}, {"kenroom", "Master Sword chamber"}, {"kinBOSS", "Kalle Demos"},
    {"kindan", "Forbidden Woods"}, {"kinMB", "Forbidden Woods: Mothula miniboss"}, {"LinkRM", "Link's house"},
    {"LinkUG", "Under Link's house"}, {"M2ganon", "Forsaken Fortress: Ganondorf's room"},
    {"M2tower", "Forsaken Fortress tower (2nd visit)"}, {"ma2room", "Forsaken Fortress interior (2nd visit)"},
    {"ma3room", "Forsaken Fortress interior (3rd visit)"}, {"majroom", "Forsaken Fortress interior (1st visit)"},
    {"MajyuE", "Forsaken Fortress outside (1st visit)"}, {"MiniHyo", "Ice Ring Isle cave"}, {"MiniKaz", "Fire Mountain cave"},
    {"Mjtower", "Forsaken Fortress tower (1st visit; only at that point of the story, else black screen or crash)"}, {"Msmoke", "Unused (Msmoke)"}, {"Mukao", "Unused (Mukao)"},
    {"M_Dai", "Earth Temple"}, {"M_DaiB", "Jalhalla"}, {"M_DaiMB", "Earth Temple: Stalfos miniboss"},
    {"M_Dra09", "Dragon Roost Cavern: Moblin room"}, {"M_DragB", "Gohma"}, {"M_NewD2", "Dragon Roost Cavern"},
    {"Nitiyou", "Mrs. Marie's school"}, {"Obombh", "Windfall bomb shop"}, {"Obshop", "Beedle's shop ship"},
    {"Ocean", "Boating Course minigame"}, {"Ocmera", "Lenzo's house"}, {"Ocrogh", "Hollo's potion shop"},
    {"Ojhous", "Orca's house"}, {"Ojhous2", "Sturgeon's room"}, {"Omasao", "Mesa's house"}, {"Omori", "Forest Haven"},
    {"Onobuta", "Abe and Rose's house"}, {"Opub", "Windfall café bar"}, {"Orichh", "House of Wealth (auction)"},
    {"Otkura", "Forest Haven waterfall cave"}, {"Pdrgsh", "Chu Jelly juice shop"}, {"Pfigure", "Nintendo Gallery"},
    {"Pjavdou", "Jabun's cave"}, {"Pnezumi", "Windfall jail"}, {"PShip", "Ghost Ship"}, {"PShip2", "Unused (PShip2)"},
    {"PShip3", "Unused (PShip3)"}, {"sea", "The Great Sea"}, {"sea_E", "Unused sea (sea_E)"}, {"sea_T", "Title screen"},
    {"ShipD", "Islet of Steel interior"}, {"Siren", "Tower of the Gods"}, {"SirenB", "Gohdan"},
    {"SirenMB", "Tower of the Gods: Darknut miniboss"}, {"SubD42", "Needle Rock Isle cave"}, {"SubD43", "Angular Isles cave"},
    {"SubD44", "Unused cave (SubD44)"}, {"SubD51", "Unused cave (SubD51)"}, {"SubD71", "Boating Course cave"},
    {"TF_01", "Stone Watcher Island cave"}, {"TF_02", "Overlook Island cave"}, {"TF_03", "Bird's Peak Rock cave"},
    {"TF_04", "Cabana Labyrinth"}, {"TF_06", "Dragon Roost Island cave"}, {"tincle", "Tingle's room"},
    {"TyuTyu", "Pawprint Isle Chuchu cave"}, {"WarpD", "Diamond Steppe warp maze"}, {"Xboss0", "Gohma rematch"},
    {"Xboss1", "Kalle Demos rematch"}, {"Xboss2", "Jalhalla rematch"}, {"Xboss3", "Molgera rematch"},
    {"Name", "File select"},
};

const char* const kSea[] = {
    "Sea floor", "Forsaken Fortress", "Star Island", "Northern Fairy Island", "Gale Isle", "Crescent Moon Island",
    "Seven-Star Isles", "Overlook Island", "Four-Eye Reef", "Mother and Child Isles", "Spectacle Island",
    "Windfall Island", "Pawprint Isle", "Dragon Roost Island", "Flight Control Platform", "Western Fairy Island",
    "Rock Spire Isle", "Tingle Island", "Northern Triangle Island", "Eastern Fairy Island", "Fire Mountain",
    "Star Belt Archipelago", "Three-Eye Reef", "Greatfish Isle", "Cyclops Reef", "Six-Eye Reef", "Tower of the Gods",
    "Eastern Triangle Island", "Thorned Fairy Island", "Needle Rock Isle", "Islet of Steel", "Stone Watcher Island",
    "Southern Triangle Island", "Private Oasis", "Bomb Island", "Bird's Peak Rock", "Diamond Steppe Island",
    "Five-Eye Reef", "Shark Island", "Southern Fairy Island", "Ice Ring Isle", "Forest Haven", "Cliff Plateau Isles",
    "Horseshoe Island", "Outset Island", "Headstone Island", "Two-Eye Reef", "Angular Isles", "Boating Course",
    "Five-Star Isles",
};

// Link's start mode (PLYR params bits 12-15; daPy_lk_c::playerInit)
const char* mode_name(int m) {
    switch (m) {
    case 1: case 5: return "walks in";
    case 2: return "on the boat";
    case 4: return "Forsaken Fortress jail";
    case 7: return "out of a warp pot";
    case 9: return "Ballad of Gales arrival";
    case 13: return "jumps out of a hole";
    case 14: return "carried by a Floormaster";
    case 15: return "falls in from above";
    default: return "";
    }
}

struct Spot {
    const char* name;
    const char* stage;
    int room, spawn;
};
// in story order; each at a spawn an exit of the game leads to where there is one, and none whose
// event is a story cutscene (the Helmaroc King's spawn 16 plays "rescue": the game halts, demo_data
// missing, d_event_data.cpp:1103; spawn 20 comes from Ganondorf's room). Not here: the
// Forsaken Fortress tower of the first visit (only loads at that point of the story: black screen on
// spawns 0 and 17, a crash in its cutscene on 16, tested from a later save)
const Spot kStory[] = {
    {"Outset Island", "sea", 44, 0}, {"Link's house", "LinkRM", 0, 1}, {"Orca's house", "Ojhous", 0, 0},
    {"Forest of Fairies (Tetra falls)", "A_mori", 0, 0}, {"Pirate ship interior", "Asoko", 0, 0},
    {"Pirate ship ride to Forsaken Fortress", "A_umikz", 0, 0},
    {"Forsaken Fortress outside (1st visit)", "MajyuE", 0, 0}, {"Forsaken Fortress interior (1st visit)", "majroom", 0, 0},
    {"Windfall Island", "sea", 11, 0},
    {"Dragon Roost Island", "sea", 13, 0}, {"Rito Aerie", "Atorizk", 0, 0}, {"Komali's room", "Comori", 0, 0},
    {"Dragon Roost Cavern entrance", "Adanmae", 0, 2}, {"Dragon Roost Cavern", "M_NewD2", 0, 0},
    {"Gohma", "M_DragB", 0, 0}, {"Forest Haven", "Omori", 0, 0}, {"Forbidden Woods", "kindan", 0, 0},
    {"Mothula miniboss", "kinMB", 10, 0}, {"Kalle Demos", "kinBOSS", 0, 0}, {"Greatfish Isle", "sea", 23, 0},
    {"Jabun's cave", "Pjavdou", 0, 0}, {"Tower of the Gods sector", "sea", 26, 0}, {"Tower of the Gods", "Siren", 0, 0},
    {"Darknut miniboss", "SirenMB", 23, 0}, {"Gohdan", "SirenB", 0, 0}, {"Hyrule Castle", "Hyrule", 0, 2},
    {"Hyrule Castle interior", "Hyroom", 0, 0}, {"Master Sword chamber", "kenroom", 0, 0},
    {"Forsaken Fortress (2nd visit)", "sea", 1, 0}, {"Forsaken Fortress interior (2nd visit)", "ma2room", 0, 0},
    {"Helmaroc King", "M2tower", 0, 20}, {"Ganondorf's room (Forsaken Fortress)", "M2ganon", 0, 0},
    {"Headstone Island (Earth Temple)", "Edaichi", 0, 1}, {"Earth Temple", "M_Dai", 0, 0},
    {"Stalfos miniboss", "M_DaiMB", 12, 0}, {"Jalhalla", "M_DaiB", 0, 0}, {"Gale Isle (Wind Temple)", "Ekaze", 0, 1},
    {"Wind Temple", "kaze", 15, 15}, {"Wizzrobe miniboss", "kazeMB", 6, 0}, {"Molgera", "kazeB", 0, 0},
    {"Ganon's Tower entrance", "GanonA", 0, 0}, {"Phantom Ganon maze", "GanonJ", 1, 0},
    {"Puppet Ganon", "GanonK", 0, 0}, {"Ganondorf (rooftop)", "GTower", 0, 0},
};
const Spot kMinigames[] = {
    {"Sploosh Kaboom", "Kaisen", 0, 0}, {"Boating Course", "Ocean", 0, 0},
    {"Bird-Man Contest (Flight Control Platform)", "sea", 14, 0}, {"Mail sorting (Rito Aerie)", "Atorizk", 0, 0},
    {"Orca's sword training", "Ojhous", 0, 0}, {"Lenzo's pictographs", "Ocmera", 0, 0},
    {"Killer Bees hide-and-seek (Windfall)", "sea", 11, 0}, {"Auction (House of Wealth, at night)", "Orichh", 0, 0},
    {"Savage Labyrinth", "Cave09", 0, 0}, {"Mrs. Marie's school", "Nitiyou", 0, 0},
    {"Nintendo Gallery", "Pfigure", 0, 0}, {"Beedle's shop ship", "Obshop", 1, 0}, {"Tingle Island", "sea", 17, 0},
    {"Cabana Labyrinth", "TF_04", 0, 0}, {"Ghost Ship", "PShip", 2, 0}, {"Islet of Steel", "ShipD", 0, 0},
    {"Submarine", "Abship", 0, 0}, {"Diamond Steppe warp maze", "WarpD", 0, 0},
};
const Spot kBosses[] = {
    {"Gohma", "M_DragB", 0, 0}, {"Kalle Demos", "kinBOSS", 0, 0}, {"Gohdan", "SirenB", 0, 0},
    {"Helmaroc King", "M2tower", 0, 20}, {"Jalhalla", "M_DaiB", 0, 0}, {"Molgera", "kazeB", 0, 0},
    {"Moblin (Dragon Roost Cavern)", "M_Dra09", 9, 0}, {"Mothula", "kinMB", 10, 0}, {"Darknut", "SirenMB", 23, 0},
    {"Stalfos", "M_DaiMB", 12, 0}, {"Wizzrobe", "kazeMB", 6, 0}, {"Gohma rematch", "Xboss0", 0, 0},
    {"Kalle Demos rematch", "Xboss1", 0, 0}, {"Jalhalla rematch", "Xboss2", 0, 0}, {"Molgera rematch", "Xboss3", 0, 0},
    {"Phantom Ganon", "GanonM", 0, 0}, {"Puppet Ganon", "GanonK", 0, 0}, {"Ganondorf", "GTower", 0, 0},
};
// stage lists: every stage of the game is in one (the rest end up under Other)
const std::vector<std::string> kDungeons = {
    "Adanmae", "M_NewD2", "M_Dra09", "M_DragB", "kindan", "kinMB", "kinBOSS", "ADMumi", "Siren", "SirenMB", "SirenB",
    "MajyuE", "majroom", "Mjtower", "ma2room", "ma3room", "M2tower", "M2ganon", "Hyrule", "Hyroom", "kenroom",
    "Edaichi", "M_Dai", "M_DaiMB", "M_DaiB", "Ekaze", "kaze", "kazeMB", "kazeB", "GanonA", "GanonB", "GanonC",
    "GanonD", "GanonE", "GanonJ", "GanonK", "GanonL", "GanonM", "GanonN", "GTower", "Xboss0", "Xboss1", "Xboss2",
    "Xboss3"};
const std::vector<std::string> kCaves = {
    "Cave01", "Cave02", "Cave03", "Cave04", "Cave05", "Cave07", "Cave09", "Cave10", "Cave11", "ITest62", "ITest63",
    "MiniHyo", "MiniKaz", "SubD42", "SubD43", "SubD71", "TF_01", "TF_02", "TF_03", "TF_04", "TF_06", "TyuTyu",
    "WarpD", "Otkura", "Pjavdou", "Fairy01", "Fairy02", "Fairy03", "Fairy04", "Fairy05", "Fairy06", "PShip", "ShipD",
    "Abship"};
const std::vector<std::string> kInteriors = {
    "LinkRM", "LinkUG", "Ojhous", "Ojhous2", "Onobuta", "Omasao", "A_mori", "Obombh", "Orichh", "Opub", "Pdrgsh",
    "Kaisen", "Ocmera", "Nitiyou", "Pnezumi", "Atorizk", "Comori", "Omori", "Ocrogh", "Pfigure", "figureA",
    "figureB", "figureC", "figureD", "figureE", "figureF", "figureG", "Abesso", "Obshop", "tincle", "Ocean", "Asoko",
    "A_umikz"};
// not offered: the title screen and file select (no play scene), the ending (story end)
const std::set<std::string> kSkip = {"sea_T", "Name", "ENDumi"};
// exits from these don't name a place (shared ship / test stages)
const std::set<std::string> kGenericExits = {"Abship", "Obshop", "PShip", "PShip2", "PShip3", "sea", "sea_E",
                                             "I_SubAN", "ITest61", "SubD44", "SubD51", "Cave06"};

struct Exit {
    std::string stage;
    int room, spawn;
};
struct Spawn {
    int mode;   // Link's start mode
    int event;  // index into the stage's events (EVNT), 255: none
};
struct Stage {
    std::map<std::pair<int, int>, Spawn> spawns;  // (room, spawn)
    std::vector<Exit> exits;
    std::vector<std::string> events;  // EVNT names, by index
    int save_tbl = -1;                // STAG: the stage's table in the save (dSv_save_c::mMemory)
    std::set<int> room_files;         // the rooms with a <stage>_Room<n>.szs
};
struct Archive {
    std::string stage;
    bool stage_file;   // <stage>_Stage.szs (the events are there)
    int room;          // <stage>_Room<n>.szs: n, else -1
    std::string path;  // a file, or empty: data holds the szs (from a pack)
    Bytes data;
    Stage found;
};

bool is_cc(const uint8_t* p) {
    for (int i = 0; i < 4; i++)
        if (p[i] < 48 || p[i] >= 123) return false;
    return true;
}

// the DZx chunk tables in a bfres, by start offset
std::set<size_t> dzx_tables(const Bytes& b) {
    std::set<size_t> out;
    static const char* const kTags[] = {"PLYR", "SCLS", "FILI", "STAG", "RTBL"};
    for (const char* tag : kTags) {
        for (size_t q = 0; q + 4 <= b.size(); q++) {
            const uint8_t* hit = (const uint8_t*)memmem(&b[q], b.size() - q, tag, 4);
            if (!hit) break;
            q = hit - b.data();
            for (size_t k = 0; k < 80; k++) {
                if (q < 4 + 12 * k) break;
                size_t x = q - 4 - 12 * k;
                uint32_t n = be32(&b[x]);
                if (n <= k || n > 100 || x + 16 + 12 * n > b.size()) continue;
                bool ok = true;
                for (uint32_t i = 0; i < n && ok; i++) ok = is_cc(&b[x + 4 + 12 * i]);
                if (ok && be32(&b[x + 12]) == 4 + 12 * n) {
                    out.insert(x);
                    break;
                }
            }
        }
    }
    return out;
}

void read_dzx(const Bytes& b, size_t x, Stage& st) {
    uint32_t n = be32(&b[x]);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* c = &b[x + 4 + 12 * i];
        uint32_t cnt = be32(c + 4);
        size_t off = x + be32(c + 8);
        if (!memcmp(c, "PLYR", 4)) {
            for (uint32_t j = 0; j < cnt && off + 0x20 * (j + 1) <= b.size(); j++) {
                const uint8_t* e = &b[off + 0x20 * j];
                uint32_t params = be32(e + 8);
                int room = params & 0x3F, spawn = be16(e + 0x1C) & 0xFF;
                if (room != 0x3F && spawn != 0xFF)  // 63 / 255: placeholders
                    st.spawns[{room, spawn}] = {(int)(params >> 12) & 0xF, (int)(params >> 24)};
            }
        } else if (!memcmp(c, "STAG", 4) && cnt && off + 0xA <= b.size()) {  // mProp at +9: save table in bits 1-7
            st.save_tbl = (b[off + 9] >> 1) & 0x7F;
        } else if (!memcmp(c, "EVNT", 4)) {  // 0x18 bytes: name at +1 (15 chars)
            for (uint32_t j = 0; j < cnt && off + 0x18 * (j + 1) <= b.size(); j++) {
                const char* e = (const char*)&b[off + 0x18 * j + 1];
                st.events.push_back(std::string(e, strnlen(e, 15)));
            }
        } else if (!memcmp(c, "SCLS", 4)) {
            for (uint32_t j = 0; j < cnt && off + 0xC * (j + 1) <= b.size(); j++) {
                const uint8_t* e = &b[off + 0xC * j];
                st.exits.push_back({std::string((const char*)e, strnlen((const char*)e, 8)), e[9], e[8]});
            }
        }
    }
}

// "<stage>_Room<n>.szs" -> n, else -1
int room_of(const std::string& name) {
    size_t r = name.rfind("_Room");
    if (r == std::string::npos || name.size() < r + 10 || name.compare(name.size() - 4, 4, ".szs") != 0) return -1;
    std::string n = name.substr(r + 5, name.size() - 4 - (r + 5));
    if (n.empty() || !std::all_of(n.begin(), n.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) return -1;
    return std::stoi(n);
}

// "<stage>_Stage.szs" / "<stage>_Room<n>.szs" -> stage, else ""
std::string stage_of(const std::string& name) {
    static const char kSzs[] = ".szs";
    if (name.size() < 5 || name.compare(name.size() - 4, 4, kSzs) != 0) return "";
    std::string base = name.substr(0, name.size() - 4);
    size_t u = base.rfind('_');
    if (u == std::string::npos || u == 0) return "";
    std::string tail = base.substr(u + 1);
    if (tail == "Stage") return base.substr(0, u);
    if (tail.size() > 4 && tail.compare(0, 4, "Room") == 0 &&
        std::all_of(tail.begin() + 4, tail.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
        return base.substr(0, u);
    return "";
}

std::vector<std::string> list_dir(const std::string& dir) {
    std::vector<std::string> out;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.') out.push_back(e->d_name);
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void json_str(std::string& o, const std::string& s) {
    o += '"';
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\';
        o += ch;
    }
    o += '"';
}

std::string stage_name(const std::string& s) {
    auto it = kStages.find(s);
    return it != kStages.end() ? it->second : s;
}

std::string lower(std::string s) {
    for (char& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}
}  // namespace

std::string build(const std::string& game_dir, const std::string& out_path, int* stages_out, int* spawns_out) {
    std::string common = game_dir + "/content/Common";
    std::vector<Archive> arcs;
    for (const std::string& f : list_dir(common + "/Stage")) {
        std::string st = stage_of(f);
        if (!st.empty()) arcs.push_back({st, f == st + "_Stage.szs", room_of(f), common + "/Stage/" + f, {}, {}});
    }
    for (const std::string& p : list_dir(common + "/Pack")) {
        if (p.size() < 5 || p.compare(p.size() - 5, 5, ".pack") != 0) continue;
        Bytes pack;
        if (!game_archive::read_file(common + "/Pack/" + p, pack)) continue;
        game_archive::sarc_each(pack, [&](const char* name, const uint8_t* d, size_t size) {
            std::string st = stage_of(name);
            if (!st.empty()) arcs.push_back({st, name == st + "_Stage.szs", room_of(name), "", Bytes(d, d + size), {}});
        });
    }
    if (arcs.empty()) return "no stage files in " + common;

    // unpack and read every archive (most of the time), then merge in file order
    std::atomic<size_t> next{0};
    auto work = [&] {
        Bytes raw, szs;
        for (size_t i; (i = next++) < arcs.size();) {
            Archive& a = arcs[i];
            if (!a.path.empty() && !game_archive::read_file(a.path, raw)) continue;
            if (!game_archive::yaz0(a.path.empty() ? a.data : raw, szs)) continue;
            Bytes().swap(a.data);
            game_archive::sarc_each(szs, [&](const char* name, const uint8_t* d, size_t size) {
                size_t len = strlen(name);
                if (len < 6 || strcmp(name + len - 6, ".bfres") != 0) return;
                Bytes bfres(d, d + size);
                for (size_t x : dzx_tables(bfres)) read_dzx(bfres, x, a.found);
            });
        }
    };
    unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
    std::map<std::string, Stage> st;
    for (Archive& a : arcs) {
        Stage& s = st[a.stage];
        for (auto& [k, m] : a.found.spawns) s.spawns[k] = m;
        if (a.room >= 0) s.room_files.insert(a.room);
        if (a.stage_file) {
            s.events = a.found.events;
            s.save_tbl = a.found.save_tbl;
        }
        s.exits.insert(s.exits.end(), a.found.exits.begin(), a.found.exits.end());
    }
    // spawns in a room without a room file: the game halts there (d_s_room.cpp "fileList != 0": e.g.
    // the unused Cave08 lists rooms 5-7 but WWHD only has rooms 1-3)
    int dropped = 0;
    for (auto& [n, s] : st)
        for (auto it = s.spawns.begin(); it != s.spawns.end();)
            if (!s.room_files.count(it->first.first)) {
                it = s.spawns.erase(it);
                dropped++;
            } else {
                ++it;
            }
    int nspawns = 0;
    for (auto& [n, s] : st) nspawns += (int)s.spawns.size();
    if (!st.count("sea")) return "no sea stage found";

    // where each spawn is entered from: "from <place>"
    std::map<std::tuple<std::string, int, int>, std::set<std::string>> came_from;
    for (auto& [src, s] : st) {
        if (kGenericExits.count(src)) continue;
        for (const Exit& e : s.exits) {
            if (e.stage == src || !st.count(e.stage)) continue;
            for (auto& [k, m] : st[e.stage].spawns)  // room 255: the room the spawn is in
                if (k.second == e.spawn && (e.room == 255 || e.room == k.first))
                    came_from[{e.stage, k.first, k.second}].insert(stage_name(src));
        }
    }
    auto label = [&](const std::string& stage, int room, int spawn, const Spawn& sp) {
        int mode = sp.mode;
        std::vector<std::string> parts;
        if (stage == "sea" && spawn >= 100 && spawn <= 103)
            parts.push_back("on the boat (corner " + std::to_string(spawn - 99) + ")");
        else if (*mode_name(mode))
            parts.push_back(mode_name(mode));
        auto it = came_from.find({stage, room, spawn});
        if (it != came_from.end()) {
            std::string f = "from ";
            bool first = true;
            for (const std::string& n : it->second) {  // std::set: sorted, as the names are
                if (!first) f += ", ";
                f += n;
                first = false;
            }
            parts.push_back(f);
        }
        if (spawn >= 200) parts.push_back("cutscene spawn");
        // the spawn's event plays as Link arrives; some story cutscenes (FIND_SISTER, rescue) need what
        // the way there loads and stop the game without it
        const std::vector<std::string>& ev = st[stage].events;
        if (sp.event != 255 && sp.event < (int)ev.size()) parts.push_back("event " + ev[sp.event]);
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) out += (i ? "; " : "") + parts[i];
        return out;
    };

    // version: the app makes an older file again (WarpCatalog.VERSION; 2: no spawns in rooms without files)
    std::string o = "{\"version\":2,\"categories\":[";
    bool first_cat = true;
    auto category = [&](const char* name, const std::function<void(std::string&)>& items) {
        if (!first_cat) o += ',';
        first_cat = false;
        o += "{\"name\":";
        json_str(o, name);
        o += ",\"items\":[";
        size_t start = o.size();
        items(o);
        if (o.size() > start && o.back() == ',') o.pop_back();
        o += "]}";
    };
    auto spawns = [&](std::string& o, const std::string& stage, int only_room) {
        o += "\"spawns\":[";
        bool first = true;
        for (auto& [k, m] : st[stage].spawns) {
            if (only_room >= 0 && k.first != only_room) continue;
            if (!first) o += ',';
            first = false;
            o += "[" + std::to_string(k.first) + "," + std::to_string(k.second) + ",";
            json_str(o, label(stage, k.first, k.second, m));
            o += "]";
        }
        o += "]";
    };
    auto direct = [&](const Spot* spots, size_t n) {
        return [&, spots, n](std::string& o) {
            for (size_t i = 0; i < n; i++) {
                const Spot& s = spots[i];
                auto it = st.find(s.stage);
                if (it == st.end() || !it->second.spawns.count({s.room, s.spawn})) continue;  // not in this game version
                o += "{\"name\":";
                json_str(o, s.name);
                o += ",\"stage\":";
                json_str(o, s.stage);
                o += ",\"room\":" + std::to_string(s.room) + ",\"spawn\":" + std::to_string(s.spawn) + "},";
            }
        };
    };
    auto places = [&](const std::vector<std::string>& stages) {
        return [&, stages](std::string& o) {
            for (const std::string& s : stages) {
                auto it = st.find(s);
                if (it == st.end() || it->second.spawns.empty()) continue;
                o += "{\"name\":";
                json_str(o, stage_name(s));
                o += ",\"stage\":";
                json_str(o, s);
                o += ',';
                spawns(o, s, -1);
                o += "},";
            }
        };
    };
    std::set<std::string> listed(kDungeons.begin(), kDungeons.end());
    listed.insert(kCaves.begin(), kCaves.end());
    listed.insert(kInteriors.begin(), kInteriors.end());
    listed.insert(kSkip.begin(), kSkip.end());
    listed.insert("sea");
    std::vector<std::string> other;
    for (auto& [n, s] : st)
        if (!listed.count(n)) other.push_back(n);
    std::stable_sort(other.begin(), other.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });

    category("Story", direct(kStory, std::size(kStory)));
    category("Minigames", direct(kMinigames, std::size(kMinigames)));
    category("Bosses", direct(kBosses, std::size(kBosses)));
    category("Great Sea", [&](std::string& o) {
        std::set<int> rooms;
        for (auto& [k, m] : st["sea"].spawns) rooms.insert(k.first);
        for (int r : rooms) {
            o += "{\"name\":";
            json_str(o, std::string(r < (int)std::size(kSea) ? kSea[r] : "Sea") + " (" + std::to_string(r) + ")");
            o += ",\"stage\":\"sea\",";
            spawns(o, "sea", r);
            o += "},";
        }
    });
    category("Dungeons", places(kDungeons));
    category("Houses and shops", places(kInteriors));
    category("Caves and fountains", places(kCaves));
    category("Other and unused", places(other));
    o += "]}\n";

    // the stages' save tables (written first: the app takes warps.json without it for an old one)
    // (mods/warp.cpp: a story preset keeps the stage left behind as the preset has it)
    std::string tables;
    for (auto& [n, s] : st)
        if (s.save_tbl >= 0) tables += n + " " + std::to_string(s.save_tbl) + "\n";
    std::string tpath = out_path.substr(0, out_path.find_last_of('/') + 1) + "warp_tables.txt";
    if (FILE* t = fopen(tpath.c_str(), "wb")) {
        fwrite(tables.data(), 1, tables.size(), t);
        fclose(t);
    }
    std::string tmp = out_path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return "cannot write " + tmp;
    bool ok = fwrite(o.data(), 1, o.size(), f) == o.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), out_path.c_str()) != 0) {
        remove(tmp.c_str());
        return "cannot write " + out_path;
    }
    if (stages_out) *stages_out = (int)st.size();
    if (spawns_out) *spawns_out = nspawns;
    return "";
}

}  // namespace warp_catalog
