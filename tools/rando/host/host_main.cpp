// The app's randomizer run (runtime/src/rando/rando_jni.cpp, generate) on the PC, for testing releases.
#include <cstdio>
#include <filesystem>

#include <command/Log.hpp>
#include <command/RandoSession.hpp>

#include "../../../runtime/src/rando/link_colors_make.h"
#include <randomizer.hpp>
#include <seedgen/config.hpp>
#include <utility/path.hpp>
#include <utility/wwhd_region.hpp>

namespace wwhd_rando {
void progress_value(int) {}
void progress_label(const std::string& label) { printf("%s\n", label.c_str()); }
}  // namespace wwhd_rando

// Mods > Link's colours (rando_jni.cpp applyColors): the game's permanent_3d.pack with test colours for both clothes
static int colors(char** argv, const char* argc_shared) {
    wwhd_rando::data_path = fspath(argv[2]) / "";
    wwhd_rando::save_path = fspath(argv[3]) / "";
    std::filesystem::create_directories(argv[3]);
    Config::writeDefault(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml");
    Config c;
    c.loadFromFile(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml", true);
    CustomModel& m = c.settings.selectedModel;
    m.casual = false;
    m.setColor("Tunic", "C02020");
    m.setColor("Skin", "8B5A2B");
    m.setColor("Eyes", "20A0FF");
    m.casual = true;
    m.setColor("Shirt", "20C020");
    m.setColor("Skin", "F0C0A0");
    const std::string err = make_link_colors(c, argv[4], argv[5], argc_shared[0] != 0);
    if (!err.empty()) return fprintf(stderr, "%s\n", err.c_str()), 1;
    printf("ok\n");
    return 0;
}

// both clothes' colours kept in preferences.yaml (switching the clothes lost the other one's)
static int prefsTest(char** argv) {
    wwhd_rando::data_path = fspath(argv[2]) / "";
    wwhd_rando::save_path = fspath(argv[3]) / "";
    const fspath cfg = wwhd_rando::save_path / "config.yaml", prefs = wwhd_rando::save_path / "preferences.yaml";
    Config::writeDefault(cfg, prefs);
    Config c;
    c.loadFromFile(cfg, prefs, true);
    c.settings.selectedModel.casual = false;
    c.settings.selectedModel.setColor("Tunic", "C02020");
    c.settings.selectedModel.casual = true;
    c.settings.selectedModel.setColor("Shirt", "20C020");
    c.writeToFile(cfg, prefs);
    Config d;
    d.loadFromFile(cfg, prefs, true);
    const bool worn = d.settings.selectedModel.casual;
    d.settings.selectedModel.casual = false;
    std::string tunic = d.settings.selectedModel.getColor("Tunic");
    d.settings.selectedModel.casual = true;
    std::string shirt = d.settings.selectedModel.getColor("Shirt");
    printf("worn casual %d, hero Tunic %s, casual Shirt %s\n", worn, tunic.c_str(), shirt.c_str());
    return tunic == "C02020" && shirt == "20C020" ? 0 : 1;
}

// the app's calls one by one (each loads, changes, saves): clothes, a colour, other clothes, a colour
static Config loadC() {
    Config c;
    c.loadFromFile(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml", true);
    return c;
}
static void saveC(const Config& c) { c.writeToFile(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml"); }
static std::string colorOf(bool casual, const char* n) {
    Config c = loadC();
    c.settings.selectedModel.casual = casual;
    return c.settings.selectedModel.getColor(n);
}
static int seqTest(char** argv) {
    wwhd_rando::data_path = fspath(argv[2]) / "";
    wwhd_rando::save_path = fspath(argv[3]) / "";
    Config::writeDefault(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml");
    auto step = [](bool casual, const char* n, const char* col) {
        { Config c = loadC(); c.settings.selectedModel.casual = casual; saveC(c); }
        { Config c = loadC(); c.settings.selectedModel.setColor(n, col); saveC(c); }
        printf("after %s %s=%s: hero Hair %s Tunic %s | casual Hair %s Shirt %s\n", casual ? "casual" : "hero", n, col,
               colorOf(false, "Hair").c_str(), colorOf(false, "Tunic").c_str(), colorOf(true, "Hair").c_str(), colorOf(true, "Shirt").c_str());
    };
    step(false, "Hair", "111111");
    step(true, "Hair", "222222");
    step(false, "Tunic", "333333");
    step(true, "Shirt", "444444");
    step(false, "Hair", "555555");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "seq") return seqTest(argv);
    if (argc == 4 && std::string(argv[1]) == "prefs") return prefsTest(argv);
    if ((argc == 6 || argc == 7) && std::string(argv[1]) == "colors") return colors(argv, argc == 7 ? argv[6] : "");
    if (argc != 5) {
        fprintf(stderr, "usage: rando_host DATA WORK GAME OUT\n");
        return 2;
    }
    wwhd_rando::data_path = fspath(argv[1]) / "";
    wwhd_rando::save_path = fspath(argv[2]) / "";
    std::filesystem::create_directories(argv[2]);
    std::filesystem::create_directories(Utility::get_logs_path());
    Config::writeDefault(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml");
    Config c;
    c.loadFromFile(wwhd_rando::save_path / "config.yaml", wwhd_rando::save_path / "preferences.yaml", true);
    c.gameBaseDir = argv[3];
    c.outputDir = argv[4];
    for (const char* d : {"code", "content", "meta"}) std::filesystem::create_directories(c.outputDir / d);
    std::filesystem::copy_file(c.gameBaseDir / "meta/meta.xml", c.outputDir / "meta/meta.xml", std::filesystem::copy_options::overwrite_existing);
    std::string err;
    if (!wwhd_rando::set_region(c.gameBaseDir, err)) {
        fprintf(stderr, "region: %s\n", err.c_str());
        return 1;
    }
    if (wwhdRandomize(c) != 0) {
        fprintf(stderr, "FAILED:\n%s\n", ErrorLog::getInstance().getLastErrors().c_str());
        return 1;
    }
    if (wwhd_rando::port_failed()) {
        fprintf(stderr, "PORT FAILED:\n%s\n", wwhd_rando::port_errors().c_str());
        return 1;
    }
    printf("ok %s hash %s\n", wwhd_rando::region_name(), LogInfo::getSeedHash().c_str());
    return 0;
}
