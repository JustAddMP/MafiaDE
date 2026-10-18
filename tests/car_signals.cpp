// car_signals: unit tests for the car signals byte (src/game/car_signals.h): the game bits -> wire bits mapping and
// back, the valid bit, the end-of-vehicle-block wire field (a block without it reads as unknown; an older reader
// stops before it), and the log texts. Prints PASS/FAIL per check; exits non-zero on any failure. No game needed.
#include <cstdint>
#include <cstdio>
#include <string>

#include "game/car_signals.h"
#include "game/player_actions.h"
#include "net/wire.h"

using namespace mc::game;
namespace cs = mc::game::car_signals;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-60s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

void TestFromGame() {
    Check(cs::FromGame(0, 0, 0) == cs::kValid, "nothing on reads as valid + 0");
    Check(cs::FromGame(cs::kGamePark, 0, 0) == (cs::kValid | cs::kPark), "park lights");
    Check(cs::FromGame(cs::kGameHead, 0, 0) == (cs::kValid | cs::kHead), "headlights");
    Check(cs::FromGame(cs::kGameIndA, 0, 0) == (cs::kValid | cs::kIndA), "indicator A (game 0x08)");
    Check(cs::FromGame(cs::kGameIndB, 0, 0) == (cs::kValid | cs::kIndB), "indicator B (game 0x10)");
    Check(cs::FromGame(cs::kGameBeacon, 0, 0) == (cs::kValid | cs::kBeacon), "beacon (game 0x40)");
    Check(cs::FromGame(0, 1, 0) == (cs::kValid | cs::kHorn), "horn byte");
    Check(cs::FromGame(0, 0, cs::kGameSiren) == (cs::kValid | cs::kSiren), "siren bit 0x2000000");
    // bits the copy makes itself (brake 0x04, reverse 0x20), the searchlight (0x400), the script's auto flag
    // (0x1000000) and the horn's / other flags in the siren word must not leak into the byte
    Check(cs::FromGame(0x04 | 0x20 | 0x80 | 0x100 | 0x400 | 0xFF000000u, 0, 0x800000 | 0x4000000) == cs::kValid,
          "brake, reverse, searchlight, high byte and other flags ignored");
    const uint8_t all = cs::FromGame(0x7F, 1, 0xFFFFFFFFu);
    Check(all == (cs::kValid | cs::kAll), "everything on", std::to_string(all));
}

void TestGameLights() {
    bool ok = true;
    for (int s = 0; s < 256; ++s) {
        const uint8_t b = static_cast<uint8_t>(s | cs::kValid);
        const uint32_t l = cs::GameLights(b);
        // round trip through the game bits keeps every light bit (horn and siren live elsewhere)
        const uint8_t back = cs::FromGame(l, (b & cs::kHorn) ? 1 : 0, (b & cs::kSiren) ? cs::kGameSiren : 0);
        ok &= back == b;
        ok &= (l & ~static_cast<uint32_t>(0x5B)) == 0;
    }
    Check(ok, "every byte round-trips through the game bits");
    Check(cs::GameLights(cs::kValid | cs::kPark | cs::kHead) == 0x03, "park + head = game 0x03");
    Check(cs::GameLights(cs::kValid | cs::kIndA | cs::kIndB) == 0x18, "hazard = game 0x18");
}

void TestSanitize() {
    Check(cs::Sanitize(cs::kHorn | cs::kHead) == 0, "bits without the valid bit are dropped");
    Check(cs::Sanitize(cs::kValid | cs::kHorn) == (cs::kValid | cs::kHorn), "valid byte kept");
    Check(cs::Sanitize(0) == 0, "zero stays unknown");
}

// a PlayerState-like vehicle block tail: ... gear, [signals]
void TestWire() {
    for (int s = 0; s < 256; ++s) {
        mc::net::ByteWriter w;
        w.U8(0x7E);  // the gear before it
        cs::Write(w, static_cast<uint8_t>(s));
        mc::net::ByteReader r(w.Span());
        const uint8_t gear = r.U8();
        const uint8_t got = cs::Read(r);
        if (gear != 0x7E || got != cs::Sanitize(static_cast<uint8_t>(s)) || !r.Ok() || !r.AtEnd()) {
            Check(false, "wire round trip", "byte " + std::to_string(s) + " read " + std::to_string(got));
            return;
        }
    }
    Check(true, "wire round trip of every byte (invalid ones as 0)");
    {
        mc::net::ByteWriter w;  // a game without the field: the block ends at the gear
        w.U8(3);
        mc::net::ByteReader r(w.Span());
        r.U8();
        const uint8_t got = cs::Read(r);
        Check(got == 0 && r.Ok(), "a block without the byte reads as unknown, reader still ok");
    }
    {
        mc::net::ByteWriter w;  // an older reader stops at the gear and never sees the extra byte
        w.U8(3);
        cs::Write(w, cs::kValid | cs::kSiren);
        mc::net::ByteReader r(w.Span());
        Check(r.U8() == 3 && r.Ok() && r.Remaining() == 1, "older reader: the byte is trailing, nothing breaks");
    }
    {
        mc::net::ByteWriter w;
        cs::Write(w, cs::kHorn);  // no valid bit: sent as 0
        Check(w.Size() == 1 && w.Data()[0] == 0, "an invalid byte goes out as 0");
    }
}

