// save_paths_test: unit tests for which of the game's files a co-op session may change (src/game/save_paths.h):
// every file seen in the profile folder (Data\<steamid>\profiles\temporaryprofile) and the save slots, with the
// game's mixed / and \ paths, classified and mapped into the scratch tree (or left alone). The 0.15.0 bug: garage.pf
// was written for real by host and guest during a co-op chapter. The 2026-10-20 bug: the HOST's garage.pf (and his
// aslot\save1.sav, aslot.pf) were still written for real in a co-op Free Ride: the logs of that test
// (tests/data/coop_file_writes.h, every save-system event in order) are replayed through save_paths::Guard, the
// decision state save_guard runs on, with the number of real writes in co-op before (as logged) and after.
// Prints PASS/FAIL per check; exits non-zero on any failure.
#include <cstdio>
#include <cstring>
#include <string>

#include "data/coop_file_writes.h"
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

// ---- replaying a logged session through the Guard ----
// mission_sync calls Want(following || hostChapter, hosting) every session frame and Want(false, false) at the end;
// module's RenderTick calls Tick(main menu up) every frame (Guard::MainMenu); LoadDetour calls Load(storage). The
// log's events stand for those.
struct Replay {
    Guard g;
    int coopWrites = 0;  // writes of saves / slot profiles / progress / unknown profile files while in co-op
    int realBefore = 0;  // ...that 0.16.0 wrote into the real files (the log says "written:")
    int realAfter = 0;   // ...that the Guard sends to the real files now
    int settings = 0, settingsReal = 0;
    std::string firstReal;  // the first co-op write still real
};

enum class Role { None, Guest, Host };

// story: every world load is from the story slot (a STORY host's CONTINUE) instead of the logged storage.
// until: stop before the first event at or after this time ("" = all; a crash there).
Replay Run(const coop_file_writes::Event* ev, size_t n, bool story = false, const char* until = "") {
    Replay r;
    Role role = Role::None;
    bool inCoop = false;  // from a session's start until the main menu after its end (the co-op world is gone)
    for (size_t i = 0; i < n; ++i) {
        const auto& e = ev[i];
        if (*until && std::strcmp(e.time, until) >= 0) break;
        const std::string type = e.type;
        if (type == "host") role = Role::Host, inCoop = true;
        if (type == "guest") role = Role::Guest, inCoop = true;
        if (type == "end") role = Role::None;
        r.g.Want(role == Role::Guest, role == Role::Host);  // every session frame (Detach: (false, false))
        if (type == "menu") {
            r.g.MainMenu();
            if (role == Role::None) inCoop = false;
        }
        if (type == "load") r.g.Load(story ? "aslot" : e.arg, e.logged);  // (a load's `logged`: the save's id)
        if (type != "write") continue;
        const Mapped m = Classify(kData + "/" + e.arg);
        const Where w = r.g.Route(m, true);
        if (m.kind == Kind::Setting) {
            ++r.settings;
            if (w == Where::Real) ++r.settingsReal;
            continue;
        }
        if (!inCoop) continue;
        ++r.coopWrites;
        if (std::strcmp(e.logged, "real") == 0) ++r.realBefore;
        if (w == Where::Real) {
            ++r.realAfter;
            if (r.firstReal.empty()) r.firstReal = std::string(e.time) + " " + e.arg;
        }
    }
    return r;
}

// Where the logged write at `time` goes: the replay up to and including it, then that write routed again (the same
// state; routing a write twice changes nothing).
Where WriteAt(const coop_file_writes::Event* ev, size_t n, const char* time, bool story = false) {
    const char* next = "99";
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(ev[i].time, time) > 0) {
            next = ev[i].time;
            break;
        }
    Replay r = Run(ev, n, story, next);
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(ev[i].time, time) == 0 && std::strcmp(ev[i].type, "write") == 0)
            return r.g.Route(Classify(kData + "/" + ev[i].arg), true);
    std::printf("      (no write at %s in the log)\n", time);
    return Where::Real;
}

