#include "game/car_look.h"

#include "net/wire.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mc::game::car_look {

using car_damage::CarRef;

bool Look::operator==(const Look& o) const {
    return model == o.model && color[0] == o.color[0] && color[1] == o.color[1] && plate == o.plate &&
           tint == o.tint && interior == o.interior && dirt == o.dirt && rust == o.rust && painting == o.painting &&
           tuningKnown == o.tuningKnown && tuning == o.tuning;
}

namespace {

std::string Ids(const std::vector<uint16_t>& ids, size_t max = 64) {
    std::string s;
    for (size_t i = 0; i < ids.size() && i < max; ++i) {
        if (i) s += ',';
        s += std::to_string(ids[i]);
    }
    return s;
}

bool PlainChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == ':' || c == '-';
}

// what the wire carries of a string: printable ASCII only, at most max bytes
std::string Clean(const std::string& s, size_t max) {
    std::string out;
    for (char c : s) {
        if (out.size() >= max) break;
        if (c >= 0x20 && c < 0x7F) out += c;
    }
    return out;
}

void WriteRef(net::ByteWriter& w, const CarRef& r) {
    if (r.kind == CarRef::kTraffic) {
        w.U8(1);
        w.U16(static_cast<uint16_t>(r.id));
    } else {
        w.U8(0);
        w.U8(r.owner);
        w.U32(r.id);
    }
}

bool ReadRef(net::ByteReader& r, CarRef& out) {
    const uint8_t kind = r.U8();
    if (kind == 0) {
        out.kind = CarRef::kPlayer;
        out.owner = r.U8();
        out.id = r.U32();
    } else if (kind == 1) {
        out.kind = CarRef::kTraffic;
        out.owner = 0;
        out.id = r.U16();
    } else {
        return false;
    }
    return !r.Failed() && out.id != 0;
}

}  // namespace

std::string Look::Describe() const {
    char buf[96];
    std::string s = model.empty() ? "?" : model;
    std::snprintf(buf, sizeof buf, " colours %d/%d", color[0], color[1]);
    s += buf;
    s += " plate " + (plate.empty() ? std::string("?") : "'" + plate + "'");
    if (tint >= 0) s += " tint " + std::to_string(tint);
    if (interior >= 0) s += " interior " + std::to_string(interior);
    if (dirt != kUnknown8) s += " dirt " + std::to_string(dirt);
    if (rust != kUnknown8) s += " rust " + std::to_string(rust);
    if (!painting.empty()) s += " painting " + painting;
    s += " tuning " + (tuningKnown ? (tuning.empty() ? std::string("none") : Ids(tuning, 16)) : std::string("?"));
    return s;
}

std::vector<uint8_t> Write(const Message& m) {
    net::ByteWriter w(64 + m.look.tuning.size() * 2);
    w.U8(m.op);
    WriteRef(w, m.ref);
    if (m.op != Message::kLook) return w.Take();
    const Look& l = m.look;
    w.U16(m.seq);
    w.Str(Clean(l.model, kMaxModel));
    w.I16(l.color[0]);
    w.I16(l.color[1]);
    w.Str(Clean(l.plate, kMaxPlate));
    w.I16(l.tint);
    w.I16(l.interior);
    w.U8(l.dirt);
    w.U8(l.rust);
    w.Str(Clean(l.painting, kMaxPainting));
    w.U8(l.tuningKnown ? 1 : 0);
    const size_t n = l.tuningKnown ? std::min(l.tuning.size(), kMaxTuning) : 0;
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) w.U16(l.tuning[i]);
    return w.Take();
}

