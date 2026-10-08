#include <utils/safe_win32.h>

#include "story_host.h"

#include "application.h"

#include <logging/logger.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace MafiaMP::Core {
    namespace {
        std::string Trim(std::string s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
                s.pop_back();
            }
            size_t start = 0;
            while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
                ++start;
            }
            return s.substr(start);
        }

        // First non-empty line of a file, trimmed; empty when the file is missing.
        std::string ReadFirstLine(const std::string &path) {
            std::ifstream in(path);
            if (!in.is_open()) {
                return {};
            }
            std::string line;
            while (std::getline(in, line)) {
                line = Trim(line);
                if (!line.empty()) {
                    return line;
                }
            }
            return {};
        }

        bool FileExists(const std::string &path) {
            const DWORD attrs = GetFileAttributesA(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
        }

        std::string ExecutableDirectory() {
            char buffer[MAX_PATH * 4] = {};
            const DWORD len           = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
            if (len == 0 || len >= sizeof(buffer)) {
                return {};
            }
            std::string path(buffer, len);
            const auto slash = path.find_last_of("\\/");
            return slash == std::string::npos ? std::string() : path.substr(0, slash);
        }
    } // namespace

    void StoryHost::Init() {
        _launcherDirectory = ExecutableDirectory();
        if (_launcherDirectory.empty()) {
            _launcherDirectory = gProjectPath;
        }

        bool fromEnv = false;
        {
            char value[16] = {};
            const DWORD len = GetEnvironmentVariableA("MAFIAMP_STORY_HOST", value, sizeof(value));
            fromEnv         = len > 0 && len < sizeof(value) && Trim(value) == "1";
        }
        const std::string flagFile = _launcherDirectory + "\\story_host.txt";
        const bool fromFile        = FileExists(flagFile);

        // Guest auto-connect (any client). One line: <host> <port> [nickname]
        {
            const std::string line = ReadFirstLine(_launcherDirectory + "\\autoconnect.txt");
            if (!line.empty() && line[0] != '#') {
                std::string host, nick;
                int port       = 0;
                const size_t a = line.find(' ');
                host           = a == std::string::npos ? line : line.substr(0, a);
                if (a != std::string::npos) {
                    const std::string rest = Trim(line.substr(a + 1));
                    const size_t b         = rest.find(' ');
                    port                   = std::atoi((b == std::string::npos ? rest : rest.substr(0, b)).c_str());
                    if (b != std::string::npos) {
                        nick = Trim(rest.substr(b + 1));
                    }
                }
                if (!host.empty()) {
                    _autoHost = host;
                    if (port > 0 && port < 65536) {
                        _autoPort = port;
                    }
                    if (!nick.empty()) {
                        _autoNickname = nick;
                    }
                    Framework::Logging::GetLogger("Story")->info("[AutoConnect] autoconnect.txt: will connect to {}:{} as \"{}\"", _autoHost, _autoPort, _autoNickname);
                }
            }
        }

        _enabled = fromEnv || fromFile;
        if (!_enabled) {
            return;
        }

        const std::string chapterFile = _launcherDirectory + "\\story_chapter.txt";
        _chapterSavePath              = ReadFirstLine(chapterFile);

        {
            const std::string serverFile = _launcherDirectory + "\\story_server.txt";
            const std::string line       = ReadFirstLine(serverFile);
            if (!line.empty()) {
                std::string host, nick;
                int port       = 0;
                const size_t a = line.find(' ');
                host           = a == std::string::npos ? line : line.substr(0, a);
                if (a != std::string::npos) {
                    const std::string rest = Trim(line.substr(a + 1));
                    const size_t b         = rest.find(' ');
                    port                   = std::atoi((b == std::string::npos ? rest : rest.substr(0, b)).c_str());
                    if (b != std::string::npos) {
                        nick = Trim(rest.substr(b + 1));
                    }
                }
                if (!host.empty()) {
                    _serverHost = host;
                }
                if (port > 0 && port < 65536) {
                    _serverPort = port;
                }
                if (!nick.empty()) {
                    _nickname = nick;
                }
            }
        }

        auto logger = Framework::Logging::GetLogger("Story");
        logger->info("[Story] Auto-connect target {}:{} as \"{}\" (story_server.txt overrides)", _serverHost, _serverPort, _nickname);
        logger->info("[Story] Story-host mode ENABLED ({}{}{})", fromEnv ? "MAFIAMP_STORY_HOST=1" : "", fromEnv && fromFile ? ", " : "", fromFile ? flagFile : "");
        if (_chapterSavePath.empty()) {
            logger->info("[Story] No {} (or empty): the vanilla main menu runs, start a chapter from it", chapterFile);
        }
        else {
            logger->info("[Story] Chapter save to load from {}: \"{}\"", chapterFile, _chapterSavePath);
        }
    }
} // namespace MafiaMP::Core