// The tail of a PlayerState in a car as player_sync.cpp writes and reads it: ... gear, signals, then the action block
// (player_actions.h) that ends every state. Both optional parts are read "if anything is left", so this build must
// always write both: then each comes back as written.
void TestRealTail() {
    namespace act = mc::game::actions;
    for (int sig : {0, int(cs::kValid), int(cs::kValid | cs::kHead | cs::kHorn), 0xFF}) {
        for (int seq : {0, 1, 200}) {
            mc::net::ByteWriter w;
            w.U8(0xFE);  // gear -2
            cs::Write(w, static_cast<uint8_t>(sig));
            act::Write(w, {static_cast<uint8_t>(seq), act::Action::Takedown});
            mc::net::ByteReader r(w.Span());
            const uint8_t gear = r.U8();
            const uint8_t gotSig = cs::Read(r);
            act::State got;
            act::Read(r, got);
            const bool ok = gear == 0xFE && gotSig == cs::Sanitize(static_cast<uint8_t>(sig)) && got.seq == seq &&
                            got.last == act::Action::Takedown && r.Ok() && r.AtEnd();
            if (!ok) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "sig %d seq %d -> sig %u seq %u kind %u", sig, seq, gotSig, got.seq,
                              static_cast<unsigned>(got.last));
                Check(false, "real tail: gear, signals, action block", buf);
                return;
            }
        }
    }
    Check(true, "real tail: gear, signals, action block");
    {
        mc::net::ByteWriter w;  // on foot: the action block follows the aim directly (no vehicle block)
        act::Write(w, {7, act::Action::Throw});
        mc::net::ByteReader r(w.Span());
        act::State got;
        act::Read(r, got);
        Check(got.seq == 7 && got.last == act::Action::Throw && r.Ok() && r.AtEnd(), "on foot: action block alone");
    }
    {
        mc::net::ByteWriter w;  // a car state that ends at the gear (neither part): unknown signals, no action
        w.U8(1);
        mc::net::ByteReader r(w.Span());
        r.U8();
        const uint8_t gotSig = cs::Read(r);
        act::State got;
        act::Read(r, got);
        Check(gotSig == 0 && got.seq == 0 && got.last == act::Action::None && r.Ok(), "neither part: unknown, none");
    }
}

void TestTexts() {
    Check(cs::Describe(0) == "unknown", "describe unknown", cs::Describe(0));
    Check(cs::Describe(cs::kValid) == "off", "describe off", cs::Describe(cs::kValid));
    const std::string d = cs::Describe(cs::kValid | cs::kHead | cs::kPark | cs::kIndA | cs::kIndB | cs::kHorn |
                                       cs::kSiren | cs::kBeacon);
    Check(d == "lights=park+head ind=hazard horn siren beacon", "describe everything", d);
    Check(cs::Describe(cs::kValid | cs::kHead | cs::kIndB) == "lights=head ind=B", "describe head + B",
          cs::Describe(cs::kValid | cs::kHead | cs::kIndB));
    const std::string c = cs::Changes(cs::kValid | cs::kHead, cs::kValid | cs::kHorn);
    Check(c == "head off, horn on", "changes", c);
    Check(cs::Changes(cs::kValid, cs::kValid).empty(), "no change, no text");
    Check(cs::Changes(0, cs::kValid | cs::kHorn) == "valid", "becoming valid");
    Check(cs::Changes(cs::kValid | cs::kHorn, 0) == "lost", "losing validity");
}

}  // namespace

int main() {
    TestFromGame();
    TestGameLights();
    TestSanitize();
    TestWire();
    TestRealTail();
    TestTexts();
    std::printf("%s (%d failure(s))\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
