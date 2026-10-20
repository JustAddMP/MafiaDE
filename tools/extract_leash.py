"""Both players' positions at 1 Hz from a host's sync trace, for tests/leash_test.cpp (tests/data/leash_1020.h).

Usage: python tools/extract_leash.py --out tests/data/leash_1020.h NAME=HOST_TRACE [NAME=HOST_TRACE ...]

From the host's trace (docs/sync-trace.md): TX = the host's own state, RX from=1 = the guest's state as the host got
it. Per second of the host's trace clock the last state of each (the car's position `cp` when he drives, else `p`);
a second without one keeps the one before (the guest's up to 3 s; the host's for as long as it takes: his puppet
stands where his last state put it, and a gap of more than 3 s in his states is marked as his game paused, as
PeerPaused would say), seconds before both are known are left out. A row: t (s since the first row), host x y, guest
x y (m), host hp, guest hp, same car (1: both name the same car key: the guest sits in the host's car or the host in
his), host paused (1: no state of the host's for more than 3 s).
"""
import argparse
import math
import re

LINE = re.compile(r"^(\d+\.\d+) (TX|RX) (.*)$")


def fields(rest):
    out = {}
    for tok in rest.split(" "):
        k, sep, v = tok.partition("=")
        if sep:
            out[k] = v
    return out


def xy(v):
    try:
        x, y = v.split(",")[:2]
        return float(x), float(y)
    except (AttributeError, ValueError):
        return None


def extract(path):
    host, guest = {}, {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line.rstrip("\r\n"))
            if not m:
                continue
            t, kind, rest = float(m.group(1)), m.group(2), m.group(3)
            fl = fields(rest)
            if kind == "RX" and fl.get("from") != "1":
                continue
            car = fl.get("car")
            pos = xy(fl.get("cp")) if car and car != "-" else None
            pos = pos or xy(fl.get("p"))
            if pos is None:
                continue
            hp = int(float(fl.get("hp", "100")))
            rec = (pos[0], pos[1], hp, car if car and car != "-" else None)
            (host if kind == "TX" else guest)[int(t)] = rec
    if not host or not guest:
        return []
    first, last = max(min(host), min(guest)), min(max(host), max(guest))
    rows, hl, gl, hage, gage = [], None, None, 99, 99
    for s in range(first, last + 1):
        if s in host:
            hl, hage = host[s], 0
        else:
            hage += 1
        if s in guest:
            gl, gage = guest[s], 0
        else:
            gage += 1
        if hl is None or gl is None or gage > 3:
            continue
        same = 1 if hl[3] and gl[3] and hl[3] == gl[3] else 0
        rows.append((s, hl[0], hl[1], gl[0], gl[1], hl[2], gl[2], same, 1 if hage > 3 else 0))
    t0 = rows[0][0] if rows else 0
    return [(r[0] - t0,) + r[1:] for r in rows]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("sessions", nargs="+", help="NAME=host trace")
    args = ap.parse_args()
    out = ["// Both players' positions at 1 Hz for tests/leash_test.cpp, extracted by tools/extract_leash.py from the",
           "// 2026-10-20 host traces (TX = the host's state, RX from=1 = the guest's; the car's position when driving).",
           "// Row: t (s from the first row), host x, host y, guest x, guest y (m), host hp, guest hp, same car (1/0),",
           "// host paused (1: no state of his for more than 3 s; his puppet stands where his last state put it).",
           "#pragma once", "", "struct LeashRow {", "    int t;", "    float hx, hy, gx, gy;", "    int hostHp, guestHp;",
           "    int sameCar, hostPaused;", "};", ""]
    for spec in args.sessions:
        name, _, path = spec.partition("=")
        rows = extract(path)
        dmax = max((math.hypot(r[1] - r[3], r[2] - r[4]) for r in rows), default=0)
        paused = sum(r[8] for r in rows)
        out.append("// %s: %d rows (%d with the host paused), players at most %.0f m apart (%s)" % (
            name, len(rows), paused, dmax, path.replace("\\", "/").split("/")[-1]))
        out.append("static const LeashRow k%s[] = {" % name)
        line = "   "
        for r in rows:
            cell = " {%d,%.1ff,%.1ff,%.1ff,%.1ff,%d,%d,%d,%d}," % r
            if len(line) + len(cell) > 118:
                out.append(line)
                line = "   "
            line += cell
        if line.strip():
            out.append(line)
        out.append("};")
        out.append("")
    with open(args.out, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(out))
    print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
