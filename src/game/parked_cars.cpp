#include "game/parked_cars.h"

#include <cmath>

namespace mc::game::parked {

bool Car::operator==(const Car& o) const {
    if (owner != o.owner || guid != o.guid || model != o.model || flags != o.flags) return false;
    for (int i = 0; i < 7; ++i)
        if (t[i] != o.t[i]) return false;
    return true;
}

bool Valid(const Car& c) {
    if (c.model.empty() || c.model.size() > 48) return false;
    for (unsigned char ch : c.model)
        if (ch <= 0x20 || ch >= 0x7F) return false;
    for (int i = 0; i < 7; ++i) {
        const float lim = i < 3 ? 1e6f : 2.0f;
        if (!(std::fabs(c.t[i]) <= lim)) return false;  // NaN fails too
    }
    return true;
}

void Write(net::ByteWriter& w, const std::vector<Car>& cars) {
    const size_t n = cars.size() < kPerMessage ? cars.size() : kPerMessage;
    w.U8(kVersion);
    w.U8(static_cast<uint8_t>(n));
    for (size_t i = 0; i < n; ++i) {
        const Car& c = cars[i];
        w.U8(c.owner);
        w.U32(c.guid);
        w.Str(c.model);
        for (float v : c.t) w.F32(v);
        w.U8(c.flags);
    }
}

bool Read(net::ByteReader& r, std::vector<Car>& out) {
    out.clear();
    if (r.U8() != kVersion) return false;
    const uint8_t n = r.U8();
    if (!r.Ok() || n > kPerMessage) return false;
    for (uint8_t i = 0; i < n; ++i) {
        Car c;
        c.owner = r.U8();
        c.guid = r.U32();
        c.model = r.Str(48);
        for (float& v : c.t) v = r.F32();
        c.flags = static_cast<uint8_t>(r.U8() & kWrecked);
        if (!r.Ok() || !Valid(c)) {
            out.clear();
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

std::vector<std::vector<Car>> Split(const std::vector<Car>& all) {
    std::vector<std::vector<Car>> parts;
    for (size_t i = 0; i < all.size(); i += kPerMessage) {
        const size_t end = i + kPerMessage < all.size() ? i + kPerMessage : all.size();
        parts.emplace_back(all.begin() + static_cast<std::ptrdiff_t>(i),
                           all.begin() + static_cast<std::ptrdiff_t>(end));
    }
    return parts;
}

void Book::Parked(const Car& c) {
    if (!Valid(c)) return;
    cars_[{c.owner, c.guid}] = c;
    while (cars_.size() > kMax) cars_.erase(cars_.begin());
}

bool Book::Driven(uint8_t owner, uint32_t guid) { return cars_.erase({owner, guid}) > 0; }

std::vector<Car> Book::Snapshot() const {
    std::vector<Car> out;
    out.reserve(cars_.size());
    for (const auto& [k, c] : cars_) out.push_back(c);
    return out;
}

}  // namespace mc::game::parked