bool Read(const uint8_t* data, size_t len, Message& m) {
    net::ByteReader r(data, len);
    m = Message{};
    m.op = r.U8();
    if (r.Failed() || m.op > Message::kRequest) return false;
    if (!ReadRef(r, m.ref)) return false;
    if (m.op == Message::kRequest) return !r.Failed();
    Look& l = m.look;
    m.seq = r.U16();
    l.model = r.Str(kMaxModel);
    l.color[0] = r.I16();
    l.color[1] = r.I16();
    l.plate = r.Str(kMaxPlate);
    l.tint = r.I16();
    l.interior = r.I16();
    l.dirt = r.U8();
    l.rust = r.U8();
    l.painting = r.Str(kMaxPainting);
    const uint8_t flags = r.U8();
    const uint8_t n = r.U8();
    if (r.Failed() || n > kMaxTuning) return false;
    l.tuningKnown = (flags & 1) != 0;
    if (!l.tuningKnown && n) return false;
    l.tuning.resize(n);
    for (uint8_t i = 0; i < n; ++i) l.tuning[i] = r.U16();
    if (r.Failed()) return false;
    // what a line can carry: no control characters (Clean on the sender; a bad sender's look is dropped)
    for (const std::string* s : {&l.model, &l.plate, &l.painting})
        for (char c : *s)
            if (c < 0x20 || c >= 0x7F) return false;
    if (l.model.empty()) return false;
    if (l.color[0] < -1 || l.color[1] < -1 || l.tint < -1 || l.interior < -1) return false;
    if ((l.dirt > 250 && l.dirt != kUnknown8) || (l.rust > 250 && l.rust != kUnknown8)) return false;
    return true;
}

std::string Escape(const std::string& s) {
    if (s.empty()) return "-";
    std::string out;
    char buf[4];
    for (char c : s) {
        if (PlainChar(c) && !(c == '-' && s.size() == 1)) {
            out += c;
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", static_cast<unsigned char>(c));
            out += buf;
        }
    }
    return out;
}

