// Map markers for the other players: what to do with a player's marker (see map_markers.h).
#include "game/map_markers.h"

#include <cstring>

namespace mc::game::mapmark {

Plan Decide(const Input& in) {
    Plan p;
    // a puppet whose brain switch is still to come carries no icon (the switch would remove it): his position does
    const bool puppet = in.puppet && !in.brainPending;
    p.want = puppet ? Target::Puppet : (in.hasPos ? Target::Pos : Target::None);
    if (p.want == Target::Puppet && in.humanFailed && in.icon != Icon::Human) {
        // no human icon for this puppet: a position icon may still follow it (or show where he is)
        if (in.icon == Icon::None) p.want = in.hasPos ? Target::Pos : Target::None;
    }
    if (p.want == Target::None) {  // nowhere to show him: an icon stays where it is
        p.want = in.icon == Icon::None ? Target::None : in.target;
        return p;
    }
    if (in.icon == Icon::None) {
        p.act = p.want == Target::Puppet ? Action::CreateHuman : Action::CreatePos;
        return p;
    }
    if (p.want == Target::Puppet) {
        if (in.icon == Icon::Pos && !in.humanFailed) {
            p.act = Action::ReplaceWithHuman;
        } else if (in.target != Target::Puppet || in.puppetChanged) {
            p.act = Action::ToPuppet;
        }
        return p;
    }
    // Target::Pos
    if (in.target != Target::Pos) {
        p.act = Action::ToPos;
        return p;
    }
    const float dx = in.x - in.lastX, dy = in.y - in.lastY, dz = in.z - in.lastZ;
    if (dx * dx + dy * dy + dz * dz > kMoveEps * kMoveEps) p.act = Action::MovePos;
    return p;
}

Probe CheckProbe(uint32_t id, double found, int lossesBefore) {
    if (found < 0) return Probe::Unknown;
    if (found == static_cast<double>(id)) return Probe::Ok;
    return lossesBefore + 1 >= kMaxLosses ? Probe::GiveUp : Probe::Lost;
}

const char* ProbeName(Probe p) {
    switch (p) {
        case Probe::Ok: return "ok";
        case Probe::Lost: return "lost";
        case Probe::GiveUp: return "give_up";
        default: return "unknown";
    }
}

const char* IconName(Icon i) {
    switch (i) {
        case Icon::Human: return "human";
        case Icon::Pos: return "pos";
        default: return "none";
    }
}

const char* TargetName(Target t) {
    switch (t) {
        case Target::Puppet: return "puppet";
        case Target::Pos: return "pos";
        default: return "none";
    }
}

const char* ActionName(Action a) {
    switch (a) {
        case Action::CreateHuman: return "create_human";
        case Action::CreatePos: return "create_pos";
        case Action::ReplaceWithHuman: return "replace_with_human";
        case Action::ToPuppet: return "to_puppet";
        case Action::ToPos: return "to_pos";
        case Action::MovePos: return "move_pos";
        default: return "keep";
    }
}

Icon IconFromName(const char* s) {
    if (!s) return Icon::None;
    if (!std::strcmp(s, "human")) return Icon::Human;
    if (!std::strcmp(s, "pos")) return Icon::Pos;
    return Icon::None;
}

Target TargetFromName(const char* s) {
    if (!s) return Target::None;
    if (!std::strcmp(s, "puppet")) return Target::Puppet;
    if (!std::strcmp(s, "pos")) return Target::Pos;
    return Target::None;
}

std::string CleanName(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (unsigned char c : name) s += (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c);
    const size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return {};
    s = s.substr(b, s.find_last_not_of(' ') - b + 1);
    if (s.size() > kMaxNameBytes) {
        size_t n = kMaxNameBytes;
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;  // s[n] continues a character
        s.resize(n);
        const size_t e = s.find_last_not_of(' ');
        s.resize(e == std::string::npos ? 0 : e + 1);
    }
    return s;
}

std::string NameLine(uint32_t id, const std::string& name) {
    const std::string n = CleanName(name);
    if (n.empty()) return {};
    return "N " + std::to_string(id) + " " + n + "\n";
}

}  // namespace mc::game::mapmark
