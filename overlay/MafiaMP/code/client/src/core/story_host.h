#pragma once

#include <string>

namespace MafiaMP::Core {
    // Story-host mode: this client runs the genuine single-player campaign (the game's own Lua
    // mission scripts stay alive, the vanilla main menu runs) while staying connected to a MafiaMP
    // server, and mirrors every non-network human and car of its game to that server so guests can
    // see them (see mirror/world_mirror.h).
    //
    // Read once at client start (StoryHost::Init) from either the environment variable
    // MAFIAMP_STORY_HOST=1 or a file story_host.txt next to MafiaMPLauncher.exe. Off by default, in
    // which case every free-ride path is untouched.
    class StoryHost {
      public:
        // Reads the flag and the optional chapter file. Call once, before any hook installs.
        static void Init();

        static bool IsEnabled() {
            return _enabled;
        }

        // Trimmed first line of story_chapter.txt next to the launcher (a chapter save path such
        // as "02_lost_heaven/lh_freeride_extreme.sav"), or empty when there is none.
        static const std::string &GetChapterSavePath() {
            return _chapterSavePath;
        }

        // Directory of the running launcher executable (where the flag files are looked up).
        static const std::string &GetLauncherDirectory() {
            return _launcherDirectory;
        }

        // Server the story host connects to automatically once the chapter is loaded. Defaults to
        // 127.0.0.1 27015 "Host"; story_server.txt next to the launcher overrides it with one line
        // "<host> <port> [nickname]".
        static const std::string &GetServerHost() {
            return _serverHost;
        }
        static int GetServerPort() {
            return _serverPort;
        }
        static const std::string &GetNickname() {
            return _nickname;
        }

        // Guest auto-connect: autoconnect.txt next to the launcher ("<host> <port> [nickname]") makes
        // ANY client (story host or not) connect to that server by itself instead of showing the
        // connect page. Empty host when the file is absent.
        static bool HasAutoConnect() {
            return !_autoHost.empty();
        }
        static const std::string &GetAutoHost() {
            return _autoHost;
        }
        static int GetAutoPort() {
            return _autoPort;
        }
        static const std::string &GetAutoNickname() {
            return _autoNickname;
        }

      private:
        static inline bool _enabled = false;
        static inline std::string _chapterSavePath;
        static inline std::string _launcherDirectory;
        static inline std::string _serverHost = "127.0.0.1";
        static inline int _serverPort         = 27015;
        static inline std::string _nickname   = "Host";
        static inline std::string _autoHost;
        static inline int _autoPort             = 27015;
        static inline std::string _autoNickname = "Player";
    };

    inline bool IsStoryHost() {
        return StoryHost::IsEnabled();
    }
} // namespace MafiaMP::Core
