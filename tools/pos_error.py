"""Position error between two co-op games, from their logs (both started with trace_pos = 1 / --trace_pos 1).

    python tools/pos_error.py out/logs/mafiacoop-host.log out/logs/mafiacoop-guest.log [--from HH:MM:SS] [--to HH:MM:SS]

Each game logs "trace L" (its player, and his car) and "trace P <id>" (each puppet: where it stands, where the network
says it should be now, and the car copy) 10 times a second. The two games run on one PC, so their log clocks agree:
every "trace P" line of game B for the player of game A is compared with game A's "trace L" position at the same
moment (linear between A's samples). Reported per direction, on foot and in a car, in metres (ground plane):
  puppet  - where the puppet / car copy really is vs where the player really is (what the other player sees)
  target  - where the network says he is (after interpolation) vs where he really is (the network part alone)
"""
import re
import sys
from bisect import bisect_left

TIME = re.compile(r"^﻿?(?:\d{4}-\d\d-\d\d )?(\d\d):(\d\d):(\d\d)\.(\d{3}) ")
L_RE = re.compile(r"trace L (\S+) (\S+) (\S+)(?: car (\S+) (\S+) (\S+))?")
P_RE = re.compile(r"trace P (\d+) (\S+) (\S+) (\S+) tgt (\S+) (\S+) (\S+)(?: car (\S+) (\S+) (\S+) ctgt (\S+) (\S+) (\S+))?")
ID_RE = re.compile(r"local player (\d+)")


def secs(m):
    h, mi, s, ms = (int(x) for x in m.groups())
    return h * 3600 + mi * 60 + s + ms / 1000.0


def parse(path, t0, t1):
    ls, ps, me = [], [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = TIME.match(line)
            if not m:
                continue
            t = secs(m)
            if me is None:
                i = ID_RE.search(line)
                if i:
                    me = i.group(1)
            if (t0 is not None and t < t0) or (t1 is not None and t > t1):
                continue
            a = L_RE.search(line)
            if a:
                v = [float(x) if x else None for x in a.groups()]
                ls.append((t, v[0:3], v[3:6] if v[3] is not None else None))
                continue
            b = P_RE.search(line)
            if b:
                g = b.groups()
                v = [float(x) if x else None for x in g[1:]]
                ps.append((t, g[0], v[0:3], v[3:6], v[6:9] if v[6] is not None else None,
                           v[9:12] if v[9] is not None else None))
    return ls, ps, me


def at(ls, times, t, car):
    i = bisect_left(times, t)
    if i == 0 or i >= len(ls):
        return None
    (ta, pa, ca), (tb, pb, cb) = ls[i - 1], ls[i]
    if tb - ta > 0.5:
        return None
    a, b = (ca, cb) if car else (pa, pb)
    if a is None or b is None:
        return None
    k = (t - ta) / (tb - ta) if tb > ta else 0
    return [a[j] + (b[j] - a[j]) * k for j in range(3)]


def d2(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def stats(xs):
    if not xs:
        return "no samples"
    xs = sorted(xs)
    n = len(xs)
    return "n %4d  mean %5.2f  median %5.2f  p95 %5.2f  max %6.2f" % (
        n, sum(xs) / n, xs[n // 2], xs[min(n - 1, int(n * 0.95))], xs[-1])


def report(name, src, dst):
    ls, _, sid = src
    _, ps, _ = dst
    times = [x[0] for x in ls]
    foot_p, foot_t, car_p, car_t = [], [], [], []
    for t, pid, pos, tgt, car, ctgt in ps:
        if sid is not None and pid != sid:
            continue
        if car is not None:
            real = at(ls, times, t, True)
            if real:
                car_p.append(d2(car, real))
                car_t.append(d2(ctgt, real))
        else:
            real = at(ls, times, t, False)
            if real:
                foot_p.append(d2(pos, real))
                foot_t.append(d2(tgt, real))
    print(name)
    print("  on foot  puppet  " + stats(foot_p))
    print("           target  " + stats(foot_t))
    print("  in car   copy    " + stats(car_p))
    print("           target  " + stats(car_t))


def main():
    args = sys.argv[1:]
    t0 = t1 = None
    if "--from" in args:
        i = args.index("--from")
        t0 = secs(TIME.match(args[i + 1] + ".000 "))
        del args[i:i + 2]
    if "--to" in args:
        i = args.index("--to")
        t1 = secs(TIME.match(args[i + 1] + ".000 "))
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    a, b = parse(args[0], t0, t1), parse(args[1], t0, t1)
    report("player of %s as seen on %s" % (args[0], args[1]), a, b)
    report("player of %s as seen on %s" % (args[1], args[0]), b, a)


if __name__ == "__main__":
    main()
