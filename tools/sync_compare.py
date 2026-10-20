"""Compare the sync traces of two co-op games (docs/sync-trace.md).

    python tools/sync_compare.py <trace of one game> <trace of the other> [--csv DIR] [--worst N]

Each game writes one trace per session to logs\\trace\\ next to its log (mafiacoop-<tag>-<date>-<time>.trace). Give
the two traces of the same session (one from each PC, or both from one PC). The report covers, for each player as
seen on the other's game:

- network: states sent and received, loss, one-way latency and jitter, the clock relation between the two machines
- on foot: how far the puppet really was from the player at the same moment (both clocks lined up), per gait, how
  often the puppet's gait matched the player's, snaps, nudges and catch-up time
- cars: how far the car copy was from the driver's car, its own physics against pinned, pins and why, retries, the
  copy's speed against the driver's, the steering wish and front wheel angle against the driver's, engine and gear
- seats: puppets thrown out of cars (from the copied log lines)
- NPCs: how far the guest's NPCs were from the host's
- swapped cars: real cars hidden behind a copy and how they came back (SWR / SWP records)
- pedestrians: each game's pedestrians as replicas on the other (PDT / PDR / PDC): true error, coverage
- bandwidth per system (from the copied "net: messages:" log lines of core's NetMonitor): typical (median) and
  worst (max) KB/s out and in, plus what the send budget thinned or deferred
- latency per system (LAT records): how long a message took from the sender's send to this game applying it,
  p50 / p95 / max per system and receiving game (age on arrival + hand-off to the script tick)
- the worst moments with the wall-clock time of both games, to look them up in a video or the logs

--csv DIR writes the lined-up time series (one file per player and kind) for plotting.
--check prints every sync target of docs/sync-architecture.md section 4 with PASS / FAIL / no data (exit code 1 when
one fails), per direction where it applies.
Only the Python standard library is used.
"""
import argparse
import bisect
import csv
import math
import os
import re
import statistics
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta

GAITS = {0: "stand", 1: "walk", 2: "run", 3: "sprint"}
FIX_LOCO = {0: "none", 1: "nudge", 2: "snap"}
FIX_VEH = {0: "none", 1: "blend", 2: "pin", 3: "snap"}
WHY = {0: "-", 1: "too slow", 2: "ran off", 3: "stays off"}
LOG_RE = re.compile(r"^\d{4}-\d\d-\d\d$")

# ---- sync targets (docs/sync-architecture.md section 4) -------------------------------------------------------------
# metric -> (comparison, limit, what it is). Sections call record(metric, value, who); --check compares every recorded
# value with its limit. A package that adds a kind adds its metrics here and record() calls in its section.
TARGETS = {
    "foot.p95": ("<=", 0.8, "puppet vs player at the same moment, p95 (m)"),
    "foot.mean": ("<=", 0.35, "puppet vs player, mean (m)"),
    "foot.stand.p95": ("<=", 0.3, "puppet vs a standing player, p95 (m)"),
    "foot.gait": (">=", 90.0, "puppet gait matches the player's (%)"),
    "foot.snaps_per_min": ("<=", 0.5, "puppet snaps per minute on foot"),
    "foot.shown": (">=", 98.0, "his puppet (or the car copy he sits in) there while his states arrive (% of the time)"),
    "foot.respawns": ("<=", 0, "puppet spawned again with no reason logged (not a character change, a new world or the game removing it)"),
    "foot.respawn_s": ("<=", 5.0, "puppet the game removed: back after (s, max)"),
    "foot.dz_p95": ("<=", 0.5, "puppet height vs the player's on foot, p95 (m; a puppet under the floor)"),
    "car.p95": ("<=", 1.0, "car copy vs the driver's car, p95 (m)"),
    "car.pinned": ("<=", 10.0, "car copy pinned (% of samples)"),
    "car.speed_ratio": ("between", (0.9, 1.1), "copy speed / driver's speed, median"),
    "traffic.true_p95": ("<=", 1.5, "traffic copy vs the authority's car at the same moment, p95 (m)"),
    "traffic.shown": (">=", 95.0, "authority's traffic shown while near it (% of the time)"),
    "traffic.unshared_near": ("<=", 0, "own traffic cars still driving near a receiver (max)"),
    "traffic.crew_exits_per_min": ("<=", 1.0, "traffic copies whose crew was sent out, per minute with copies"),
    "traffic.reseats_per_min": ("<=", 3.0, "traffic copy crew members put back into their seat, per minute with copies"),
    "npc.p95": ("<=", 1.0, "NPC replica vs the authority's NPC, p95 (m)"),
    "npc.max": ("<=", 4.0, "NPC replica vs the authority's NPC, max (m)"),
    "npc.bw": ("<=", 8.0, "NPC states + infos to one guest, host upload, median (KB/s)"),
    "ped.coverage": (">=", 80.0, "authority pedestrians within 40 m with a replica (%)"),
    "ped.p95": ("<=", 1.5, "pedestrian replica vs the authority's, p95 (m)"),
    "world.time_err": ("<=", 0.5, "time of day difference (game minutes, max after the first sync)"),
    "join.complete_s": ("<=", 10.0, "a guest's world complete after a join / reconnect (s)"),
    "lat.player.p95": ("<=", 40.0, "PlayerState send -> applied, p95 (ms)"),
    "bw.host_up": ("<=", 160.0, "host upload, all systems, median (KB/s)"),
}
METRICS = []  # (metric, value, who)


def record(metric, value, who=""):
    """A measured value for --check (docs/sync-architecture.md section 4)."""
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return
    METRICS.append((metric, float(value), who))


def check():
    section("Sync targets (docs/sync-architecture.md section 4)")
    failed = 0
    for metric, (op, limit, what) in TARGETS.items():
        vals = [(v, who) for m, v, who in METRICS if m == metric]
        if not vals:
            print("  %-22s no data       %s" % (metric, what))
            continue
        for v, who in vals:
            if op == "<=":
                ok, lim = v <= limit, "<= %g" % limit
            elif op == ">=":
                ok, lim = v >= limit, ">= %g" % limit
            else:
                ok, lim = limit[0] <= v <= limit[1], "%g..%g" % limit
            failed += 0 if ok else 1
            print("  %-22s %-4s %9.2f  (%s)  %s%s" % (metric, "PASS" if ok else "FAIL", v, lim, what,
                                                      ("  [" + who + "]") if who else ""))
    print("  %d failed" % failed)
    return failed


