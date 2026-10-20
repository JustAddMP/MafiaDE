// Generated from the 2026-10-20 two-PC test logs (Adam: mafiacoop.log.1, host of session A then guest of B;
// Miles: mafiacoop.log, guest of A then host of B) by a script outside the repo: every save-system event of
// save_guard's log in order. tests/save_paths_test.cpp replays them through save_paths::Guard.
// type: host / guest (a session starts), end (it ended), menu (the main menu is up), load (arg = storage),
// write (arg = path from profiles/ or gamesaves/ on; logged = where 0.16.0 put it: real, scratch, held).
#pragma once

namespace coop_file_writes {

struct Event {
    const char* time;
    const char* type;
    const char* arg;
    const char* logged;
};

constexpr Event adam[] = {
    {"02:39:36.768", "menu", "", ""},
    {"02:39:38.313", "host", "", ""},
    {"02:39:46.504", "menu", "", ""},
    {"02:41:22.486", "load", "debugloadchapter", ""},
    {"02:41:23.960", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:41:23.986", "load", "debugloadchapter", ""},
    {"02:41:33.384", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:41:48.504", "write", "profiles/temporaryprofile/garage.pf", "real"},
    {"02:42:08.569", "write", "profiles/temporaryprofile/garage.pf", "real"},
    {"02:42:23.955", "write", "profiles/temporaryprofile/garage.pf", "real"},
    {"02:42:54.100", "write", "profiles/temporaryprofile/garage.pf", "real"},
    {"02:43:00.235", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:43:33.556", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:43:39.943", "write", "profiles/temporaryprofile/profile_videosettings.pf", "real"},
    {"02:45:30.512", "write", "profiles/temporaryprofile/profile_videosettings.pf", "real"},
    {"02:45:33.442", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:45:34.389", "write", "profiles/temporaryprofile/gameinputmodule.pf", "real"},
    {"02:45:34.399", "write", "profiles/temporaryprofile/aslot.pf", "real"},
    {"02:45:37.084", "end", "", ""},
    {"02:45:38.836", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:45:40.684", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:45:45.586", "menu", "", ""},
    {"02:46:19.315", "guest", "", ""},
    {"02:46:53.694", "end", "", ""},
    {"02:46:56.189", "menu", "", ""},
    {"02:47:01.508", "end", "", ""},
    {"02:47:03.491", "menu", "", ""},
    {"02:47:08.941", "end", "", ""},
    {"02:47:13.331", "menu", "", ""},
    {"02:47:18.433", "end", "", ""},
    {"02:47:19.668", "menu", "", ""},
};

constexpr Event adam2[] = {
    {"02:48:44.449", "guest", "", ""},
    {"02:48:49.136", "menu", "", ""},
    {"02:48:49.160", "load", "debugloadchapter", ""},
    {"02:48:50.627", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:48:50.652", "load", "debugloadchapter", ""},
    {"02:49:00.391", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:49:28.433", "write", "profiles/temporaryprofile/garage.pf", "scratch"},
    {"02:49:38.676", "write", "profiles/temporaryprofile/garage.pf", "scratch"},
    {"02:50:03.182", "write", "profiles/temporaryprofile/garage.pf", "scratch"},
    {"02:50:22.695", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:50:23.069", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:50:27.343", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:50:44.535", "end", "", ""},
    {"02:50:48.517", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:50:48.667", "write", "profiles/temporaryprofile/gameinputmodule.pf", "real"},
    {"02:50:48.677", "write", "profiles/temporaryprofile/aslot.pf", "scratch"},
    {"02:50:51.210", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:50:53.042", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
};

constexpr Event miles[] = {
    {"02:41:03.383", "guest", "", ""},
    {"02:41:16.902", "menu", "", ""},
    {"02:41:16.943", "load", "debugloadchapter", ""},
    {"02:41:18.489", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:41:18.527", "load", "debugloadchapter", ""},
    {"02:41:27.068", "write", "profiles/temporaryprofile/aslot.pf", "scratch"},
    {"02:41:48.709", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:41:53.059", "write", "profiles/temporaryprofile/aslot.pf", "scratch"},
    {"02:42:14.281", "write", "profiles/temporaryprofile/garage.pf", "scratch"},
    {"02:43:19.566", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:44:49.558", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:45:31.506", "end", "", ""},
    {"02:45:31.569", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:45:34.368", "write", "profiles/temporaryprofile/gameinputmodule.pf", "real"},
    {"02:45:34.375", "write", "profiles/temporaryprofile/aslot.pf", "scratch"},
    {"02:45:36.874", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:45:38.879", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:45:42.211", "load", "debugloadchapter", ""},
    {"02:45:43.779", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:45:43.958", "load", "debugloadchapter", ""},
    {"02:45:55.363", "write", "gamesaves/default/aslot/save1.sav", "scratch"},
    {"02:46:05.334", "host", "", ""},
    {"02:46:40.714", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:46:42.646", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:46:45.799", "menu", "", ""},
    {"02:48:43.534", "load", "debugloadchapter", ""},
    {"02:48:45.085", "write", "profiles/temporaryprofile/gamesettings.pf", "real"},
    {"02:48:45.127", "load", "debugloadchapter", ""},
    {"02:48:56.170", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:49:11.383", "write", "profiles/temporaryprofile/garage.pf", "held"},
    {"02:49:43.862", "write", "profiles/temporaryprofile/garage.pf", "held"},
    {"02:50:05.093", "write", "profiles/temporaryprofile/garage.pf", "held"},
    {"02:50:22.972", "write", "gamesaves/default/aslot/save1.sav", "real"},
    {"02:50:38.872", "write", "gamesaves/default/aslot/save1.sav", "real"},
};

}  // namespace coop_file_writes
