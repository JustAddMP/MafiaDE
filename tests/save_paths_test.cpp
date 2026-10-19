// save_paths_test: unit tests for which of the game's files a co-op session may change (src/game/save_paths.h):
// every file seen in the profile folder (Data\<steamid>\profiles\temporaryprofile) and the save slots, with the
// game's mixed / and \ paths, classified and mapped into the scratch tree (or left alone). The 0.15.0 bug: garage.pf
// was written for real by host and guest during a co-op chapter. Prints PASS/FAIL per check; exits non-zero on any
// failure.
#include <cstdio>
#include <string>

#include "game/save_paths.h"

using namespace mc::game::save_paths;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-64s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

// As the game passes them (re-notes 7: "...\Data\/<steamid>/gamesaves/default/bslot/save1.sav").
const std::string kData = "C:\\Users\\p\\OneDrive\\Documents\\My Games\\Mafia Definitive Edition\\Data\\/76561198045004490";
const std::string kProfile = kData + "/profiles/temporaryprofile/";
const std::string kSaves = "C:\\Users\\p\\OneDrive\\Documents\\My Games\\Mafia Definitive Edition\\Saves\\";
const std::string kScratch = "C:\\Users\\p\\AppData\\Local\\Temp\\MafiaCoop\\coop-saves\\4242";

void ExpectKind(const std::string& path, Kind want, const char* name) {
    const Mapped m = Classify(path);
    Check(m.kind == want, name, std::string(KindName(m.kind)) + " (" + m.file + ")");
}

}  // namespace

int main() {
    // ---- the save slots ----
    ExpectKind(kData + "/gamesaves/default/aslot/save1.sav", Kind::Save, "story save is a save");
    ExpectKind(kData + "/gamesaves/default/bslot/save0.sav", Kind::Save, "Free Ride save is a save");
    Check(Target(kData + "/gamesaves/default/bslot/save1.sav", kScratch) ==
              kScratch + "\\gamesaves\\default\\bslot\\save1.sav",
          "save maps into the scratch gamesaves tree", Target(kData + "/gamesaves/default/bslot/save1.sav", kScratch));
    ExpectKind(kData + "/gamesaves/", Kind::None, "the gamesaves folder itself is left alone");

    // ---- slot profiles ----
    for (const char* f : {"aslot.pf", "bslot.pf", "cslot.pf"}) ExpectKind(kProfile + f, Kind::SlotProfile, f);

    // ---- progress: garage (the 0.15.0 bug), stats, persistent data ----
    ExpectKind(kProfile + "garage.pf", Kind::Progress, "garage.pf is progress");
    ExpectKind(kProfile + "GARAGE.PF", Kind::Progress, "garage.pf in upper case too");
    ExpectKind(kProfile + "my2k_game_stats.pf", Kind::Progress, "my2k_game_stats.pf is progress");
    ExpectKind(kProfile + "profile_data_storage_persistantdata.pf", Kind::Progress, "persistent data is progress");
    const std::string garage = Target(kProfile + "garage.pf", kScratch);
    Check(garage == kScratch + "\\profiles\\temporaryprofile\\garage.pf", "garage.pf maps into the scratch tree", garage);
    Check(Target(kProfile + "garage.pf", kScratch + "-held") ==
              kScratch + "-held\\profiles\\temporaryprofile\\garage.pf",
          "garage.pf maps into the held tree");
    Check(Redirected(Kind::Progress) && HeldAfterSession(Kind::Progress), "progress: redirected and held");
    Check(Redirected(Kind::SlotProfile) && !HeldAfterSession(Kind::SlotProfile), "slot profile: redirected, not held");
    Check(Redirected(Kind::Save) && !HeldAfterSession(Kind::Save), "save: redirected, not held");

    // ---- the player's own settings: left alone ----
    for (const char* f : {"gamesettings.pf", "gameinputmodule.pf", "profile_language.pf", "profile_audiosettings.pf",
                          "profile_videosettings.pf", "map_filter.pf"}) {
        ExpectKind(kProfile + f, Kind::Setting, f);
        Check(Target(kProfile + f, kScratch).empty(), "  ...and is not redirected");
    }
    ExpectKind(kSaves + "videoconfig.cfg", Kind::Setting, "videoconfig.cfg is a setting");
    ExpectKind(kSaves + "launcconfig.cfg", Kind::Setting, "launcconfig.cfg is a setting");
    Check(Target(kSaves + "videoconfig.cfg", kScratch).empty(), "videoconfig.cfg is not redirected");
    Check(!Redirected(Kind::Setting) && !HeldAfterSession(Kind::Setting), "setting: neither redirected nor held");

    // ---- unknown profile files: kept out to be safe ----
    ExpectKind(kProfile + "some_new_block.pf", Kind::UnknownProfile, "an unknown *.pf is an unknown profile file");
    Check(Redirected(Kind::UnknownProfile) && HeldAfterSession(Kind::UnknownProfile), "unknown: redirected and held");
    ExpectKind(kProfile + "garage.pf.tmp", Kind::None, "a non-.pf file in profiles is left alone");

    // ---- everything else ----
    ExpectKind(kData + "/steam_autocloud.vdf", Kind::None, "steam_autocloud.vdf is left alone");
    ExpectKind("G:\\SteamLibrary\\steamapps\\common\\Mafia Definitive Edition\\sds_retail\\x.sds", Kind::None,
               "game data is left alone");
    ExpectKind("", Kind::None, "empty path");
    ExpectKind("garage.pf", Kind::None, "a bare garage.pf (no profiles folder) is left alone");

    // ---- our own trees are never redirected again (no scratch-in-scratch) ----
    ExpectKind(kScratch + "\\gamesaves\\default\\bslot\\save1.sav", Kind::None, "a scratch save is left alone");
    ExpectKind(kScratch + "-held\\profiles\\temporaryprofile\\garage.pf", Kind::None, "a held garage is left alone");
    ExpectKind("c:/users/p/appdata/local/temp/MafiaCoop/coop-saves/1/profiles/x/garage.pf", Kind::None,
               "scratch with forward slashes too");

    // ---- path details ----
    const Mapped m = Classify(kData + "//profiles//temporaryprofile/Garage.pf");
    Check(m.file == "garage.pf", "file name is lower case", m.file);
    Check(Join(kScratch, m.sub) == kScratch + "\\profiles\\temporaryprofile\\Garage.pf",
          "doubled separators collapse, case kept", Join(kScratch, m.sub));
    Check(Join(kScratch + "\\", "\\profiles\\a.pf") == kScratch + "\\profiles\\a.pf", "Join: root with a slash");
    Check(Join(kScratch, "").empty() && Join("", "\\profiles\\a.pf").empty(), "Join: empty parts give nothing");
    Check(std::string(Why(Classify(kProfile + "garage.pf"))).find("garage") != std::string::npos, "Why names the garage");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