def num(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def vec(v):
    if v is None or v == "-":
        return None
    try:
        return tuple(float(x) for x in v.split(","))
    except ValueError:
        return None


def parse_fields(rest):
    out = {}
    for tok in rest.split(" "):
        k, sep, v = tok.partition("=")
        if sep:
            out[k] = v
    return out


class Trace:
    def __init__(self, path):
        self.path = path
        self.header = {}
        self.wall0 = None
        self.qpc0 = None
        self.players = {}
        self.records = defaultdict(list)  # type -> [(t, fields)]
        self.logs = []  # (t, text)
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\r\n")
                if not line:
                    continue
                head, _, rest = line.partition(" ")
                t = num(head)
                if t is None:
                    continue
                typ, _, rest = rest.partition(" ")
                if LOG_RE.match(typ):  # a copied main-log line: "<date> <time> [tid] LVL text"
                    parts = rest.split(" ", 3)
                    self.logs.append((t, parts[3] if len(parts) > 3 else rest))
                    continue
                if typ == "H":
                    self.header = parse_fields(rest)
                    w = self.header.get("wall")
                    if w:
                        try:
                            self.wall0 = datetime.strptime(w, "%Y-%m-%dT%H:%M:%S.%f")
                        except ValueError:
                            self.wall0 = None
                    self.qpc0 = num(self.header.get("qpc"), t)
                    continue
                if typ == "PL":
                    name = rest.split("name=", 1)[1] if "name=" in rest else "?"
                    fl = parse_fields(rest.split("name=", 1)[0])
                    self.players[int(num(fl.get("id"), -1))] = name
                    continue
                self.records[typ].append((t, parse_fields(rest)))
        self.role = self.header.get("role", "?")
        self.local = int(num(self.header.get("local"), -1))
        self.name = self.players.get(self.local, "player %d" % self.local)

    def wall(self, t):
        if self.wall0 is None or self.qpc0 is None:
            return "?"
        return (self.wall0 + timedelta(seconds=t - self.qpc0)).strftime("%H:%M:%S.%f")[:-3]

    def span(self):
        ts = [r[0] for recs in self.records.values() for r in recs[:1] + recs[-1:]]
        return (min(ts), max(ts)) if ts else (0, 0)


class Series:
    """Time series of points for lookups by time (linear interpolation, None outside or across gaps)."""

    def __init__(self, items, gap=0.5):
        items = sorted((t, v) for t, v in items if v is not None)
        self.t = [i[0] for i in items]
        self.v = [i[1] for i in items]
        self.gap = gap

    def at(self, t):
        i = bisect.bisect_left(self.t, t)
        if i <= 0 or i >= len(self.t):
            return None
        t0, t1 = self.t[i - 1], self.t[i]
        if t1 - t0 > self.gap:
            return None
        a, b = self.v[i - 1], self.v[i]
        k = (t - t0) / (t1 - t0) if t1 > t0 else 0
        if isinstance(a, tuple):
            return tuple(x + (y - x) * k for x, y in zip(a, b))
        return a + (b - a) * k

    def nearest(self, t, within=0.2):
        i = bisect.bisect_left(self.t, t)
        best = None
        for j in (i - 1, i):
            if 0 <= j < len(self.t) and abs(self.t[j] - t) <= within:
                if best is None or abs(self.t[j] - t) < abs(self.t[best] - t):
                    best = j
        return None if best is None else self.v[best]


def dist2(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def pct(values, p):
    if not values:
        return float("nan")
    s = sorted(values)
    k = (len(s) - 1) * p / 100
    lo, hi = math.floor(k), math.ceil(k)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def stats_line(values, unit="m"):
    if not values:
        return "no samples"
    return "mean %.2f, median %.2f, p95 %.2f, max %.2f %s (%d samples)" % (
        statistics.fmean(values), pct(values, 50), pct(values, 95), max(values), unit, len(values))


def clock_relation(rx_side, tx_side, sender_id):
    """Local clock of rx_side minus the clock of tx_side (seconds), from RX offsets and the round trip time."""
    offs = [num(f.get("off")) for _, f in rx_side.records["RX"] if num(f.get("from"), -1) == sender_id]
    offs = [o for o in offs if o is not None]
    rtts = [num(f.get("rtt")) for _, f in rx_side.records["CLK"] if num(f.get("id"), -1) == sender_id]
    rtts = [r for r in rtts if r is not None and r > 0]
    if not offs:
        return None, None, None
    off_min = min(offs[len(offs) // 10:] or offs)  # the minimum after the first tenth (settled)
    rtt = pct(rtts, 10) / 1000 if rtts else 0.0
    return off_min - rtt / 2, off_min, rtt


def section(title):
    print()
    print(title)
    print("-" * len(title))


def compare(sender, receiver, args, worst):
    """How player `sender.local` looked on `receiver`'s game."""
    sid = sender.local
    label = "%s (%s, player %d) on %s's game (%s)" % (sender.name, sender.role, sid, receiver.name, receiver.role)
    section(label)
    diff, off_min, rtt = clock_relation(receiver, sender, sid)
    if diff is None:
        print("  no states from this player in the receiver's trace")
        return
    to_sender = lambda t: t - diff  # receiver clock -> sender clock

    # ---- network ----
    tx = {int(num(f["sT"])): (t, f) for t, f in sender.records["TX"] if "sT" in f}
    rx = [(t, f) for t, f in receiver.records["RX"] if num(f.get("from"), -1) == sid]
    got = {int(num(f["sT"])): t for t, f in rx}
    first, last = (min(got), max(got)) if got else (0, 0)
    sent_in = [s for s in tx if first <= s <= last]
    lost = [s for s in sent_in if s not in got]
    lat = [(got[s] - diff - tx[s][0]) * 1000 for s in sent_in if s in got]
    drops = len([1 for _, f in receiver.records["RXD"] if num(f.get("from"), -1) == sid])
    print("  network: %d states sent in that time, %d received, %d lost (%.2f %%), %d dropped as old/duplicate" % (
        len(sent_in), len(got), len(lost), 100.0 * len(lost) / max(1, len(sent_in)), drops))
    print("  clocks: receiver - sender = %.4f s (min offset %.4f s, round trip ~%.1f ms)" % (diff, off_min, rtt * 1000))
    if lat:
        print("  one-way latency (estimated): %s; jitter (p95 - p5) %.1f ms" % (
            stats_line(lat, "ms"), pct(lat, 95) - pct(lat, 5)))
    interp = num(receiver.header.get("interp"), 0)
    print("  shown delay: interpolation %d ms (led forward by the same along the velocity) + latency" % interp)

    # ---- truth: the sender's own positions (TX, its clock) ----
    truth_p = Series([(num(f["sT"]) / 1000, vec(f.get("p"))) for _, f in sender.records["TX"]], gap=0.3)
    truth_c = Series([(num(f["sT"]) / 1000, vec(f.get("cp"))) for _, f in sender.records["TX"] if "cp" in f], gap=0.3)
    truth_mode = Series([(num(f["sT"]) / 1000, num(f.get("m"), 0)) for _, f in sender.records["TX"]], gap=0.3)
    lr_rb = Series([(t, vec(f.get("rb"))) for t, f in sender.records["LR"] if "rb" in f], gap=0.3)

    # ---- on foot ----
    pup = [(t, f) for t, f in receiver.records["PUP"] if num(f.get("id"), -1) == sid]
    by_gait = defaultdict(list)
    errs_ctl, errs_true, rows = [], [], []
    confusion = Counter()
    snaps = nudges = carry_n = 0
    nudge_sum = 0.0
    # He sat on something his game sent as on foot (0.16.0: a motorbike rider, 0.7 m above his puppet): those
    # moments (the puppet more than 0.45 m below him, +-1.5 s) are counted apart, not in the on-foot error (their
    # height shows in foot.dz_p95); 2026-10-20 test: they were 38-77 % of the "on foot" samples of a direction.
    seated_t = []
    for t, f in pup:
        cp, tp = vec(f.get("cp")), truth_p.at(to_sender(t))
        if f.get("have") == "1" and cp and tp and len(cp) > 2 and len(tp) > 2 and cp[2] - tp[2] < -0.45:
            seated_t.append(t)

    def seated(t):
        i = bisect.bisect_left(seated_t, t - 1.5)
        return i < len(seated_t) and seated_t[i] <= t + 1.5

    errs_seated = []
    for t, f in pup:
        if f.get("have") != "1":
            continue
        if seated_t and seated(t):
            cp, tp = vec(f.get("cp")), truth_p.at(to_sender(t))
            if cp and tp:
                errs_seated.append(dist2(cp, tp))
            continue
        cp, err = vec(f.get("cp")), num(f.get("err"), 0)
        g = int(num(f.get("g"), 0))
        sm = int(num(f.get("sm"), 0))
        errs_ctl.append(err)
        confusion[(sm, g)] += 1
        fix = int(num(f.get("fix"), 0))
        if fix == 2:
            snaps += 1
        elif fix == 1:
            nudges += 1
            n = vec(f.get("nudge"))
            if n:
                nudge_sum += math.hypot(n[0], n[1])
        if num(f.get("carry"), 0) > 0:
            carry_n += 1
        tp = truth_p.at(to_sender(t))
        if cp and tp:
            e = dist2(cp, tp)
            errs_true.append(e)
            by_gait[GAITS.get(sm, "?")].append(e)
            worst.append((e, "on foot", label, receiver.wall(t), sender.wall(to_sender(t)), "gait %s (player %s)" % (
                GAITS.get(g), GAITS.get(sm))))
            rows.append((t, to_sender(t), cp[0], cp[1], tp[0], tp[1], e, err, g, sm, fix,
                         num(f.get("mult"), 1), num(f.get("want"), 0), num(f.get("meas"), -1), num(f.get("carry"), 0)))
    print("  on foot: %d samples" % len(pup))
    who = "%s on %s" % (sender.role, receiver.role)
    if errs_seated:
        print("    left out: he sat on something sent as on foot (puppet > 0.45 m below him): %s" % stats_line(
            errs_seated))
    if errs_true:
        print("    puppet vs the player at the same moment: %s" % stats_line(errs_true))
        record("foot.p95", pct(errs_true, 95), who)
        record("foot.mean", statistics.fmean(errs_true), who)
        if by_gait.get("stand"):
            record("foot.stand.p95", pct(by_gait["stand"], 95), who)
        for gname in ("stand", "walk", "run", "sprint"):
            if by_gait.get(gname):
                print("      player %-6s %s" % (gname + ":", stats_line(by_gait[gname])))
    if errs_ctl:
        print("    puppet vs its network target (controller error): %s" % stats_line(errs_ctl))
    total = sum(confusion.values())
    if total:
        same = sum(v for (a, b), v in confusion.items() if a == b)
        print("    gait matches the player's: %.1f %% of samples" % (100.0 * same / total))
        mism = [(v, a, b) for (a, b), v in confusion.items() if a != b]
        for v, a, b in sorted(mism, reverse=True)[:5]:
            print("      player %s, puppet %s: %.1f %%" % (GAITS.get(a), GAITS.get(b), 100.0 * v / total))
        print("    fixes: %d snaps, %d nudged samples (%.2f m in all), catching up in %.1f %% of samples" % (
            snaps, nudges, nudge_sum, 100.0 * carry_n / total))
        record("foot.gait", 100.0 * same / total, who)
        on_foot_s = [t for t, f in pup if f.get("have") == "1"]
        if len(on_foot_s) > 1:
            # PUP is written at 60 Hz while on foot: minutes on foot from the sample count (gaps not counted)
            record("foot.snaps_per_min", snaps / max(len(on_foot_s) / 60.0 / 60.0, 1e-6), who)
    # ---- the game's AI walk against our nudges (the open question of the 2026-10-20 test: does a SetPos every
    # frame keep the AI from walking?). meas = the controller's own measurement of its walk (every frame's nudge
    # taken out); nsum = metres our nudges moved it so far (PUP field since the velocity matching, not in 0.16.0)
    walk_meas = defaultdict(list)
    last_meas = None
    moved = nudged = 0.0
    prev = None
    for t, f in pup:
        if f.get("have") != "1" or (seated_t and seated(t)):
            prev = None
            continue
        g, m = int(num(f.get("g"), 0)), num(f.get("meas"), -1)
        if g > 0 and m >= 0 and m != last_meas:
            walk_meas[g].append((m, num(f.get("mult"), 1)))
        last_meas = m
        cp, ns = vec(f.get("cp")), num(f.get("nsum"))
        if prev and cp and ns is not None and int(num(f.get("sm"), 0)) > 0 and t - prev[0] < 0.1:
            moved += dist2(cp, prev[1])
            nudged += max(0.0, ns - prev[2])
        prev = (t, cp, ns) if cp and ns is not None else None
    if walk_meas:
        print("    the AI walk (its own, nudges out): %s" % ", ".join(
            "%s %.2f m/s at x%.2f (p50 of %d)" % (GAITS.get(g), pct([x[0] for x in v], 50), pct([x[1] for x in v], 50),
                                                  len(v)) for g, v in sorted(walk_meas.items())))
    if moved > 1:
        print("    while he moved our nudges moved the puppet %.1f m against %.1f m in all (%.0f %%: foot sliding)" % (
            nudged, moved, 100.0 * nudged / moved))
    # ---- there at all (2026-10-20 test: a puppet that is gone counts in none of the numbers above) ----
    pr = [(t, f) for t, f in receiver.records["PR"] if num(f.get("id"), -1) == sid]
    if rx:
        have = set(int(t * 2) for t, f in pr if f.get("hid") != "1" or f.get("veh", "nil") not in ("nil", "-"))
        # the receiver's own pauses (menus: no script tick, no PR records) are left out
        paused, since = set(), None
        for t, f in receiver.records["PAU"]:
            if num(f.get("id"), -1) != receiver.local:
                continue
            ev = f.get("ev")
            if ev == "local-paused":
                since = t - num(f.get("ms"), 0) / 1000.0
            elif ev == "local-resumed" and since is not None:
                paused.update(range(int(since * 2) - 1, int(t * 2) + 3))
                since = None
        if since is not None:
            paused.update(range(int(since * 2) - 1, int(rx[-1][0] * 2) + 3))
        bins = sorted(set(int(t * 2) for t, _ in rx) - paused)
        ok = sum(1 for b in bins if b in have or b - 1 in have or b + 1 in have)
        share = 100.0 * ok / max(1, len(bins))
        print("    shown: his puppet (or the car copy he sits in) there in %.1f %% of %.0f s with his states arriving" % (
            share, len(bins) / 2.0))
        record("foot.shown", share, who)
    spawns = [(t, s) for t, s in receiver.logs if ("puppet %d spawned" % sid) in s]
    gone = [t for t, s in receiver.logs if ("puppet %d is gone" % sid) in s]
    reasons = [t for t, s in receiver.logs if ("puppet %d removed" % sid) in s or ("puppet %d queued (" % sid) in s
               or "local player in world" in s or "a new world" in s] + gone
    if gone:
        backs = []
        for tg in gone:
            nxt = [t for t, _ in spawns if t >= tg]
            backs.append((nxt[0] - tg) if nxt else (receiver.span()[1] - tg))
        print("    removed by the game %d time(s): back after %s s" % (len(gone), ", ".join("%.1f" % b for b in backs)))
        record("foot.respawn_s", max(backs), who)
    if spawns:
        extra = 0
        for i in range(1, len(spawns)):
            t0, t1 = spawns[i - 1][0], spawns[i][0]
            if not any(t0 <= tr <= t1 for tr in reasons):
                extra += 1
                if extra <= 4:
                    print("    spawned again without a character change or a new world: %s" % receiver.wall(t1))
        print("    puppet spawns: %d (%d without such a reason)" % (len(spawns), extra))
        record("foot.respawns", extra, who)
    dz = []
    for t, f in pup:
        cp = vec(f.get("cp"))
        tp = truth_p.at(to_sender(t))
        if cp and tp and len(cp) > 2 and len(tp) > 2 and f.get("have") == "1" and f.get("busy", "0") == "0":
            dz.append(abs(cp[2] - tp[2]))
    if dz:
        print("    height, puppet vs player on foot: %s" % stats_line(dz))
        record("foot.dz_p95", pct(dz, 95), who)
    if args.csv and rows:
        write_csv(args.csv, "foot-p%d-on-%s.csv" % (sid, receiver.role), [
            "t_receiver", "t_sender", "copy_x", "copy_y", "true_x", "true_y", "true_err", "ctl_err", "gait",
            "player_mode", "fix", "mult", "want", "measured", "carry"], rows)

    # ---- cars ----
    veh = [(t, f) for t, f in receiver.records["VEH"] if num(f.get("id"), -1) == sid]
    cr = Series([(t, f) for t, f in receiver.records["CR"] if f.get("drv") == str(sid)], gap=0.3)
    if veh:
        pinned = sum(1 for _, f in veh if f.get("pin") == "1")
        pins = Counter(WHY.get(int(num(f.get("why"), 0)), "?") for _, f in veh if f.get("jp") == "1")
        retries = sum(1 for _, f in veh if f.get("rt") == "1")
        errs, spd, steer_d, angle_d, rows = [], [], [], [], []
        eng = Counter()
        how = Counter()
        for t, f in veh:
            if f.get("have") != "1":
                continue
            cp = vec(f.get("cp"))
            tc = truth_c.at(to_sender(t))
            e = dist2(cp, tc) if cp and tc else None
            if e is not None:
                errs.append(e)
                worst.append((e, "car", label, receiver.wall(t), sender.wall(to_sender(t)),
                              "%s, %s" % ("pinned" if f.get("pin") == "1" else "own physics", f.get("key"))))
            tsp, csp = num(f.get("tsp"), 0), num(f.get("csp"), 0)
            if tsp > 2:
                spd.append(csp / tsp)
            c = cr.nearest(t, 0.06)
            rb = vec(c.get("rb")) if c else None
            if c:
                eng[c.get("eng", "-")] += 1
                how[c.get("speedHow", "-")] += 1
            if rb:
                steer_d.append(abs(rb[0] - num(f.get("st"), 0)))
                drv = lr_rb.at(to_sender(t))
                if drv and len(drv) >= 6 and len(rb) >= 6:
                    angle_d.append(abs(math.degrees(rb[5] - drv[5])))
            rows.append((t, to_sender(t), cp[0] if cp else "", cp[1] if cp else "", tc[0] if tc else "",
                         tc[1] if tc else "", e if e is not None else "", num(f.get("err"), 0), f.get("pin"),
                         tsp, csp, num(f.get("st"), 0), rb[0] if rb else "", rb[5] if rb and len(rb) >= 6 else ""))
        print("  cars: %d samples, pinned in %.1f %% (pins: %s; %d retries of its physics)" % (
            len(veh), 100.0 * pinned / len(veh), ", ".join("%s %d" % kv for kv in pins.items()) or "none", retries))
        who = "%s on %s" % (sender.role, receiver.role)
        record("car.pinned", 100.0 * pinned / len(veh), who)
        if errs:
            print("    car copy vs the driver's car at the same moment: %s" % stats_line(errs))
            record("car.p95", pct(errs, 95), who)
        if spd:
            print("    copy speed / driver's speed (above 2 m/s): median %.2f, p10 %.2f, p90 %.2f" % (
                pct(spd, 50), pct(spd, 10), pct(spd, 90)))
            record("car.speed_ratio", pct(spd, 50), who)
        if steer_d:
            print("    steering wish on the copy vs the driver's (did SetSteer stick): mean diff %.3f, p95 %.3f" % (
                statistics.fmean(steer_d), pct(steer_d, 95)))
        if angle_d:
            print("    front wheel angle on the copy vs the driver's car: mean diff %.1f deg, p95 %.1f deg" % (
                statistics.fmean(angle_d), pct(angle_d, 95)))
        if eng:
            print("    engine on the copy (CR eng): %s; speed given back after moves as: %s" % (
                dict(eng), dict(how)))
        if args.csv and rows:
            write_csv(args.csv, "car-p%d-on-%s.csv" % (sid, receiver.role), [
                "t_receiver", "t_sender", "copy_x", "copy_y", "true_x", "true_y", "true_err", "ctl_err", "pinned",
                "target_speed", "copy_speed", "driver_steer", "copy_steer_wish", "copy_wheel_angle"], rows)
    else:
        print("  cars: none driven")

    # ---- seats (copied log lines) ----
    ejects = [(t, s) for t, s in receiver.logs if ("puppet %d thrown out" % sid) in s or
              ("puppet %d keeps getting thrown out" % sid) in s]
    if ejects:
        print("  seats: %d thrown-out lines, first ones:" % len(ejects))
        for t, s in ejects[:4]:
            print("    %s %s" % (receiver.wall(t), s.split("sync: ", 1)[-1][:150]))
    # 0.15: the puppet's brain, put-backs inside the car (seat given up while still in it), seat taken by others
    seatnotes = [(t, s) for t, s in receiver.logs if ("puppet %d: friend brain" % sid) in s or
                 ("puppet %d is in car" % sid) in s or ("puppet %d: seat" % sid) in s or
                 ("puppet %d: no friend brain" % sid) in s]
    backs = sum(1 for _, s in seatnotes if ("puppet %d is in car" % sid) in s)
    if seatnotes:
        print("  seat notes: %d (put-backs inside the car: %d), first ones:" % (len(seatnotes), backs))
        for t, s in seatnotes[:4]:
            print("    %s %s" % (receiver.wall(t), s.split("sync: ", 1)[-1][:150]))
    rides = [(t, s) for t, s in receiver.logs if "ride along:" in s]
    for t, s in rides[:4]:
        print("  ride along: %s %s" % (receiver.wall(t), s.split("ride along: ", 1)[-1][:150]))
    probes = [s for _, s in receiver.logs if "probe:" in s or "SetSpeed" in s or "engine was" in s]
    for s in probes[:6]:
        print("  car note: %s" % s.split("sync: ", 1)[-1][:160])


def players(sender, receiver):
    """Package `players` (docs/sync-architecture.md section 8): how the puppet and the car copy of `sender` were driven
    on `receiver`'s game - the new PUP / VEH fields (hint, rest blend, brake slide, lead; pinned time, speed fixes,
    skids) and the parked cars a newcomer got. Traces from before the package have none of it ("older trace")."""
    sid = sender.local
    pup = [f for _, f in receiver.records["PUP"] if num(f.get("id"), -1) == sid and f.get("have") == "1"]
    veh = [f for _, f in receiver.records["VEH"] if num(f.get("id"), -1) == sid and f.get("have") == "1"]
    pks = [f for _, f in sender.records["PKS"] if num(f.get("to"), -1) == receiver.local]
    pkr = [f for _, f in receiver.records["PKR"]]
    if not (pup or veh or pks or pkr):
        return
    section("Driving %s (player %d) on %s's game: puppet and car copy" % (sender.name, sid, receiver.name))
    hinted = [f for f in pup if "hint" in f]
    if pup and not hinted:
        print("  on foot: older trace (no hint / rest / brake fields)")
    elif hinted:
        n = len(hinted)
        same = sum(1 for f in hinted if f.get("hint") == f.get("g"))
        rest = sum(1 for f in hinted if f.get("rest") == "1")
        brk = sum(1 for f in hinted if f.get("brk") == "1")
        stp = sum(1 for f in hinted if f.get("stp") == "1")
        lead = [num(f.get("lead"), 0) for f in hinted]
        print("  on foot: %d samples; commanded gait = his reported gait %.1f %% (the rest: he stands, it walks to his"
              " spot)" % (n, 100.0 * same / n))
        print("    rest blend %.1f %%, brake slide %.1f %%, walking to his stop point %.1f %% of samples; lead median "
              "%.0f ms" % (100.0 * rest / n, 100.0 * brk / n, 100.0 * stp / n, pct(lead, 50)))
    new_veh = [f for f in veh if "pf" in f]
    if veh and not new_veh:
        print("  car: older trace (no pinned-for / speed-fix fields)")
    elif new_veh:
        n = len(new_veh)
        pf = [num(f.get("pf"), 0) for f in new_veh if f.get("pin") == "1"]
        gs = sum(1 for f in new_veh if f.get("gs") == "1")
        skid = sum(1 for f in new_veh if f.get("skid") == "1")
        quiet = sum(1 for f in new_veh if f.get("fix") == "0")
        print("  car: %d samples; nothing set on the copy %.1f %%, given his speed alone %d times, his car sliding "
              "%.1f %%" % (n, 100.0 * quiet / n, gs, 100.0 * skid / n))
        if pf:
            print("    pinned: longest %.1f s (retried after 1.5 s doubling to 8 s)" % max(pf))
    for f in pks:
        print("  parked cars sent to %s on joining: %s in %s message(s)" % (receiver.name, f.get("n"), f.get("msgs")))
    for f in pkr:
        print("  parked cars received from the host: %s, placed %s, destroyed ones left out %s" % (
            f.get("n"), f.get("applied"), f.get("wrecked")))


def npcs(guest):
    ngs = [f for _, f in guest.records["NGS"]]
    if not ngs:
        return
    section("NPCs on %s's game (guest)" % guest.name)
    avg = [num(f.get("avg"), 0) for f in ngs if num(f.get("matched"), 0) > 0]
    mx = [num(f.get("max"), 0) for f in ngs if num(f.get("matched"), 0) > 0]
    live = [num(f.get("live"), 0) for f in ngs]
    matched = [num(f.get("matched"), 0) for f in ngs]
    print("  host NPCs in range: mean %.1f, paired mean %.1f; copies spawned (max) %d" % (
        statistics.fmean(live), statistics.fmean(matched), max(num(f.get("copies"), 0) for f in ngs)))
    if avg:
        print("  distance ours vs the host's (per sample mean): %s" % stats_line(avg))
        print("  worst NPC per sample: %s" % stats_line(mx))
    # (NP: our NPC vs the target the C++ side computed for it, not lined up with the host's NPC in time; the true
    # error and the npc.* targets come from NPT / NPR, npc_replicas())
    far = Counter()
    for _, f in guest.records["NP"]:
        if num(f.get("d"), -1) > 3:
            far[f.get("key")] += 1
    if far:
        print("  NPCs most often more than 3 m off: %s" % ", ".join("%s (%d)" % kv for kv in far.most_common(5)))
    veh = [num(f.get("d"), -1) for _, f in guest.records["NP"] if f.get("car") in ("1", "2") and num(f.get("d"), -1) >= 0]
    foot = [num(f.get("d"), -1) for _, f in guest.records["NP"] if f.get("car") == "0" and num(f.get("d"), -1) >= 0]
    if veh:
        print("  paired host NPCs in a vehicle (car flag or faster than 11 m/s): distance %s" % stats_line(veh))
    if foot:
        print("  paired host NPCs on foot: distance %s" % stats_line(foot))
    last = ngs[-1]
    if "refused" in last:
        print("  driven: %d commands, snaps %d, paired NPCs frozen %d, refused (hidden, a copy shown) %d, unpaired (in a "
              "vehicle, ours on foot) %d, copies with a fallback profile %d" % (
                  num(last.get("drives"), 0), num(last.get("snaps"), 0), num(last.get("frozen"), 0),
                  num(last.get("refused"), 0), num(last.get("unpairs"), 0), num(last.get("fallbacks"), 0)))
    elif "unpairs" in last:
        print("  pulls %d, unpaired (in a vehicle, ours on foot) %d, cops steered %d, copies with a fallback profile %d"
              % (num(last.get("pulls"), 0), num(last.get("unpairs"), 0), num(last.get("steers"), 0),
                 num(last.get("fallbacks"), 0)))


def hermite_at(samples, t, gap):
    """samples: sorted [(t, (x, y, z), (vx, vy))]. Position at t on the Hermite curve through the two samples around it
    (their velocities), None outside or across a gap."""
    ts = [x[0] for x in samples]
    i = bisect.bisect_left(ts, t)
    if i <= 0 or i >= len(samples):
        if samples and abs(samples[0 if i <= 0 else -1][0] - t) < 1e-3:
            return samples[0 if i <= 0 else -1][1]
        return None
    t0, p0, v0 = samples[i - 1]
    t1, p1, v1 = samples[i]
    h = t1 - t0
    if h <= 0 or h > gap:
        return None
    u = (t - t0) / h
    h00, h10, h01, h11 = 2 * u ** 3 - 3 * u ** 2 + 1, u ** 3 - 2 * u ** 2 + u, -2 * u ** 3 + 3 * u ** 2, u ** 3 - u ** 2
    x = h00 * p0[0] + h10 * h * v0[0] + h01 * p1[0] + h11 * h * v1[0]
    y = h00 * p0[1] + h10 * h * v0[1] + h01 * p1[1] + h11 * h * v1[1]
    return (x, y, p0[2] + (p1[2] - p0[2]) * u)


def npc_replicas(authority, receiver, worst):
    """One authority per NPC (docs/sync-trace.md NPT / NPR): each replica on the receiver vs the authority's NPC at the
    same moment. NPT = the authority's NPC as sent (its sample time sT on its steady clock, the clock of TX); NPR = the
    replica where the receiver's game shows it (10 Hz), lined up through the receiver's clock relation to the authority
    (as the player section). npc.p95 / npc.max (host NPCs on a guest, on foot)."""
    aid = authority.local
    npr = [(t, f) for t, f in receiver.records["NPR"] if int(num(f.get("from"), -1)) == aid]
    npt = authority.records["NPT"]
    if not npr and not npt:
        return
    what = "host NPCs on %s's game" % receiver.name if authority.role == "host" else \
        "%s's own NPCs on the host's game" % authority.name
    section("NPC replicas: %s (one authority per NPC)" % what)
    if not npr or not npt:
        print("  %d truth records (NPT) on the authority, %d replica records (NPR) on the receiver: nothing to line up" % (
            len(npt), len(npr)))
        return
    diff, _, _ = clock_relation(receiver, authority, aid)
    if diff is None:
        print("  no player states from the authority in the receiver's trace: the clocks cannot be related")
        return
    truth = defaultdict(list)
    for _, f in npt:
        p, v = vec(f.get("p")), vec(f.get("v"))
        if p and v:
            truth[f.get("key")].append((num(f.get("sT"), 0) / 1000.0, p, v))
    for k in truth:
        truth[k].sort()
    errs, foot, copies, paired, ctl, lag = [], [], [], [], [], []
    fixes = Counter()
    by_key = Counter()
    for t, f in npr:
        cp = vec(f.get("cp"))
        if not cp:
            continue
        ctl.append(num(f.get("err"), 0))
        fixes[f.get("fix", "0")] += 1
        ts = t - diff
        tp = hermite_at(truth.get(f.get("key"), []), ts, 1.2)
        if tp is None:
            continue
        e = dist2(cp, tp)
        errs.append(e)
        (copies if f.get("kind") == "2" else paired).append(e)
        rt = num(f.get("rT"))
        if rt is not None:
            lag.append(ts * 1000.0 - rt)
        if e > 3:
            by_key[f.get("key")] += 1
        worst.append((e, "NPC", "%s NPC %s" % (authority.name, f.get("key")), receiver.wall(t), authority.wall(ts),
                      "kind %s, fix %s" % (f.get("kind"), f.get("why", "-"))))
        foot.append(e)
    print("  %d replica records, %d lined up with the authority's NPC (truth records %d of %d NPCs)" % (
        len(npr), len(errs), len(npt), len(truth)))
    if errs:
        print("  replica vs the authority's NPC at the same moment: %s" % stats_line(errs))
        if paired:
            print("    paired NPCs (driven):  %s" % stats_line(paired))
        if copies:
            print("    spawned copies:        %s" % stats_line(copies))
        who = "%s on %s" % (what.split(" on ")[0], receiver.role)
        record("npc.p95", pct(foot, 95), who)
        record("npc.max", max(foot), who)
    if ctl:
        print("  replica vs its target (controller error): %s" % stats_line(ctl))
    if lag:
        print("  target time behind the true moment (ms; 0 = the lead reaches now): median %.0f, p95 %.0f" % (
            pct(lag, 50), pct(lag, 95)))
    print("  fixes: none %d, nudge %d, snap %d" % (fixes.get("0", 0), fixes.get("1", 0), fixes.get("2", 0)))
    snaps = Counter(f.get("why") for _, f in receiver.records["NPS"] if int(num(f.get("from"), -1)) == aid)
    if snaps:
        print("  snaps by reason: %s" % ", ".join("%s %d" % kv for kv in snaps.most_common()))
    if by_key:
        print("  NPCs most often more than 3 m off: %s" % ", ".join("%s (%d)" % kv for kv in by_key.most_common(5)))


def npc_bandwidth(host):
    """NBW (host, every 10 s): NPC states + infos + leaves sent to each guest (KB/s), the NPCs near him."""
    nbw = host.records["NBW"] if host else []
    if not nbw:
        return
    section("NPC bandwidth per guest (host upload)")
    by = defaultdict(list)
    near = defaultdict(list)
    for _, f in nbw:
        by[f.get("to")].append(num(f.get("kbs"), 0))
        near[f.get("to")].append(num(f.get("near"), 0))
    for g in sorted(by):
        v = by[g]
        print("  to guest %s: median %.2f, max %.2f KB/s; NPCs in his interest median %.0f, max %.0f" % (
            g, pct(v, 50), max(v), pct(near[g], 50), max(near[g])))
        record("npc.bw", pct(v, 50), "to guest %s" % g)


def swapped_cars(tr):
    """Real cars hidden behind a copy on this game and how they came back (SWR events, SWP machine steps)."""
    events = [(t, "SWR", f) for t, f in tr.records["SWR"]] + [(t, "SWP", f) for t, f in tr.records["SWP"]]
    if not events:
        return
    section("Swapped cars on %s's game (%s)" % (tr.name, tr.role))

    def why(f):
        return f.get("why", "").replace("_", " ")

    swaps = sum(1 for _, k, f in events if k == "SWR" and f.get("ev") == "swap")
    back = [f for _, k, f in events if k == "SWR" and f.get("ev") == "restore"]
    gone = sum(1 for _, k, f in events if k == "SWR" and f.get("ev") == "forget")
    print("  %d swapped, %d brought back (%s), %d forgotten" % (
        swaps, len(back), ", ".join("%s: %d" % kv for kv in Counter(why(f) for f in back).items()) or "-", gone))
    for t, k, f in sorted(events, key=lambda e: e[0])[:40]:
        if k == "SWR":
            print("    %s %s %s %s %s" % (tr.wall(t), f.get("key"), f.get("ev"),
                                         (f.get("from") or f.get("at") or "").replace("_", " "), why(f)))
        else:
            print("    %s %s machine: %s, %s (%s)" % (tr.wall(t), f.get("key"), f.get("phase"), f.get("act"), why(f)))


def own_npcs(guest, host):
    """Guest-owned NPCs: the ones only the guest's game had, as the host showed them (NO / NM / NGO / NGE / NMC)."""
    ngo = [f for _, f in guest.records["NGO"]]
    nm = [f for _, f in host.records["NM"]] if host else []
    if not ngo and not nm:
        return
    section("NPCs only %s's game had (guest-owned)" % guest.name)
    owned = sum(1 for f in ngo if f.get("owned") == "1")
    ends = Counter(f.get("why") for _, f in guest.records["NGE"])
    names = {"0": "died", "1": "handed back", "2": "gone", "3": "claim refused"}
    print("  became the guest's own: %d; ends: %s" % (owned, ", ".join(
        "%s %d" % (names.get(k, k), v) for k, v in sorted(ends.items())) or "none"))
    if host:
        print("  host claims sent: %d; mirrors shown with a copy (samples): %d of %d" % (
            len(host.records["NMC"]), sum(1 for f in nm if f.get("loc") not in (None, "nil")), len(nm)))
        d = [num(f.get("d"), -1) for f in nm if num(f.get("d"), -1) >= 0]
        if d:
            print("  distance of the host's copy from the guest's NPC: %s" % stats_line(d))


def _hermite(p0, v0, p1, v1, dt, u):
    u2, u3 = u * u, u * u * u
    h00, h10, h01, h11 = 2 * u3 - 3 * u2 + 1, u3 - 2 * u2 + u, -2 * u3 + 3 * u2, u3 - u2
    return tuple(h00 * a + h10 * dt * va + h01 * b + h11 * dt * vb for a, va, b, vb in zip(p0, v0, p1, v1))


class TruthTrack:
    """An authority's TRS records of one car: where it was at a time on the authority's clock (Hermite between the
    records with their velocities; None outside them or across a gap)."""

    def __init__(self, items, gap=0.6):
        self.items = sorted(items)
        self.t = [i[0] for i in self.items]
        self.gap = gap

    def at(self, t):
        i = bisect.bisect_left(self.t, t)
        if i <= 0 or i >= len(self.t):
            return None
        t0, p0, v0 = self.items[i - 1]
        t1, p1, v1 = self.items[i]
        if t1 - t0 > self.gap or t1 <= t0:
            return None
        return _hermite(p0, v0, p1, v1, t1 - t0, (t - t0) / (t1 - t0))


def _truth(auth_tr):
    """net -> TruthTrack from an authority's TRS records (sT: its state clock, ms)."""
    per = defaultdict(list)
    for _, f in auth_tr.records["TRS"]:
        st, p, v = num(f.get("sT")), vec(f.get("p")), vec(f.get("v"))
        if st is not None and p and v:
            per[f.get("net")].append((st / 1000.0, p, v))
    return {k: TruthTrack(v) for k, v in per.items()}


TRAFFIC_DRIVER_LOD = 40.0  # m: src/game/traffic_model.h kDriverLod


def _traffic_row(t, f, tt, ta, p, cp, me):
    """One TRC row lined up with the authority's car: what the car did then and how the copy stood to it."""
    a, b = tt.at(ta - 0.05), tt.at(ta + 0.05)
    v = ((b[0] - a[0]) / 0.1, (b[1] - a[1]) / 0.1) if a and b else (0.0, 0.0)
    spd = math.hypot(v[0], v[1])
    tgt, cv, pp = vec(f.get("tgt")), vec(f.get("cv")), vec(f.get("pp"))
    row = {"e": dist2(cp, p), "spd": spd, "pin": f.get("pin") == "1", "d": None, "along": None, "across": None,
           "away": False, "pre": dist2(pp, p) if pp else None}
    m = me.at(t) or me.nearest(t, 1.0)
    if m:
        row["d"] = dist2(cp, m)
    if spd > 0.5:
        ux, uy = v[0] / spd, v[1] / spd
        dx, dy = cp[0] - p[0], cp[1] - p[1]
        row["along"], row["across"] = dx * ux + dy * uy, -dx * uy + dy * ux
    if tgt and cv:
        tv = vec(f.get("tv")) or (0, 0, 0)
        cs = math.hypot(cv[0], cv[1])
        ex, ey = tgt[0] - cp[0], tgt[1] - cp[1]
        el = math.hypot(ex, ey)
        # a copy of a standing car that moves away from it (the 0.16.0 copy speed: |target velocity + error| along
        # the copy's heading drove a copy that was past its standing target on, away from it)
        row["away"] = (math.hypot(tv[0], tv[1]) < 0.5 and cs >= 2 and el > 0.3 and
                       (ex * cv[0] + ey * cv[1]) / (cs * el) < -0.5)
    return row


def _traffic_breakdown(rows, recv_name):
    """Where the true error comes from: standing / moving cars, along / across the way the car drives, the distance
    from the receiving player, copies driven off a standing car, and the copy before our commands (TRC pp)."""
    if not rows:
        return
    def p95(v):
        return ("%.2f" % pct(v, 95)) if v else "-"
    stand = [r for r in rows if r["spd"] < 0.5]
    move = [r for r in rows if r["spd"] >= 0.5]
    print("    true error by what the car did: standing p95 %s m (%d), moving p95 %s m (%d); moving along its way p95 "
          "%s m, across p95 %s m; pinned rows %d" % (
              p95([r["e"] for r in stand]), len(stand), p95([r["e"] for r in move]), len(move),
              p95([abs(r["along"]) for r in move if r["along"] is not None]),
              p95([abs(r["across"]) for r in move if r["across"] is not None]), sum(1 for r in rows if r["pin"])))
    bands = []
    for lo, hi in ((0, 30), (30, 90), (90, 1e9)):
        v = [r["e"] for r in rows if r["d"] is not None and lo <= r["d"] < hi]
        if v:
            bands.append("%d%s m p95 %s (%d)" % (lo, ("-%d" % hi) if hi < 1e9 else "+", p95(v), len(v)))
    if bands:
        print("    true error by distance from %s's player: %s" % (recv_name, ", ".join(bands)))
    away = sum(1 for r in stand if r["away"])
    if stand:
        print("    copies of a standing car driven off it (>= 2 m/s away from their target): %d of %d standing rows "
              "(%.0f%%)" % (away, len(stand), 100.0 * away / len(stand)))
    pre = [r["pre"] for r in rows if r["pre"] is not None]
    if pre:
        print("    copy before this tick's commands (TRC pp: the game's own placement) vs the authority's car: %s" %
              stats_line(pre))


def traffic(a, b):
    """Traffic sync by region (docs/sync-trace.md TRS / TRC / TRG / TRA / TRL / TRR / TRH / TRP / TRO): every game is
    the authority for its own traffic and shows the other's; per direction the true error of the copies (TRC on the
    receiver against the authority's TRS at the same moment), what was shown, and the receiver's own cars left
    unshared."""
    if not any(tr.records[k] for tr in (a, b) for k in ("TRG", "TRH", "TRC", "TRS")):
        return
    section("Traffic (every game's cars on the other's game)")
    for auth, recv in ((a, b), (b, a)):
        trh = auth.records["TRH"]
        trc = [f for _, f in recv.records["TRC"] if num(f.get("a"), 0) == auth.local or
               ("a" not in f and auth.role == "host")]  # 0.15 traces: TRC without a= are the host's cars
        trs = auth.records["TRS"]
        if not trh and not trc and not trs:
            continue
        who = "%s's cars on %s's game" % (auth.name, recv.name)
        print("  %s:" % who)
        if len(trh) >= 2:
            (t0, x), (t1, y) = trh[0], trh[-1]
            secs = max(t1 - t0, 1e-6)
            cars = [num(f.get("cars"), 0) for _, f in trh]
            print("    authority: %.0f own traffic cars near other players on average (max %d), %.1f KB/s sent, %d spawn / "
                  "%d state / %d despawn messages, bumps reported to it %d" % (
                      statistics.fmean(cars), max(cars), (num(y.get("bytes"), 0) - num(x.get("bytes"), 0)) / 1024 / secs,
                      num(y.get("spawnMsgs"), 0), num(y.get("stateMsgs"), 0), num(y.get("despawnMsgs"), 0),
                      num(y.get("bumps"), 0)))
            trp = [f for _, f in auth.records["TRP"] if num(f.get("id"), -1) == recv.local]
            if trp:
                print("    authority -> %s: relevant mean %.1f, admitted mean %.1f, states skipped as unchanged %d of "
                      "%d, deferred by the budget (max) %d" % (
                          recv.name, statistics.fmean(num(f.get("relevant"), 0) for f in trp),
                          statistics.fmean(num(f.get("admitted"), 0) for f in trp), num(trp[-1].get("skipped"), 0),
                          num(trp[-1].get("skipped"), 0) + num(trp[-1].get("states"), 0),
                          max(num(f.get("deferred"), 0) for f in trp)))
        # copy vs its target (what the receiver's controller saw)
        errs = [e for e in (num(f.get("err"), 0) for f in trc) if e < 1e6]
        if errs:
            print("    copy vs its target (per sample): %s" % stats_line(errs))
            far = Counter(f.get("net") for f in trc if 3 < num(f.get("err"), 0) < 1e6)
            if far:
                print("    copies most often more than 3 m off: %s" % ", ".join(
                    "net %s (%d)" % kv for kv in far.most_common(5)))
        # the true error: the copy against the authority's car at the same moment (the clocks lined up by the RX
        # offsets), and against the authority's car at the time its target stood for (rT: the playout part left out)
        truth = _truth(auth) if trs else {}
        if trc and truth:
            rel, _, _ = clock_relation(recv, auth, auth.local)
            same, at_rt, rows = [], [], []
            me = Series([(t, vec(f.get("p"))) for t, f in recv.records["TX"]], gap=1.0)
            for t, f in recv.records["TRC"]:
                if num(f.get("a"), 0) != auth.local:
                    continue
                tt, cp = truth.get(f.get("net")), vec(f.get("cp"))
                if not tt or not cp:
                    continue
                if rel is not None:
                    p = tt.at(t - rel)
                    if p:
                        same.append(dist2(cp, p))
                        rows.append(_traffic_row(t, f, tt, t - rel, p, cp, me))
                rt = num(f.get("rT"), -1)
                if rt and rt > 0:
                    p = tt.at(rt / 1000.0)
                    if p:
                        at_rt.append(dist2(cp, p))
            if same:
                print("    copy vs the authority's car at the same moment (true error): %s" % stats_line(same))
                record("traffic.true_p95", pct(same, 95), who)
            elif at_rt:
                print("    (no clock relation: the true error at the target's time rT instead)")
                record("traffic.true_p95", pct(at_rt, 95), who + " (at rT)")
            if at_rt:
                print("    copy vs the authority's car at its target's time rT: %s" % stats_line(at_rt))
            if not same and not at_rt:
                print("    no TRC lined up with a TRS (the copies' cars were not written as truth)")
            _traffic_breakdown(rows, recv.name)
        elif trc:
            print("    no truth records (TRS) on %s's game: the true error cannot be computed" % auth.name)
    # what each receiver showed, its region and its own leftovers
    for recv in (a, b):
        trg = [f for _, f in recv.records["TRG"]]
        if not trg:
            continue
        print("  on %s's game:" % recv.name)
        due = [num(f.get("due"), -1) for f in trg]
        if any(d >= 0 for d in due):
            sd = sum(d for d in due if d > 0)
            ss = sum(num(f.get("shown"), 0) for f in trg if num(f.get("due"), 0) > 0)
            near = sum(1 for d in due if d > 0)
            if sd:
                share = 100.0 * ss / sd
                print("    shown: %.1f%% of the other games' cars within 150 m had a copy that drove (%d samples with "
                      "cars near)" % (share, near))
                record("traffic.shown", share, "on %s's game" % recv.name)
            own = [f for f in trg if num(f.get("mode"), 0) == 0]
            print("    region: our own %.0f%% of the time, another player's %.0f%%" % (
                100.0 * len(own) / len(trg), 100.0 - 100.0 * len(own) / len(trg)))
        else:  # a 0.15 trace: the share of the time the host's traffic showed
            on = [f for f in trg if num(f.get("mode"), 0) == 1]
            print("    host traffic shown %.0f%% of the time (0.15 trace)" % (100.0 * len(on) / len(trg)))
            record("traffic.shown", 100.0 * len(on) / len(trg), "host traffic on %s (0.15 trace)" % recv.name)
        print("    copies mean %.1f (max %d) of %.1f cars of others; pins %d, snaps %d, bumps %d, states %d (old %d)" % (
            statistics.fmean(num(f.get("copies"), 0) for f in trg), max(num(f.get("copies"), 0) for f in trg),
            statistics.fmean(num(f.get("live"), 0) for f in trg), num(trg[-1].get("pins"), 0),
            num(trg[-1].get("snaps"), 0), num(trg[-1].get("bumps"), 0), num(trg[-1].get("states"), 0),
            num(trg[-1].get("old"), 0)))
        for a_id in sorted({f.get("a") for _, f in recv.records["TRA"]}):
            tra = [f for _, f in recv.records["TRA"] if f.get("a") == a_id]
            d = [num(f.get("due"), 0) for f in tra]
            s = [num(f.get("shown"), 0) for f in tra]
            print("    player %s's cars: live mean %.1f, copies mean %.1f, shown %s, playout %.0f ms median" % (
                a_id, statistics.fmean(num(f.get("live"), 0) for f in tra),
                statistics.fmean(num(f.get("copies"), 0) for f in tra),
                ("%.1f%%" % (100.0 * sum(s) / sum(d))) if sum(d) else "-",
                pct([num(f.get("pd"), 0) for f in tra], 50)))
        trr = recv.records["TRR"]
        if trr:
            print("    region changes: %s" % ", ".join(
                "%s at %s (%s m)" % ("own" if f.get("own") == "1" else "player " + f.get("auth", "?"), recv.wall(t),
                                    f.get("d", "?")) for t, f in trr[:8]))
        trl = [f for _, f in recv.records["TRL"]]
        if trl:
            late = [num(f.get("near"), 0) for f in trl if num(f.get("since"), 99) >= 10]
            if "since" in trl[0]:
                print("    own cars within 70 m not shared with a player near them (10 s after a region change): max "
                      "%d; own cars hidden max %d, near us mean %.1f" % (
                          max(late) if late else 0, max(num(f.get("hidden"), 0) for f in trl),
                          statistics.fmean(num(f.get("mine"), 0) for f in trl)))
                if late:
                    record("traffic.unshared_near", max(late), "%s's own cars" % recv.name)
            else:  # a 0.15 trace: the guest's own cars still driving near him while the host's showed
                print("    our own traffic while the host's showed: hidden max %d, still driving near us max %d "
                      "(0.15 trace)" % (max(num(f.get("hidden"), 0) for f in trl),
                                        max(num(f.get("near"), 0) for f in trl)))
                record("traffic.unshared_near", max(num(f.get("near"), 0) for f in trl), "%s's own cars (0.15)" % recv.name)
    # drivers and crews (TRO): an authority's driver reads that failed and came back, a receiver's crews out / in / put
    # back
    for tr in (a, b):
        tro = [f for _, f in tr.records["TRO"]]
        misses = [f for f in tro if f.get("ev") == "miss"]
        if misses or any(f.get("ev") == "back" for f in tro):
            backs = [num(f.get("after"), 0) for f in tro if f.get("ev") == "back"]
            print("  %s (authority): no driver read %d times (%s); read again %d times (after %s s); moving when "
                  "missed %d" % (
                      tr.name, len(misses), ", ".join("%s %d" % kv for kv in Counter(
                          "seat 0 holds another human" if "holds" in f.get("why", "") else
                          f.get("why", "?").replace("_", " ") for f in misses).most_common(4)),
                      len(backs), stats_line(backs, "s") if backs else "-",
                      sum(1 for f in misses if num(f.get("spd"), 0) > 2)))
            dist = [num(f.get("dist"), -1) for f in misses if num(f.get("dist"), -1) >= 0]
            if dist:
                print("  %s: distance from its player when a driver was not read: %s" % (tr.name, stats_line(dist)))
                far = sum(1 for d in dist if d > TRAFFIC_DRIVER_LOD)
                print("  %s: %d of %d beyond %.0f m (the game streams traffic drivers out there and drives the car on: "
                      "a driven car, not driverless%s)" % (tr.name, far, len(dist), TRAFFIC_DRIVER_LOD,
                                                         "" if any("far" in f for f in misses) else
                                                         "; this trace's build still counted them as missing"))
            trh = tr.records["TRH"]
            last = trh[-1][1] if trh else {}
            print("  %s: driverless (confirmed, 1 s slow) %d, driven again %d" % (
                tr.name, num(last.get("orphaned"), 0), num(last.get("driven"), 0)))
        ev = Counter(f.get("ev") for f in tro)
        if ev.get("out") or ev.get("in") or ev.get("reseat"):
            print("  %s (receiver): crews got out %d, back in %d, crew members put back in their seats %d" % (
                tr.name, ev.get("out", 0), ev.get("in", 0), ev.get("reseat", 0)))
        # churn per minute that this game had copies of the others' cars (TRG every 0.5 s)
        trg = [f for _, f in tr.records["TRG"]]
        mins = sum(0.5 for f in trg if num(f.get("copies"), 0) > 0) / 60.0
        if mins >= 0.5:
            outs = [(t, (f.get("a"), f.get("net"))) for t, f in tr.records["TRO"] if f.get("ev") == "out"]
            ins = defaultdict(list)
            for t, f in tr.records["TRO"]:
                if f.get("ev") == "in":
                    ins[(f.get("a"), f.get("net"))].append(t)
            flaps = sum(1 for t, k in outs if any(0 < ti - t < 2 for ti in ins[k]))
            print("  %s (receiver): %.1f min with copies: %.1f crews out / min (%d back in within 2 s), %.1f put back "
                  "/ min" % (tr.name, mins, len(outs) / mins, flaps, ev.get("reseat", 0) / mins))
            record("traffic.crew_exits_per_min", len(outs) / mins, "on %s's game" % tr.name)
            record("traffic.reseats_per_min", ev.get("reseat", 0) / mins, "on %s's game" % tr.name)
        notes = [s for _, s in tr.logs if "traffic: " in s and ("failed" in s or "knocked" in s)]
        for s in notes[:6]:
            print("  note (%s): %s" % (tr.name, s.split("traffic: ", 1)[-1][:160]))


def peds(auth, recv):
    """Pedestrians (docs/sync-trace.md PDT / PDR / PDC / PDS): `auth`'s pedestrians as replicas on `recv`'s game.
    The true error lines up each replica record (PDR rT = the authority clock time its target stands for) with the
    authority's truth records (PDT sT, the same clock) of that pedestrian."""
    pdr = [f for _, f in recv.records["PDR"] if int(num(f.get("from"), -1)) == auth.local]
    pdc = [f for _, f in recv.records["PDC"]]
    if not pdr and not pdc and not auth.records["PDS"]:
        return
    section("Pedestrians (%s's on %s's game)" % (auth.name, recv.name))
    truth = defaultdict(list)
    for _, f in auth.records["PDT"]:
        p, st = vec(f.get("p")), num(f.get("sT"))
        if p is not None and st is not None:
            truth[f.get("net")].append((st / 1000.0, p))
    series = {net: Series(pts, gap=1.0) for net, pts in truth.items()}
    errs, by_how, missing = [], defaultdict(list), 0
    worst = Counter()
    for f in pdr:
        ser = series.get(f.get("net"))
        cp, rt = vec(f.get("cp")), num(f.get("rT"))
        if ser is None or cp is None or rt is None:
            missing += 1
            continue
        tp = ser.at(rt / 1000.0)
        if tp is None:
            missing += 1
            continue
        e = dist2(cp, tp)
        errs.append(e)
        by_how[f.get("how", "?")].append(e)
        if e > 3:
            worst[f.get("net")] += 1
    if errs:
        print("  replica vs the authority's pedestrian at the same moment: %s" % stats_line(errs))
        for how, v in sorted(by_how.items()):
            print("    %-8s %s" % (how, stats_line(v)))
        record("ped.p95", pct(errs, 95), "%s's on %s" % (auth.name, recv.name))
        if worst:
            print("  pedestrians most often more than 3 m off: %s" % ", ".join(
                "net %s (%d)" % kv for kv in worst.most_common(5)))
    if pdr:
        print("  replica records %d, %d without a truth record at that moment (PDT is 1 in 2 states)" % (
            len(pdr), missing))
    if pdc:
        near = sum(num(f.get("near"), 0) for f in pdc)
        rep_n = sum(num(f.get("rep"), 0) for f in pdc)
        if near > 0:
            cov = 100.0 * rep_n / near
            print("  coverage: %.1f%% of the authorities' pedestrians within 40 m had a replica (%d samples); "
                  "hidden max %d, adopted mean %.1f, spawned mean %.1f, spawn failures %d" % (
                      cov, len(pdc), max(num(f.get("hidden"), 0) for f in pdc),
                      statistics.fmean(num(f.get("adopted"), 0) for f in pdc),
                      statistics.fmean(num(f.get("spawned"), 0) for f in pdc), num(pdc[-1].get("fails"), 0)))
            record("ped.coverage", cov, "on %s" % recv.name)
    pds = auth.records["PDS"]
    if len(pds) >= 2:
        (t0, a), (t1, b) = pds[0], pds[-1]
        secs = max(t1 - t0, 1e-6)
        print("  %s as the authority: %.0f ambient humans near players (mean), %.0f his to send, %.2f KB/s sent, "
              "%d states, %d spawns, %d gone, %d deaths, %d kill requests" % (
                  auth.name, statistics.fmean(num(f.get("peds"), 0) for _, f in pds),
                  statistics.fmean(num(f.get("owned"), 0) for _, f in pds),
                  (num(b.get("bytes"), 0) - num(a.get("bytes"), 0)) / 1024 / secs, num(b.get("states"), 0),
                  num(b.get("spawns"), 0), num(b.get("gones"), 0), num(b.get("deaths"), 0),
                  num(b.get("killreq"), 0)))


def car_damage(a, b):
    """Car damage sync (docs/sync-trace.md CDS / CDT / CDR / CDA / CDX / CDQ): what each game sent, received and set,
    and per player car the last state on both games side by side (traffic cars are named by entity on the host and by
    net id on the guest, so they are only listed)."""
    if not any(tr.records[k] for tr in (a, b) for k in ("CDS", "CDT", "CDA")):
        return
    section("Car damage")
    last = {}
    for tr in (a, b):
        sent = Counter(f.get("kind", "?") for _, f in tr.records["CDT"])
        got = Counter(f.get("kind", "?") for _, f in tr.records["CDR"])
        applied = Counter(f.get("why", "?") for _, f in tr.records["CDA"])
        reported = [f for _, f in tr.records["CDX"] if "echo" not in f]
        echoes = Counter(f.get("echo") for _, f in tr.records["CDX"] if "echo" in f)
        print("  %s's game (%s): sent %s; received %s; set on cars %s; own damage reported %d, state asked for %d; "
              "echoes not reported %s" % (
                  tr.name, tr.role, ", ".join("%s %d" % kv for kv in sorted(sent.items())) or "-",
                  ", ".join("%s %d" % kv for kv in sorted(got.items())) or "-",
                  ", ".join("%s %d" % kv for kv in sorted(applied.items())) or "-",
                  len(reported), len(tr.records["CDQ"]),
                  ", ".join("%s %d" % kv for kv in sorted(echoes.items())) or "-"))
        for t, f in tr.records["CDS"]:
            last[(tr.name, f.get("ref"))] = (t, f)
        unread = sorted({f.get("ref") for _, f in tr.records["CDS"] if f.get("read") == "0"})
        if unread:
            print("    cars whose damage could not be read: %s" % ", ".join(unread[:12]))
    refs = sorted({r for (_, r) in last if r and ":" in r})
    if refs:
        print("  player cars, last state (role, damaged parts / parts, worst 0-254, gone, tyres, flags 1 exploded 2 "
              "burning, GetDamage):")
    for r in refs:
        cells = []
        for tr in (a, b):
            e = last.get((tr.name, r))
            if not e:
                cells.append("%s: -" % tr.name)
                continue
            f = e[1]
            cells.append("%s: %s %s/%s w%s g%s t%s f%s gd %s" % (tr.name, f.get("role"), f.get("dmg"), f.get("parts"),
                                                               f.get("worst"), f.get("gone"), f.get("tyres"),
                                                               f.get("flags"), f.get("gd")))
        ea, eb = last.get((a.name, r)), last.get((b.name, r))
        diff = ""
        if ea and eb:
            fa, fb = ea[1], eb[1]
            if (fa.get("tyres"), fa.get("flags"), fa.get("gone")) != (fb.get("tyres"), fb.get("flags"), fb.get("gone")) \
                    or abs(num(fa.get("dmg"), 0) - num(fb.get("dmg"), 0)) > 1:
                diff = "  <- differs"
        print("    %-14s %s%s" % (r, " | ".join(cells), diff))
    for tr in (a, b):
        tc = sorted({r for (n, r) in last if n == tr.name and r and r[0] in "th" and ":" not in r})
        if tc:
            dmg = [r for r in tc if num(last[(tr.name, r)][1].get("dmg"), 0) or last[(tr.name, r)][1].get("tyres") != "0"]
            print("  %s's traffic cars: %d (%d damaged: %s)" % (tr.name, len(tc), len(dmg), ", ".join(dmg[:10]) or "-"))


def car_looks(a, b):
    """Car look sync (docs/sync-trace.md CLS / CLR / CLQ / CLA / CLW / CLM / CLF): the looks each game sent, and per
    copy whether it took the car's look."""
    if not any(tr.records[k] for tr in (a, b) for k in ("CLS", "CLR", "CLA", "CLQ")):
        return
    section("Car looks")
    for tr in (a, b):
        sent = [f for _, f in tr.records["CLS"] if "answer" not in f]
        answers = [f for _, f in tr.records["CLS"] if "answer" in f]
        print("  %s's game (%s): looks sent %d, answers %d, received %d, asked %d, set on copies %d, matching %d, "
              "given up %d" % (tr.name, tr.role, len(sent), len(answers), len(tr.records["CLR"]), len(tr.records["CLQ"]),
                               len(tr.records["CLA"]), len(tr.records["CLM"]), len(tr.records["CLF"])))
        for f in sent[:6]:
            print("    ours %-12s %s" % (f.get("ref"), (f.get("look") or "").replace("|", " ")))
        for _, f in tr.records["CLW"][:6]:
            print("    set on entity %s: %s" % (f.get("ent"), (f.get("res") or "").replace(";", ", ").replace("_", " ")))
        for _, f in tr.records["CLF"][:6]:
            print("    NOT taken: %s entity %s still differs in %s" % (f.get("ref"), f.get("ent"),
                                                                     (f.get("what") or "").replace("|", " ")))


def reconnects(tr):
    """Lost connections and rejoins on this game (RCN, docs/sync-trace.md; docs/protocol.md "Reconnect")."""
    ev = tr.records["RCN"]
    if not ev:
        return
    section("Reconnects on %s's game (%s)" % (tr.name, tr.role))
    lost = None
    for t, f in ev[:40]:
        kind = f.get("ev", "?")
        extra = " ".join("%s=%s" % (k, v) for k, v in f.items() if k != "ev")
        gap = ""
        if kind == "lost":
            lost = t
        elif kind == "back" and lost is not None:
            gap = " (%.1f s without the host)" % (t - lost)
            lost = None
        print("  %s %s %s%s" % (tr.wall(t), kind, extra, gap))
    if lost is not None:
        print("  (not back by the end of the trace)")


def hour_diff(a, b):
    """a - b in hours, the short way round the clock: (-12, 12]."""
    d = (a - b) % 24.0
    return d - 24.0 if d > 12.0 else d


def world(host, guest):
    """Time of day and weather on the guest against the host's at the same moment (WEH on the host, WEG on the guest,
    both stamped with the session clock sT; docs/sync-trace.md, docs/protocol.md "WorldEnv")."""
    weg = guest.records["WEG"] if guest else []
    weh = host.records["WEH"] if host else []
    if not weg and not weh:
        return
    section("World: time of day on %s's game against the host's" % (guest.name if guest else "the guest"))
    if not weg:
        print("  host readings (WEH): %d, no guest records (WEG)" % len(weh))
        return
    hs = []
    for _, f in weh:
        st, t = num(f.get("sT")), num(f.get("t"))
        if st is not None and t is not None:
            hs.append((st, t, f.get("weather")))
    hs.sort()
    hst = [h[0] for h in hs]

    def host_at(st):
        """The host's time (hours) and weather at session ms st, from the two WEH around it (at most 2.5 s apart)."""
        i = bisect.bisect_left(hst, st)
        if 0 < i < len(hs) and hs[i][0] - hs[i - 1][0] <= 2500:
            a, b = hs[i - 1], hs[i]
            k = (st - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
            return (a[1] + hour_diff(b[1], a[1]) * k) % 24.0, a[2]
        if i < len(hs) and abs(hs[i][0] - st) <= 600:
            return hs[i][1], hs[i][2]
        if i > 0 and abs(st - hs[i - 1][0]) <= 600:
            return hs[i - 1][1], hs[i - 1][2]
        return None, None

    # stretches: a gap of over 5 s between guest readings is a new world or a rejoin; the first 10 s of each are the
    # first sync and left out of the target
    errs, own, acts, measured = [], [], Counter(), []
    wmatch = wtotal = from_truth = 0
    seg_start = last_t = None
    for t, f in weg:
        if last_t is None or t - last_t > 5.0:
            seg_start = t
        last_t = t
        acts[f.get("act", "?")] += 1
        e_own = num(f.get("err"))
        gt, st = num(f.get("t")), num(f.get("sT"))
        ht, hw = host_at(st) if (st is not None and hs) else (None, None)
        if ht is not None and gt is not None:
            e = hour_diff(ht, gt) * 60.0
            from_truth += 1
            if hw is not None:
                wtotal += 1
                wmatch += 1 if hw == f.get("weather") else 0
        else:
            e = e_own  # no host trace (or no stamp): the guest's own estimate
        if e is None:
            continue
        if e_own is not None and ht is not None:
            own.append(abs(e_own - e))
        if t - seg_start >= 10.0:
            errs.append(abs(e))
            measured.append((e, t, f))
    print("  guest readings (WEG): %d, host readings (WEH): %d; compared with the host's own reading at the same "
          "session time: %d" % (len(weg), len(weh), from_truth))
    print("  what the guest did: %s" % ", ".join("%s %d" % (k, v) for k, v in sorted(acts.items())))
    sets = [(t, f) for t, f in weg if f.get("act") == "set"]
    for t, f in sets[:10]:
        print("    %s SetTime: ours %s h, host %s h (%s game minutes)" % (guest.wall(t), f.get("t"), f.get("host"),
                                                                       f.get("err")))
    if errs:
        print("  |host - guest| after the first 10 s of each stretch: %s" % stats_line(errs, "game minutes"))
        worst = max(measured, key=lambda m: abs(m[0]))
        print("  worst: %.2f game minutes at %s (act=%s)" % (worst[0], guest.wall(worst[1]), worst[2].get("act")))
        record("world.time_err", max(errs), guest.name)
    else:
        print("  no readings after the first 10 s")
    if own:
        print("  the guest's own estimate against the truth: %s" % stats_line(own, "game minutes"))
    if wtotal:
        print("  weather the same as the host's: %.1f %% of %d readings" % (100.0 * wmatch / wtotal, wtotal))
    rates = [num(f.get("rate")) for _, f in weg if f.get("measured") == "1"]
    rates = [r for r in rates if r]
    if rates:
        print("  the game's rate as the guest measured it: %.5f game hours per second per flow unit (median)" %
              pct(rates, 50))


def joins(guest, host):
    """How long a guest waited for his world after a join or a reconnect (JN, docs/sync-trace.md)."""
    jn = guest.records["JN"] if guest else []
    reports = [(t, f) for t, f in (host.records["JN"] if host else []) if f.get("ev") == "report"]
    if not jn and not reports:
        return
    section("Join and reconnect: %s's world complete" % (guest.name if guest else "the guest"))
    for t, f in jn:
        ev = f.get("ev")
        if ev == "begin":
            print("  %s %s (%s other player(s))" % (guest.wall(t), "back after a reconnect" if f.get("reconnect") == "1"
                                                    else "joined", f.get("others", "?")))
        elif ev == "complete":
            missing = f.get("missing", "-")
            after = num(f.get("after"))
            what = "complete" if missing == "-" else "STILL MISSING " + missing
            none = f.get("none", "-")
            print("  %s %s after %s s%s%s" % (guest.wall(t), what, f.get("after"),
                                             " (late)" if f.get("late") == "1" else "",
                                             "; nothing came for " + none if none != "-" else ""))
            if after is not None and f.get("late") != "1":
                who = "reconnect" if f.get("reconnect") == "1" else "join"
                record("join.complete_s", after, "%s %s" % (guest.name, who))
        else:
            print("    %-10s %-8s after %s s" % (ev, f.get("key", "-"), f.get("after")))
    for t, f in reports:
        print("  host %s: player %s reported complete=%s after %s s, missing %s (%s)" % (
            host.wall(t), f.get("from"), f.get("complete"), f.get("after"), f.get("missing"),
            (f.get("times") or "").replace(",", ", ").replace(":", " ")))


def doors(tr, other):
    """Door changes on this game and what the other game made of them (DR / DL / DA, docs/sync-trace.md)."""
    dr, dl, da = tr.records["DR"], tr.records["DL"], tr.records["DA"]
    if not (dr or dl or da):
        return
    section("Doors on %s's game (%s)" % (tr.name, tr.role))
    count = defaultdict(int)
    for _, f in dr:
        ev = f.get("ev", "?")
        key = ev
        if ev == "local":
            key = "local %s (ours=%s)" % (f.get("verdict", "?"), f.get("ours", "?"))
        elif ev == "rx":
            key = "rx %s" % f.get("verdict", "?")
        elif ev == "apply":
            key = "apply %s" % f.get("why", "?")
        count[key] += 1
    for k in sorted(count):
        print("  %-34s %d" % (k, count[k]))
    hows = defaultdict(int)
    for _, f in dl:
        if f.get("ev") == "change":
            hows["change how=%s" % f.get("how", "?")] += 1
        elif f.get("ev") == "msg":
            hows["door message %s (ours=%s)" % (f.get("kind", "?"), f.get("ours", "?"))] += 1
    for k in sorted(hows):
        print("  %-34s %d" % (k, hows[k]))
    if da:
        bad = [f for _, f in da if f.get("ok") != "1"]
        print("  applied on our doors: %d, with a failed call: %d" % (len(da), len(bad)))
        for f in bad[:10]:
            print("    door %s (%s) calls=%s" % (f.get("id"), f.get("name"), f.get("calls")))
    # what we sent: did the other game get it (same door hash and act)?
    if other is not None:
        theirs = defaultdict(int)
        for _, f in other.records["DR"]:
            if f.get("ev") == "rx":
                theirs[(f.get("door"), f.get("act"))] += 1
        sent = [(t, f) for t, f in dr if f.get("ev") == "tx"]
        got = 0
        for _, f in sent:
            k = (f.get("door"), f.get("act"))
            if theirs.get(k, 0) > 0:
                theirs[k] -= 1
                got += 1
        if sent:
            print("  sent %d door changes, %d of them arrived on the other game (by door and act)" % (len(sent), got))
    for t, f in [r for r in dr if r[1].get("ev") in ("tx", "rx", "fix", "snapreq", "snaptx", "snaprx", "unmet")][:30]:
        extra = " ".join("%s=%s" % (k, v) for k, v in f.items() if k != "ev")
        print("  %s %s %s" % (tr.wall(t), f.get("ev"), extra))


def latency(traces):
    """LAT records (game/latency.h): per receiving game and system, send -> applied (apply), of it on the wire and in
    the queues until the handler (age) and the hand-off to the script tick (hand). Streams are sampled 1 in 4.
    held=1 records (the system's tick had stopped: a guest in the menus / loading) are left out and listed apart."""
    section("Latency (sender's send -> applied here; session clock)")
    any_rec = False
    for tr in traces:
        by = defaultdict(lambda: ([], [], []))
        held = defaultdict(list)
        for _, f in tr.records.get("LAT", []):
            a, g, h = num(f.get("apply")), num(f.get("age")), num(f.get("hand"))
            if a is None:
                continue
            if f.get("held") == "1":
                held[f.get("sys", "?")].append(a)
                continue
            ap, ag, ha = by[f.get("sys", "?")]
            ap.append(a)
            ag.append(g if g is not None else a)
            ha.append(h if h is not None else 0.0)
        if not by and not held:
            continue
        any_rec = True
        print("  on %s's game (%s):" % (tr.name, tr.role))
        if held:
            print("    waited for the game's tick (menus / loading; left out below): " + ", ".join(
                "%s %d (max %.1f s)" % (s, len(v), max(v) / 1000.0) for s, v in sorted(held.items())))
        print("    %-14s %7s %8s %8s %8s   %-24s %s" % ("system", "msgs", "p50", "p95", "max", "of it on arrival p50/p95",
                                                     "hand-off p50/p95"))
        for sys_name in sorted(by, key=lambda s: -pct(by[s][0], 50)):
            ap, ag, ha = by[sys_name]
            if sys_name == "player":
                record("lat.player.p95", pct(ap, 95), "on %s" % tr.role)
            print("    %-14s %7d %6.1f ms %6.1f ms %6.1f ms   %6.1f / %6.1f ms        %5.1f / %5.1f ms" % (
                sys_name, len(ap), pct(ap, 50), pct(ap, 95), max(ap), pct(ag, 50), pct(ag, 95), pct(ha, 50),
                pct(ha, 95)))
    if not any_rec:
        print("  no LAT records (a build before the latency trace, or the clocks never synced)")


MSG_RE = re.compile(r"(\S+) out ([\d.]+) KB/s [\d.]+/s, in ([\d.]+) KB/s [\d.]+/s([^;]*)")


def msg_system(name):
    """Message name (module.cpp MessageName) -> system, as in game/latency.h."""
    n = name
    if n in ("PlayerState", "CarParked", "PlayerModel"):
        return "player"
    if n in ("WorldEnv", "MissionState", "HostSave", "Cutscene", "MissionFail", "FailVerdict"):
        return "world"
    if n in ("NpcStates", "GuestNpcStates", "NpcStates2", "GuestNpcStates2"):
        return "npc"
    if n.startswith("Npc") or n in ("GuestNpcEnd", "HostNpcHit"):
        return "npc-event"
    if n in ("TrafficStates", "TrafficStates2"):
        return "traffic"
    if n.startswith("Traffic"):
        return "traffic-event"
    if n.startswith("Door"):
        return "door"
    if n in ("Explosion", "CarExploded") or n.startswith("Throw") or n.startswith("0x024"):
        return "explosion"
    if n.startswith("CarDamage"):
        return "car-damage"
    if n.startswith("Police"):
        return "police"
    if n == "ItemStates":
        return "item"
    if n.startswith("Item") or n == "Collectible":
        return "item-event"
    if n == "Hit":
        return "hit"
    if n == "PedStates":
        return "ped"
    if n.startswith("Ped"):
        return "ped-event"
    return n


def bandwidth(traces):
    section("Bandwidth per system (NetMonitor reports, KB/s)")
    any_rec = False
    for tr in traces:
        rows = defaultdict(lambda: ([], [], 0, 0))  # system -> (out per report, in per report, thinned, deferred)
        reports = 0
        totals = []  # all systems out, per report
        for _, text in tr.logs:
            if "net: messages: " not in text:
                continue
            reports += 1
            per = defaultdict(lambda: [0.0, 0.0])
            for m in MSG_RE.finditer(text.split("net: messages: ", 1)[1]):
                s = msg_system(m.group(1))
                per[s][0] += float(m.group(2))
                per[s][1] += float(m.group(3))
                extra = m.group(4)
                th = re.search(r"thinned (\d+)", extra)
                de = re.search(r"deferred (\d+)", extra)
                o, i, t0, d0 = rows[s]
                rows[s] = (o, i, t0 + (int(th.group(1)) if th else 0), d0 + (int(de.group(1)) if de else 0))
            totals.append(sum(v[0] for v in per.values()))
            for s, (o, i) in per.items():
                rows[s][0].append(o)
                rows[s][1].append(i)
        if not rows:
            continue
        any_rec = True
        print("  %s's game (%s), %d reports:" % (tr.name, tr.role, reports))
        if tr.role == "host" and totals:
            record("bw.host_up", pct(totals, 50), "%s, %d player(s)" % (tr.name, len(tr.players)))
        print("    %-14s %16s %16s %10s %10s" % ("system", "out median/max", "in median/max", "thinned", "deferred"))
        for s in sorted(rows, key=lambda k: -(max(rows[k][0] or [0]) + max(rows[k][1] or [0]))):
            o, i, th, de = rows[s]
            o = o + [0.0] * (reports - len(o))  # reports without this system: 0
            i = i + [0.0] * (reports - len(i))
            print("    %-14s %7.2f / %6.2f %7.2f / %6.2f %10d %10d" % (s, pct(o, 50), max(o), pct(i, 50), max(i), th, de))
    if not any_rec:
        print("  no NetMonitor lines in the traces (net_log_interval 0, or the log copy was off)")


def _secs(hms):
    try:
        h, m, s = hms.split(":")
        return int(h) * 3600 + int(m) * 60 + float(s)
    except ValueError:
        return 0.0


def write_csv(d, name, header, rows):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)
    print("    -> %s" % os.path.join(d, name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("traces", nargs=2)
    ap.add_argument("--csv", help="write the lined-up time series to this folder")
    ap.add_argument("--worst", type=int, default=10, help="how many worst moments to list (default 10)")
    ap.add_argument("--check", action="store_true",
                    help="compare with the sync targets (docs/sync-architecture.md section 4); exit 1 on a failure")
    args = ap.parse_args()
    a, b = Trace(args.traces[0]), Trace(args.traces[1])
    for x, y in ((a, b), (b, a)):  # a game names the other players; its own name comes from the other trace
        if x.local not in x.players and x.local in y.players:
            x.name = y.players[x.local]
    print("Sync comparison")
    print("===============")
    for tr in (a, b):
        t0, t1 = tr.span()
        h = tr.header
        print("%s: %s, player %d (%s), mod %s, core %s, %s Hz, %s ms interpolation, %.0f s from %s" % (
            os.path.basename(tr.path), tr.role, tr.local, tr.name, h.get("mod"), h.get("core"), h.get("hz"),
            h.get("interp"), t1 - t0, tr.wall(t0)))
    if a.header.get("mod") != b.header.get("mod"):
        print("WARNING: the two games ran different mod versions")
    worst = []
    compare(a, b, args, worst)
    compare(b, a, args, worst)
    players(a, b)
    players(b, a)
    guest = a if a.role == "guest" else b if b.role == "guest" else None
    if guest:
        npcs(guest)
        own_npcs(guest, b if guest is a else a)
        other = b if guest is a else a
        npc_replicas(other, guest, worst)  # the host's NPCs on the guest
        npc_replicas(guest, other, worst)  # the guest's own NPCs on the host
    for tr in (a, b):
        swapped_cars(tr)
    for tr in (a, b):
        reconnects(tr)
    for x, y in ((a, b), (b, a)):
        doors(x, y)
    host = a if a.role == "host" else b if b.role == "host" else None
    world(host, guest)
    joins(guest, host)
    traffic(a, b)
    for x, y in ((a, b), (b, a)):
        peds(x, y)
    npc_bandwidth(host)
    car_damage(a, b)
    car_looks(a, b)
    bandwidth((a, b))
    latency((a, b))
    if worst:
        section("Worst moments (at least a second apart)")
        shown = []
        for e, kind, label, wr, ws, note in sorted(worst, reverse=True):
            sec = (label, wr[:8])
            if any(sec[0] == s0 and abs(_secs(sec[1]) - _secs(s1)) < 1.0 for s0, s1 in shown):
                continue
            shown.append(sec)
            print("  %6.2f m  %-7s %s  (receiver %s, sender %s) %s" % (e, kind, label.split(" on ")[0], wr, ws, note))
            if len(shown) >= args.worst:
                break
    if args.check:
        return 1 if check() else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
