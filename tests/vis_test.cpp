// vis_test: the VIS record (src/game/vis.h). The per-kind rate cap (60 a second, the rest counted and reported once
// the next second starts, kinds apart), and real records in a trace: the format sync_compare reads, the extra fields,
// a burst cut to the cap with one "ev=dropped" record, and every event (also the dropped ones) counted in core/diag.h.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/diag.h"
#include "core/log.h"
#include "core/trace.h"
#include "game/vis.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-62s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

int Count(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

}  // namespace

int main() {
    using mc::game::vis::Kind;
    mc::game::vis::RateCap cap;
    uint32_t rep = 0;
    int ok = 0;
    for (int i = 0; i < 70; ++i) ok += cap.Allow(Kind::Traffic, 10.0 + i * 0.001, &rep) ? 1 : 0;
    Check(ok == mc::game::vis::kMaxPerSecond, "cap: 60 a second", std::to_string(ok));
    Check(cap.Allow(Kind::Ped, 10.5, &rep) && rep == 0, "cap: another kind has its own count");
    Check(cap.Allow(Kind::Traffic, 11.2, &rep) && rep == 10, "cap: the next second reports the 10 dropped");
    Check(cap.Allow(Kind::Traffic, 11.3, &rep) && rep == 0, "cap: ... once");

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"mc-vis-test";
    CreateDirectoryW(dir.c_str(), nullptr);
    mc::core::log::Init(dir, L"v.log");
    mc::core::diag::Reset();
    mc::core::trace::Open("role=test", "v");
    const std::wstring path = mc::core::trace::Path();
    mc::game::vis::Event(Kind::Traffic, 42, "admit", "tx", 1, "p=%.1f,%.1f,%.1f", 1.0, 2.0, 3.0);
    mc::game::vis::Event(Kind::Ped, (0u << 16) | 7u, "show", "rx", 0, "local=%u", 12345u);
    mc::game::vis::Event(Kind::Npc, 99, "leave", "rx", 0);
    mc::game::vis::Event(Kind::Player, 1, "spawn", "rx", 1, "%s", "after=0.5 world=3.0");
    mc::game::vis::Event(Kind::Held, 555, "hold", "-", -1, "host=0 why=unpaired");
    mc::game::vis::Event(Kind::Npc, 1, "bad ev", "rx", 0);
    for (int i = 0; i < 75; ++i) mc::game::vis::Event(Kind::Traffic, 1000 + i, "hide", "rx", 0);
    Sleep(1100);
    mc::game::vis::Event(Kind::Traffic, 5000, "show", "rx", 0);
    mc::core::trace::Close();
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    Check(text.find(" VIS kind=traffic key=42 ev=admit dir=tx peer=1 p=1.0,2.0,3.0\r\n") != std::string::npos,
          "trace: a host admit with its position");
    Check(text.find(" VIS kind=ped key=7 ev=show dir=rx peer=0 local=12345\r\n") != std::string::npos,
          "trace: a pedestrian shown here");
    Check(text.find(" VIS kind=npc key=99 ev=leave dir=rx peer=0\r\n") != std::string::npos, "trace: no extra fields");
    Check(text.find(" VIS kind=player key=1 ev=spawn dir=rx peer=1 after=0.5 world=3.0\r\n") != std::string::npos,
          "trace: a puppet spawned (the Lua's extra text)");
    Check(text.find(" VIS kind=held key=555 ev=hold dir=- peer=- host=0 why=unpaired\r\n") != std::string::npos,
          "trace: a held NPC, no peer");
    Check(text.find("key=1 ev=? dir=rx") != std::string::npos, "trace: a bad event name becomes ?");
    // the cap: 60 traffic records in that second (the admit counts too), then "dropped"
    const int hides = Count(text, " ev=hide ");
    Check(hides == mc::game::vis::kMaxPerSecond - 1, "trace: a burst is cut to the cap", std::to_string(hides));
    Check(text.find(" VIS kind=traffic key=0 ev=dropped dir=- peer=- n=16\r\n") != std::string::npos &&
              text.find("key=5000 ev=show") != std::string::npos,
          "trace: one dropped record with the count, then records again");
    uint64_t hideCount = 0, admitCount = 0;
    for (const auto& v : mc::core::diag::Snapshot()) {
        if (v.name == "vis.traffic.hide") hideCount = v.sum;
        if (v.name == "vis.traffic.admit") admitCount = v.sum;
    }
    Check(hideCount == 75 && admitCount == 1, "diag: every event counted, dropped ones too",
          std::to_string(hideCount));
    mc::core::log::Shutdown();
    DeleteFileW(path.c_str());
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
