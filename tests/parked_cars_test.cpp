// parked_cars_test: unit tests for src/game/parked_cars.h (ParkedCars 0x0104, docs/protocol.md "ParkedCars"): the codec
// round trip, messages split at kPerMessage cars, malformed messages refused as a whole, the host's Book (a newer
// CarParked replaces the older, a car driven again leaves it, key order, the size cap). Prints PASS/FAIL per check;
// exits non-zero on any failure. Built by build.cmd (out\parked_cars_test.exe), run by tools\make_release.cmd.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "game/parked_cars.h"

using namespace mc::game::parked;
using mc::net::ByteReader;
using mc::net::ByteWriter;

namespace {

int g_failures = 0;

void Check(bool ok, const char* name, const std::string& detail = {}) {
    std::printf("%s  %-70s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    std::fflush(stdout);
    if (!ok) ++g_failures;
}

Car Make(uint8_t owner, uint32_t guid, const char* model, float x, uint8_t flags = 0) {
    Car c;
    c.owner = owner;
    c.guid = guid;
    c.model = model;
    c.t[0] = x;
    c.t[1] = -300.25f;
    c.t[2] = 2.5f;
    c.t[3] = 0.1f;
    c.t[4] = -0.2f;
    c.t[5] = 0.3f;
    c.t[6] = 0.927f;
    c.flags = flags;
    return c;
}

bool RoundTrip(const std::vector<Car>& in, std::vector<Car>& out, size_t* bytes = nullptr) {
    ByteWriter w(256);
    Write(w, in);
    if (bytes) *bytes = w.Size();
    ByteReader r(w.Span());
    return Read(r, out);
}

}  // namespace

int main() {
    {  // round trip, flags kept
        const std::vector<Car> in = {Make(0, 1991178, "bolt_ace", -902.5f),
                                     Make(1, 4746250, "samson_drifter", 12, kWrecked)};
        std::vector<Car> out;
        size_t bytes = 0;
        const bool ok = RoundTrip(in, out, &bytes);
        Check(ok && out.size() == 2 && out[0] == in[0] && out[1] == in[1], "two cars: read back exactly (flags too)",
              std::to_string(bytes) + " bytes");
        Check(bytes == 2 + (1 + 4 + 1 + 8 + 28 + 1) + (1 + 4 + 1 + 14 + 28 + 1),
              "layout: version, count, per car owner guid model transform flags");
    }
    {  // empty message
        std::vector<Car> out = {Make(0, 1, "x", 0)};
        Check(RoundTrip({}, out) && out.empty(), "no cars: an empty list");
    }
    {  // split
        std::vector<Car> all;
        for (uint32_t i = 0; i < 60; ++i) all.push_back(Make(0, i, "bolt_ace", static_cast<float>(i)));
        const auto parts = Split(all);
        Check(parts.size() == 3 && parts[0].size() == kPerMessage && parts[2].size() == 60 - 2 * kPerMessage,
              "60 cars: 3 messages of at most 24");
        std::vector<Car> back;
        bool ok = true;
        for (const auto& p : parts) {
            std::vector<Car> o;
            ok = ok && RoundTrip(p, o);
            back.insert(back.end(), o.begin(), o.end());
        }
        Check(ok && back == all, "... all 60 read back in order");
        Check(Split({}).empty(), "no parked cars: no message");
        // Write never writes more than kPerMessage
        std::vector<Car> o;
        Check(RoundTrip(all, o) && o.size() == kPerMessage, "Write caps a message at 24 cars");
    }
    {  // malformed
        std::vector<Car> out;
        ByteWriter w(64);
        Write(w, {Make(0, 7, "bolt_ace", 1)});
        std::vector<uint8_t> bytes(w.Span().begin(), w.Span().end());
        for (size_t cut = 0; cut < bytes.size(); ++cut) {
            ByteReader r(bytes.data(), cut);
            if (Read(r, out)) {
                Check(false, "truncated message refused", "accepted at " + std::to_string(cut) + " bytes");
                break;
            }
        }
        Check(out.empty(), "every truncation refused, nothing used");
        std::vector<uint8_t> v2 = bytes;
        v2[0] = 2;
        ByteReader r2(v2.data(), v2.size());
        Check(!Read(r2, out), "another version refused");
        Car bad = Make(0, 8, "bolt ace\nH 1", 1);  // would split a Lua command line
        Check(!Valid(bad), "a model with spaces / control characters is not valid");
        Car nan = Make(0, 9, "bolt_ace", std::numeric_limits<float>::quiet_NaN());
        Check(!Valid(nan), "a NaN position is not valid");
        Car far = Make(0, 9, "bolt_ace", 2e6f);
        Check(!Valid(far), "a position beyond 1e6 m is not valid");
        // a valid first car and an invalid second one: the whole message is refused
        ByteWriter w3(128);
        w3.U8(kVersion);
        w3.U8(2);
        for (const Car& c : {Make(0, 1, "bolt_ace", 1), Make(0, 2, "a b", 1)}) {
            w3.U8(c.owner);
            w3.U32(c.guid);
            w3.Str(c.model);
            for (float f : c.t) w3.F32(f);
            w3.U8(c.flags);
        }
        ByteReader r3(w3.Span());
        Check(!Read(r3, out) && out.empty(), "one bad car: the whole message refused");
        // a count beyond kPerMessage
        ByteWriter w4(8);
        w4.U8(kVersion);
        w4.U8(200);
        ByteReader r4(w4.Span());
        Check(!Read(r4, out), "a count beyond 24 refused");
    }
    {  // the book
        Book b;
        b.Parked(Make(1, 50, "samson_drifter", 10));
        b.Parked(Make(0, 20, "bolt_ace", 5));
        b.Parked(Make(0, 20, "bolt_ace", 7, kWrecked));  // parked again elsewhere, destroyed
        auto s = b.Snapshot();
        Check(s.size() == 2 && s[0].owner == 0 && s[0].guid == 20 && s[1].owner == 1,
              "two cars, in key order (owner, guid)");
        Check(s[0].t[0] == 7 && s[0].flags == kWrecked, "a newer CarParked replaces the older (where, destroyed)");
        Check(b.Driven(0, 20) && b.Size() == 1 && !b.Driven(0, 20), "driven again: no longer parked (once)");
        b.Parked(Make(2, 1, "bad model", 1));
        Check(b.Size() == 1, "an invalid car is not booked");
        Book cap;
        for (uint32_t i = 0; i < Book::kMax + 10; ++i) cap.Parked(Make(3, i, "bolt_ace", 1));
        Check(cap.Size() == Book::kMax && cap.Snapshot().front().guid == 10, "capped at 512 cars (lowest keys go)");
    }
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