const char* WhereName(Where w) { return w == Where::Scratch ? "scratch" : w == Where::Held ? "held" : "real"; }

void Report(const char* name, const Replay& r) {
    std::printf("      %-34s co-op writes %2d: real before %d, after %d; settings %d (real %d)%s%s\n", name,
                r.coopWrites, r.realBefore, r.realAfter, r.settings, r.settingsReal,
                r.firstReal.empty() ? "" : "; first real: ", r.firstReal.c_str());
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

    // ---- scopes ----
    Check(KeptOut(Kind::Save, Scope::All) && KeptOut(Kind::SlotProfile, Scope::All) &&
              KeptOut(Kind::Progress, Scope::All) && KeptOut(Kind::UnknownProfile, Scope::All) &&
              !KeptOut(Kind::Setting, Scope::All),
          "scope All: saves, slot profiles, progress, unknown; not settings");
    Check(!KeptOut(Kind::Save, Scope::Progress) && !KeptOut(Kind::SlotProfile, Scope::Progress) &&
              KeptOut(Kind::Progress, Scope::Progress) && KeptOut(Kind::UnknownProfile, Scope::Progress),
          "scope Progress (STORY host): story saves and aslot.pf his, garage not");
    Check(!KeptOut(Kind::Progress, Scope::None) && !KeptOut(Kind::Save, Scope::None), "scope None: nothing");

    const Mapped garageM = Classify(kProfile + "garage.pf");
    const Mapped saveM = Classify(kData + "/gamesaves/default/aslot/save1.sav");
    const Mapped aslotM = Classify(kProfile + "aslot.pf");
    const Mapped settingsM = Classify(kProfile + "gamesettings.pf");

    // ---- single player, no session: untouched ----
    {
        Guard g;
        g.Load("aslot");
        g.MainMenu();
        Check(g.Current() == Scope::None && g.Route(garageM, true) == Where::Real &&
                  g.Route(saveM, true) == Where::Real && g.Route(aslotM, true) == Where::Real &&
                  g.Route(settingsM, true) == Where::Real && !g.AnyHeld(),
              "no session: every file real, nothing held");
    }

    // ---- the host, by the world he is in ----
    {
        Guard g;  // hosting at the main menu, then a lobby FREE RIDE (the menu's chapter loader)
        g.Want(false, true);
        g.Load("debugloadchapter");
        Check(g.Current() == Scope::All, "host in FREE RIDE: saves and profile kept out", ScopeName(g.Current()));
        Check(g.Route(garageM, true) == Where::Scratch && g.Route(saveM, true) == Where::Scratch &&
                  g.Route(aslotM, true) == Where::Scratch && g.Route(settingsM, true) == Where::Real,
              "  ...garage, save1.sav, aslot.pf to scratch; settings real");
        g.Load("aslot");  // STORY: CONTINUE loads his story
        Check(g.Current() == Scope::Progress, "host in his STORY: profile only", ScopeName(g.Current()));
        Check(g.Route(garageM, true) == Where::Scratch && g.Route(saveM, true) == Where::Real &&
                  g.Route(aslotM, true) == Where::Real,
              "  ...garage to scratch, his story save and aslot.pf real");
    }
    {
        Guard g;  // he starts hosting from inside his single-player story world
        g.Load("aslot");
        Check(g.Route(garageM, true) == Where::Real, "single-player story before hosting: garage real");
        g.Want(false, true);
        Check(g.Current() == Scope::Progress && g.Route(garageM, true) == Where::Scratch &&
                  g.Route(saveM, true) == Where::Real,
              "hosting from his own story world: garage kept out, story saves his");
        Guard f;  // ...or from inside his single-player Free Ride
        f.Load("debugloadchapter");
        f.Want(false, true);
        Check(f.Current() == Scope::All && f.Route(saveM, true) == Where::Scratch,
              "hosting from his single-player Free Ride world: its saves kept out too");
    }
    {
        Guard g;  // a lobby CHAPTER host: Block before the load, kept while the session wants it
        g.Want(false, true);
        g.Load("aslot");  // his last single-player world was his story
        g.SetBlock(true);
        g.MainMenu();  // still at the menu, the chapter's load requested
        Check(g.Current() == Scope::All, "CHAPTER host: everything kept out (Block), also over a story world");
        g.Want(true, true);
        g.MainMenu();
        Check(g.Block(), "  ...the main menu does not end what the session still wants");
        g.Want(false, true);  // the chapter is over, still hosting
        Check(g.Block(), "  ...no longer wanted: still on in the chapter's world");
        g.MainMenu();
        Check(!g.Block() && g.Host(), "  ...ended at the main menu; hosting stays");
    }

    // ---- session end, held files, a crash ----
    {
        Guard g;
        g.Want(false, true);
        g.Load("debugloadchapter");
        g.Route(garageM, true);  // a garage visit
        g.Want(false, false);    // STOP HOSTING in the pause menu
        Check(g.Current() == Scope::All && g.Route(saveM, true) == Where::Scratch,
              "session ended in the world: its quit save still kept out");
        g.MainMenu();
        Check(g.Current() == Scope::None, "main menu after the session: nothing kept out");
        Check(g.Route(garageM, true) == Where::Held && g.Route(garageM, false) == Where::Held,
              "  ...the garage the session wrote stays held", g.HeldList());
        Check(g.Route(saveM, true) == Where::Real && g.Route(aslotM, true) == Where::Real &&
                  g.Route(settingsM, true) == Where::Real,
              "  ...saves, slot profiles and settings real again (single player)");
        Guard next;  // the next start of the game (after a restart or a crash): nothing of the shadow is used
        Check(next.Route(garageM, false) == Where::Real && next.Route(garageM, true) == Where::Real && !next.AnyHeld(),
              "next process: the real garage is read and written, no shadow");
    }

    // ---- the 2026-10-20 two-PC test, replayed (tests/data/coop_file_writes.h) ----
    using namespace coop_file_writes;
    constexpr size_t nAdam = sizeof adam / sizeof adam[0];
    constexpr size_t nAdam2 = sizeof adam2 / sizeof adam2[0];
    constexpr size_t nMiles = sizeof miles / sizeof miles[0];
    // Adam hosts session A (lobby FREE RIDE), takes vehicles from the Free Ride garage, stops hosting in the world.
    for (const char* t : {"02:41:48.504", "02:42:08.569", "02:42:23.955", "02:42:54.100"}) {
        const Where w = WriteAt(adam, nAdam, t);
        Check(w == Where::Scratch, (std::string("Adam host ") + t + " garage.pf kept out (0.16.0: real)").c_str(),
              WhereName(w));
    }
    for (const char* t : {"02:41:33.384", "02:43:00.235", "02:45:34.399", "02:45:38.836"}) {
        const Where w = WriteAt(adam, nAdam, t);
        Check(w == Where::Scratch, (std::string("Adam host ") + t + " save1.sav / aslot.pf kept out (was real)").c_str(),
              WhereName(w));
    }
    for (const char* t : {"02:41:23.960", "02:43:39.943", "02:45:34.389", "02:45:40.684"})
        Check(WriteAt(adam, nAdam, t) == Where::Real, (std::string("Adam host ") + t + " a setting stays real").c_str());
    const Replay a = Run(adam, nAdam), a2 = Run(adam2, nAdam2), mi = Run(miles, nMiles);
    Report("Adam (host A, guest B start)", a);
    Report("Adam (guest B)", a2);
    Report("Miles (guest A, host B)", mi);
    Check(a.realBefore == 8 && a.realAfter == 0, "Adam: 8 real co-op writes in 0.16.0, now 0",
          std::to_string(a.realBefore) + " -> " + std::to_string(a.realAfter));
    Check(a2.realBefore == 0 && a2.realAfter == 0 && a2.coopWrites > 0, "Adam as a guest: none before, none now");
    Check(mi.realBefore == 4 && mi.realAfter == 0, "Miles: 4 real co-op writes (save1.sav as host) in 0.16.0, now 0",
          std::to_string(mi.realBefore) + " -> " + std::to_string(mi.realAfter));
    Check(a.settings == a.settingsReal && a2.settings == a2.settingsReal && mi.settings == mi.settingsReal,
          "every settings write real in all three logs");
    {
        const Where w = WriteAt(miles, nMiles, "02:49:11.383");
        Check(w == Where::Scratch, "Miles host B 02:49:11 garage.pf kept out (0.16.0: held by luck)", WhereName(w));
        const Where s1 = WriteAt(miles, nMiles, "02:46:40.714");
        Check(s1 == Where::Scratch, "Miles host B (from his Free Ride world) 02:46:40 save1.sav kept out",
              WhereName(s1));
    }
    {
        // After Adam's session, at the main menu (02:45:45): single player again, the garage held.
        Replay after = Run(adam, nAdam, false, "02:46:00");
        Check(after.g.Current() == Scope::None && after.g.Route(garageM, true) == Where::Held &&
                  after.g.Route(saveM, true) == Where::Real,
              "Adam after session A, main menu: saves real, garage held", after.g.HeldList());
    }
    {
        // The same session as a STORY host (his world loaded from the story slot): his story saves stay his.
        const Replay st = Run(adam, nAdam, true);
        Report("Adam's session as a STORY host", st);
        Check(WriteAt(adam, nAdam, "02:41:48.504", true) == Where::Scratch &&
                  WriteAt(adam, nAdam, "02:42:23.955", true) == Where::Scratch,
              "STORY host: 02:41:48 / 02:42:23 garage.pf kept out");
        Check(WriteAt(adam, nAdam, "02:41:33.384", true) == Where::Real &&
                  WriteAt(adam, nAdam, "02:45:34.399", true) == Where::Real,
              "STORY host: his save1.sav and aslot.pf written as in single player");
        Check(st.realAfter == 4, "STORY host: only his 3 story saves and aslot.pf real", std::to_string(st.realAfter));
    }
    {
        // The 2026-10-21 one-PC run: the Free Ride host died; the respawn loaded 'freeroam_player_death' from the story
        // slot (aslot), which made him a STORY host, and his real aslot\save1.sav was written at once (0.17.0).
        constexpr size_t nDeath = sizeof host_death / sizeof host_death[0];
        const Replay d = Run(host_death, nDeath);
        Report("Free Ride host who died (one PC)", d);
        const Where w = WriteAt(host_death, nDeath, "03:36:44.263");
        Check(w == Where::Scratch, "Free Ride host's death respawn 03:36:44 save1.sav kept out (0.17.0: real)",
              WhereName(w));
        Check(d.realBefore == 1 && d.realAfter == 0, "Free Ride host who died: 1 real co-op write in 0.17.0, now 0",
              std::to_string(d.realBefore) + " -> " + std::to_string(d.realAfter));
        Check(d.g.Current() == Scope::All && !d.g.StoryWorld(), "...still a Free Ride (scope All) after the respawn");
        Guard s;  // a STORY host (his own story from the slot): a free roam id does not change that
        s.Want(false, true);
        s.Load("aslot", "chapter_checkpoint");
        s.Load("aslot", "freeroam_player_death");
        Check(s.StoryWorld() && s.Current() == Scope::Progress, "STORY host: a freeroam_* load keeps his story");
        Guard c;  // CONTINUE from the menu after a Free Ride session: a story load
        c.Want(false, true);
        c.Load("debugloadchapter", "freeroam_test");
        c.Load("aslot", "mm_110_omerta_cp_020");
        Check(c.StoryWorld(), "a story id from the slot after a Free Ride: his story again");
    }
    {
        // A crash in the middle of Adam's session (after the 02:41:48 garage write): nothing real was written.
        const Replay c = Run(adam, nAdam, false, "02:42:00");
        Check(c.realAfter == 0 && c.coopWrites == 2, "crash at 02:42: none of the 2 co-op writes reached the real files",
              std::to_string(c.coopWrites) + " writes, " + std::to_string(c.realAfter) + " real");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
