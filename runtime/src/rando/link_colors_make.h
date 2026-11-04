// Mods > Link's colours (rando_jni.cpp applyColors; tools/rando/host's "colors" test): Link's textures in one save
// file's colours, for mods/link_colors.cpp to put into the game's memory while that file is played. Header only: the
// app's library and the PC test build both take it in.
//
// The randomizer's customizer recolours the game's permanent_3d.pack (Link.szs > Link.bfres) as usual, in a scratch
// session; the textures it changes are taken as they are in memory (each level: data, mipData) before and after:
//   "WWLC" u32 3, u32 count, per entry: u16 name length, name ("<texture>@<index>" or that + "#mip"), u32 size,
//   u32 offset (in the game's Link.bfres), u8 hero (1: a hero's version follows), the game's bytes, the island
//   clothes' bytes[, the hero's bytes]
// (all little endian). The hands, mouth and eyes are one texture each for both clothes: with `separate` the hero's own
// colours for them follow (else both clothes' colours are alike, rando_jni.cpp syncShared). Every other texture belongs
// to one clothes only and is the same in both versions.
#pragma once
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <command/Log.hpp>
#include <command/RandoSession.hpp>
#include <filetypes/bfres.hpp>
#include <seedgen/config.hpp>
#include <utility/path.hpp>
#include <utility/platform.hpp>

// "" when done, else why not
inline std::string make_link_colors(const Config& c, const fspath& game, const fspath& outFile, bool separate) {
    const fspath tmp = wwhd_rando::save_path / "apply";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    ErrorLog::getInstance().clearLastErrors();
    if (!g_session.init(game, tmp)) return "cannot start: " + ErrorLog::getInstance().getLastErrors();
    g_session.setFirstTimeSetup(false);  // (only the file changed, not a copy of the whole game)
    using Pass = CustomModel::Pass;
    CustomModel hero = c.settings.selectedModel, casual = c.settings.selectedModel;
    hero.casual = false;
    casual.casual = true;
    // the textures the customizer colours (customizer/model.cpp's mappings); shared: the ones both clothes draw with
    auto colored = [](const std::string& n) {
        for (const char* p : {"linktex", "katsuraS3TC", "handsS3TC", "mouthS3TC", "hitomi"})
            if (n.rfind(p, 0) == 0) return true;
        return false;
    };
    auto shared = [](const std::string& n) { return n.rfind("linktex", 0) != 0 && n.rfind("katsura", 0) != 0; };
    // per texture level: as the game has it, the hero's shared ones, all after both passes
    std::map<std::string, std::string> orig, heroTex, done;
    std::map<std::string, uint32_t> offset;  // (read with the game's file: before any change)
    auto capture = [&](std::map<std::string, std::string>& into, bool restoreShared) {
        RandoSession::CacheEntry& link = g_session.openGameFile("content/Common/Pack/permanent_3d.pack@SARC@Link.szs@YAZ0@SARC@Link.bfres@BFRES");
        link.addAction([&, restoreShared](RandoSession*, FileType* d) -> int {
            auto* bfres = dynamic_cast<FileTypes::resFile*>(d);
            if (!bfres) return 1;
            for (size_t i = 0; i < bfres->textures.size(); i++) {
                FileTypes::Subfiles::FTEXFile& t = bfres->textures[i];
                if (!colored(t.name)) continue;
                const std::string key = t.name + "@" + std::to_string(i);
                into[key] = t.data;
                into[key + "#mip"] = t.mipData;
                if (&into == &orig) offset[key] = (uint32_t)t.dataOffset.offset, offset[key + "#mip"] = (uint32_t)t.mipOffset.offset;
                if (restoreShared && shared(t.name)) t.data = orig[key], t.mipData = orig[key + "#mip"];
            }
            return 0;
        });
    };
    capture(orig, false);
    ModelError e = hero.applyModel(Pass::Body);
    if (separate && e == ModelError::NONE) {
        e = hero.applyModel(Pass::Shared);
        capture(heroTex, true);  // (then the shared ones as they were, for the island clothes' pass)
    }
    if (e == ModelError::NONE) e = casual.applyModel();
    capture(done, false);
    if (e != ModelError::NONE) return "cannot colour Link";
    if (!g_session.modFiles()) return "cannot colour Link: " + ErrorLog::getInstance().getLastErrors();
    std::filesystem::remove_all(tmp, ec);  // (the repacked game file: not used, the textures go straight into memory)

    std::string b = "WWLC";
    auto u32 = [&](uint32_t v) { b.append((const char*)&v, 4); };
    u32(3);
    const size_t countAt = b.size();
    u32(0);
    uint32_t count = 0;
    for (const auto& [key, game] : orig) {
        const std::string& cas = done[key];
        auto h = heroTex.find(key);
        const bool ownHero = separate && h != heroTex.end() && shared(key) && h->second != cas;
        if (game.empty() || cas.size() != game.size() || (ownHero && h->second.size() != game.size())) continue;
        if (cas == game && !ownHero) continue;  // (not coloured)
        uint16_t len = (uint16_t)key.size();
        b.append((const char*)&len, 2);
        b += key;
        u32((uint32_t)game.size());
        u32(offset[key]);
        b += (char)(ownHero ? 1 : 0);
        b += game;
        b += cas;
        if (ownHero) b += h->second;
        count++;
    }
    memcpy(&b[countAt], &count, 4);
    std::filesystem::create_directories(outFile.parent_path(), ec);
    const fspath part = outFile.string() + ".part";
    {
        std::ofstream f(part, std::ios::binary | std::ios::trunc);
        f.write(b.data(), (std::streamsize)b.size());
        if (!f) return "cannot write " + outFile.string();
    }
    std::filesystem::rename(part, outFile, ec);
    if (ec) return "cannot write " + outFile.string() + ": " + ec.message();
    Utility::platformLog("Link's colours: " + std::to_string(count) + " texture levels, " + std::to_string(b.size() >> 10) + " KB");
    return "";
}