std::string Unescape(const std::string& s) {
    if (s == "-") return "";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const char hex[3] = {s[i + 1], s[i + 2], 0};
            char* end = nullptr;
            const long v = std::strtol(hex, &end, 16);
            if (end == hex + 2) {
                out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::string ToLine(const Look& l) {
    std::string t;
    if (!l.tuningKnown)
        t = "-";
    else if (l.tuning.empty())
        t = ".";
    else
        t = Ids(l.tuning, kMaxTuning);
    char buf[64];
    std::snprintf(buf, sizeof buf, " %d %d ", l.color[0], l.color[1]);
    std::string s = Escape(l.model) + buf + Escape(l.plate);
    std::snprintf(buf, sizeof buf, " %d %d %d %d ", l.tint, l.interior, l.dirt == kUnknown8 ? -1 : l.dirt,
                  l.rust == kUnknown8 ? -1 : l.rust);
    s += buf + Escape(l.painting) + " " + t;
    return s;
}

bool FromLine(const std::string& line, Look& l) {
    std::istringstream in(line);
    std::string f[10];
    for (auto& x : f)
        if (!(in >> x)) return false;
    l = Look{};
    auto num = [](const std::string& s, long lo, long hi, long& v) {
        char* end = nullptr;
        v = std::strtol(s.c_str(), &end, 10);
        return end && *end == 0 && end != s.c_str() && v >= lo && v <= hi;
    };
    l.model = Clean(Unescape(f[0]), kMaxModel);
    if (l.model.empty()) return false;
    long v[6];
    if (!num(f[1], -1, 32767, v[0]) || !num(f[2], -1, 32767, v[1]) || !num(f[4], -1, 32767, v[2]) ||
        !num(f[5], -1, 32767, v[3]) || !num(f[6], -1, 250, v[4]) || !num(f[7], -1, 250, v[5]))
        return false;
    l.color[0] = static_cast<int16_t>(v[0]);
    l.color[1] = static_cast<int16_t>(v[1]);
    l.plate = Clean(Unescape(f[3]), kMaxPlate);
    l.tint = static_cast<int16_t>(v[2]);
    l.interior = static_cast<int16_t>(v[3]);
    l.dirt = v[4] < 0 ? kUnknown8 : static_cast<uint8_t>(v[4]);
    l.rust = v[5] < 0 ? kUnknown8 : static_cast<uint8_t>(v[5]);
    l.painting = Clean(Unescape(f[8]), kMaxPainting);
    if (f[9] == "-") {
        l.tuningKnown = false;
    } else {
        l.tuningKnown = true;
        if (f[9] != ".") {
            std::istringstream ids(f[9]);
            std::string id;
            while (std::getline(ids, id, ',')) {
                long x = 0;
                if (!num(id, 0, 65535, x)) return false;
                if (l.tuning.size() < kMaxTuning) l.tuning.push_back(static_cast<uint16_t>(x));
            }
        }
    }
    return true;
}

Change Diff(const Look& have, const Look& want) {
    Change c;
    c.color = want.color[0] >= 0 && want.color[1] >= 0 &&
              (want.color[0] != have.color[0] || want.color[1] != have.color[1]);
    c.plate = !want.plate.empty() && want.plate != have.plate;
    c.tint = want.tint >= 0 && want.tint != have.tint;
    c.interior = want.interior >= 0 && want.interior != have.interior;
    c.dirt = want.dirt != kUnknown8 && want.dirt != have.dirt;
    c.rust = want.rust != kUnknown8 && want.rust != have.rust;
    c.painting = !want.painting.empty() && want.painting != have.painting;
    if (want.tuningKnown) {
        for (uint16_t id : want.tuning)
            if (!have.tuningKnown || std::find(have.tuning.begin(), have.tuning.end(), id) == have.tuning.end())
                c.tuningIds.push_back(id);
        c.tuning = !c.tuningIds.empty();
    }
    return c;
}

std::string Change::Describe() const {
    std::string s;
    auto add = [&s](bool on, const char* what) {
        if (!on) return;
        if (!s.empty()) s += ", ";
        s += what;
    };
    add(color, "colours");
    add(plate, "plate");
    add(tint, "window tint");
    add(interior, "interior");
    add(dirt, "dirt");
    add(rust, "rust");
    add(painting, "painting");
    if (tuning) {
        if (!s.empty()) s += ", ";
        s += "tuning " + std::to_string(tuningIds.size()) + " item(s) " + Ids(tuningIds, 16);
    }
    return s.empty() ? "nothing" : s;
}

// ---- Book ---------------------------------------------------------------------------------------------------------------
namespace {
// a look in one trace field: the line form with '|' for its spaces
std::string NoSpace(std::string s) {
    std::replace(s.begin(), s.end(), ' ', '|');
    return s;
}
std::string TraceLook(const Look& l) {
    std::string s = ToLine(l);
    std::replace(s.begin(), s.end(), ' ', '|');
    return s;
}
}  // namespace

void Book::OnLook(const Message& m, int from, double now) {
    if (m.op != Message::kLook) return;
    Car& c = cars_[m.ref];
    if (c.seen == 0) c.seen = now;
    ++stats_.received;
    const bool newer = static_cast<int16_t>(m.seq - c.wantSeq) > 0;
    if (c.haveWant && from == c.wantFrom && !newer) return;  // an older or the same look of the same sender
    c.wantSeq = m.seq;
    c.wantFrom = from;
    if (c.haveWant && c.want == m.look) return;
    c.haveWant = true;
    c.want = m.look;
    c.tries = 0;
    c.gaveUp = c.matchLogged = c.modelWarned = false;
}

void Book::OnRequest(const CarRef& ref, int from, double now) {
    Car& c = cars_[ref];
    c.askAt = now;
    if (std::find(c.askers.begin(), c.askers.end(), from) == c.askers.end()) c.askers.push_back(from);
}

void Book::OnPlayerLeft(int id) {
    for (auto& [ref, c] : cars_) c.askers.erase(std::remove(c.askers.begin(), c.askers.end(), id), c.askers.end());
}

std::vector<CarRef> Book::Wanted() const {
    std::vector<CarRef> out;
    for (const auto& [ref, c] : cars_)
        if (!c.askers.empty()) out.push_back(ref);
    return out;
}

void Book::Round(const std::vector<CarInput>& cars, double now, Outbox& out) {
    stats_.cars = stats_.sources = stats_.copies = stats_.matching = stats_.waiting = stats_.failed = 0;
    stats_.deferred = 0;
    char buf[512];
    // sources first: a copy of our own car on our own game (our car hidden behind it) takes what we send
    for (int pass = 0; pass < 2; ++pass) {
        for (const CarInput& in : cars) {
            if ((pass == 0) != (in.role == Role::Source)) continue;
            Car& c = cars_[in.ref];
            c.seen = now;
            ++stats_.cars;
            const std::string ref = in.ref.Text();
            if (in.role == Role::Source) {
                ++stats_.sources;
                if (!in.read) continue;
                const bool changed = !c.haveSent || in.have != c.sent;
                if (changed && now - c.sentAt >= cfg_.minSendGap) {
                    const bool first = !c.haveSent;
                    ++c.seq;
                    c.sent = in.have;
                    c.haveSent = true;
                    c.sentAt = now;
                    Message m;
                    m.ref = in.ref;
                    m.seq = c.seq;
                    m.look = c.sent;
                    // a player car's look goes to everyone; the host's traffic only to whoever asks (below)
                    if (in.ref.kind == CarRef::kPlayer) {
                        out.broadcast.push_back(m);
                        ++stats_.sent;
                    }
                    std::snprintf(buf, sizeof buf, "car %s: %s from our game (#%u): %s", ref.c_str(),
                                  first ? "its look" : "its look changed", c.seq, c.sent.Describe().c_str());
                    if (in.ref.kind == CarRef::kPlayer) out.logs.emplace_back(buf);
                    out.trace.push_back("CLS ref=" + ref + " ent=" + std::to_string(in.entity) + " seq=" +
                                        std::to_string(c.seq) + " look=" + TraceLook(c.sent));
                }
                for (int peer : c.askers) {
                    Message m;
                    m.ref = in.ref;
                    m.seq = c.seq;
                    m.look = c.haveSent ? c.sent : in.have;
                    out.to.emplace_back(peer, m);
                    ++stats_.answers;
                    out.trace.push_back("CLS ref=" + ref + " ent=" + std::to_string(in.entity) + " to=" +
                                        std::to_string(peer) + " seq=" + std::to_string(c.seq) + " answer=1");
                }
                c.askers.clear();
                continue;
            }
            ++stats_.copies;
            if (c.entity != in.entity) {
                c.entity = in.entity;
                c.tries = 0;
                c.gaveUp = c.matchLogged = c.modelWarned = false;
                c.triedAt = -1e9;
                c.boundAt = now;
                c.deferred = false;
            }
            // a new copy of a player car: no puppet is seated in it before its look is on (a later tuning would
            // rebuild the car under him)
            const bool young = in.ref.kind == CarRef::kPlayer && !in.busy && now - c.boundAt < cfg_.holdMax;
            const Look* want = c.haveSent ? &c.sent : c.haveWant ? &c.want : nullptr;
            if (!want) {
                ++stats_.waiting;
                if (young && in.owner >= 0) out.holds.push_back(in.entity);
                if (in.owner >= 0 && now - c.requestAt >= cfg_.requestEvery) {
                    c.requestAt = now;
                    Message m;
                    m.op = Message::kRequest;
                    m.ref = in.ref;
                    out.requests.emplace_back(in.owner, m);
                    ++stats_.requests;
                    out.trace.push_back("CLQ ref=" + ref + " ent=" + std::to_string(in.entity) + " to=" +
                                        std::to_string(in.owner));
                }
                continue;
            }
            if (!in.read) continue;
            if (want->model != in.have.model && !c.modelWarned) {
                c.modelWarned = true;
                std::snprintf(buf, sizeof buf, "car %s: our copy is a %s, the car a %s: its look is set as far as it goes",
                              ref.c_str(), in.have.model.c_str(), want->model.c_str());
                out.logs.emplace_back(buf);
            }
            Change ch = Diff(in.have, *want);
            if (ch.tuning && in.busy) {
                // someone sits in our copy: InstallTuningItems would rebuild the car under him (docs/re-notes.md
                // 13). The tuning waits until the copy is empty; the rest does not rebuild it and is set now.
                ++stats_.deferred;
                if (!c.deferred) {
                    c.deferred = true;
                    std::snprintf(buf, sizeof buf, "car %s: its tuning (%zu item(s)) waits: someone sits in our copy "
                                                   "(entity %u)%s",
                                  ref.c_str(), ch.tuningIds.size(), in.entity,
                                  ch.color || ch.plate || ch.tint || ch.interior || ch.dirt || ch.rust || ch.painting
                                      ? "; the rest of its look is set now"
                                      : "");
                    out.logs.emplace_back(buf);
                    out.trace.push_back("CLD ref=" + ref + " ent=" + std::to_string(in.entity) + " ev=defer items=" +
                                        std::to_string(ch.tuningIds.size()));
                }
                ch.tuning = false;
                ch.tuningIds.clear();
                if (ch.Empty()) continue;
            } else if (c.deferred && !in.busy) {
                // empty again: the tuning gets its own tries
                c.deferred = false;
                c.tries = 0;
                c.gaveUp = false;
                c.triedAt = -1e9;
                if (ch.tuning) {
                    std::snprintf(buf, sizeof buf, "car %s: our copy (entity %u) is empty: its tuning is set now",
                                  ref.c_str(), in.entity);
                    out.logs.emplace_back(buf);
                    out.trace.push_back("CLD ref=" + ref + " ent=" + std::to_string(in.entity) + " ev=resume items=" +
                                        std::to_string(ch.tuningIds.size()));
                }
            }
            if (young && ch.tuning && !c.gaveUp) out.holds.push_back(in.entity);
            if (ch.Empty()) {
                c.deferred = false;
                ++stats_.matching;
                if (!c.matchLogged) {
                    c.matchLogged = true;
                    std::snprintf(buf, sizeof buf, "car %s: our copy (entity %u) shows its look%s: %s", ref.c_str(),
                                  in.entity, c.tries ? (" after " + std::to_string(c.tries) + " try(s)").c_str() : "",
                                  in.have.Describe().c_str());
                    out.logs.emplace_back(buf);
                    out.trace.push_back("CLM ref=" + ref + " ent=" + std::to_string(in.entity) + " tries=" +
                                        std::to_string(c.tries));
                }
                continue;
            }
            c.matchLogged = false;
            if (c.gaveUp) {
                ++stats_.failed;
                continue;
            }
            if (c.tries >= cfg_.tries) {
                c.gaveUp = true;
                ++stats_.failed;
                std::snprintf(buf, sizeof buf, "car %s: our copy (entity %u) does not take its look after %d tries: "
                                               "still differs in %s (it has %s; the car %s)",
                              ref.c_str(), in.entity, c.tries, ch.Describe().c_str(), in.have.Describe().c_str(),
                              want->Describe().c_str());
                out.logs.emplace_back(buf);
                out.trace.push_back("CLF ref=" + ref + " ent=" + std::to_string(in.entity) + " what=" + NoSpace(ch.Describe()));
                continue;
            }
            if (now - c.triedAt < cfg_.retryEvery) continue;
            ++c.tries;
            c.triedAt = now;
            Apply a;
            a.entity = in.entity;
            a.ref = in.ref;
            a.change = ch;
            a.want = *want;
            a.attempt = c.tries;
            std::snprintf(buf, sizeof buf, "car %s: its look set on our copy (entity %u, try %d: %s; it had %s)",
                          ref.c_str(), in.entity, c.tries, ch.Describe().c_str(), in.have.Describe().c_str());
            out.logs.emplace_back(buf);
            out.trace.push_back("CLA ref=" + ref + " ent=" + std::to_string(in.entity) + " try=" +
                                std::to_string(c.tries) + " what=" + NoSpace(ch.Describe()));
            out.applies.push_back(std::move(a));
            ++stats_.applied;
        }
    }
    for (auto it = cars_.begin(); it != cars_.end();) {
        const Car& c = it->second;
        if (now - c.seen > cfg_.forget && (c.askers.empty() || now - c.askAt > cfg_.forget))
            it = cars_.erase(it);
        else
            ++it;
    }
}

}  // namespace mc::game::car_look
