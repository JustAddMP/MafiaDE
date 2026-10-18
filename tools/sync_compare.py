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
- the worst moments with the wall-clock time of both games, to look them up in a video or the logs

--csv DIR writes the lined-up time series (one file per player and kind) for plotting.
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
    for t, f in pup:
        if f.get("have") != "1":
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
    if errs_true:
        print("    puppet vs the player at the same moment: %s" % stats_line(errs_true))
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
        if errs:
            print("    car copy vs the driver's car at the same moment: %s" % stats_line(errs))
        if spd:
            print("    copy speed / driver's speed (above 2 m/s): median %.2f, p10 %.2f, p90 %.2f" % (
                pct(spd, 50), pct(spd, 10), pct(spd, 90)))
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
    probes = [s for _, s in receiver.logs if "probe:" in s or "SetSpeed" in s or "engine was" in s]
    for s in probes[:6]:
        print("  car note: %s" % s.split("sync: ", 1)[-1][:160])


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
    far = Counter()
    for _, f in guest.records["NP"]:
        if num(f.get("d"), -1) > 3:
            far[f.get("key")] += 1
    if far:
        print("  NPCs most often more than 3 m off: %s" % ", ".join("%s (%d)" % kv for kv in far.most_common(5)))


def traffic(host, guest):
    """Traffic sync (docs/sync-trace.md TRH / TRP / TRG / TRC / TRL)."""
    trg = [f for _, f in guest.records["TRG"]] if guest else []
    trh = host.records["TRH"] if host else []
    if not trg and not trh:
        return
    section("Traffic (the host's cars on %s's game)" % (guest.name if guest else "the guest"))
    if len(trh) >= 2:
        (t0, a), (t1, b) = trh[0], trh[-1]
        secs = max(t1 - t0, 1e-6)
        cars = [num(f.get("cars"), 0) for _, f in trh]
        print("  host: %.0f traffic cars near guests on average (max %d), %.1f KB/s sent, %d spawn / %d state / %d "
              "despawn messages, bumps reported %d" % (
                  statistics.fmean(cars), max(cars), (num(b.get("bytes"), 0) - num(a.get("bytes"), 0)) / 1024 / secs,
                  num(b.get("spawnMsgs"), 0), num(b.get("stateMsgs"), 0), num(b.get("despawnMsgs"), 0),
                  num(b.get("bumps"), 0)))
        trp = [f for _, f in host.records["TRP"]]
        if trp:
            print("  host per guest: relevant mean %.1f, admitted mean %.1f, states skipped as unchanged %d of %d, "
                  "deferred by the budget (max) %d" % (
                      statistics.fmean(num(f.get("relevant"), 0) for f in trp),
                      statistics.fmean(num(f.get("admitted"), 0) for f in trp), num(trp[-1].get("skipped"), 0),
                      num(trp[-1].get("skipped"), 0) + num(trp[-1].get("states"), 0),
                      max(num(f.get("deferred"), 0) for f in trp)))
    if trg:
        on = [f for f in trg if num(f.get("mode"), 0) == 1]
        print("  guest: host traffic shown %.0f%% of the time; copies mean %.1f (max %d) of %.1f host cars" % (
            100.0 * len(on) / len(trg), statistics.fmean(num(f.get("copies"), 0) for f in trg),
            max(num(f.get("copies"), 0) for f in trg), statistics.fmean(num(f.get("live"), 0) for f in trg)))
        last = trg[-1]
        print("  guest: pins %d, snaps %d, bumps %d, states received %d (dropped as old %d)" % (
            num(last.get("pins"), 0), num(last.get("snaps"), 0), num(last.get("bumps"), 0),
            num(last.get("states"), 0), num(last.get("old"), 0)))
    errs = [num(f.get("err"), 0) for _, f in guest.records["TRC"]] if guest else []
    errs = [e for e in errs if e < 1e6]
    if errs:
        print("  copy vs host car (target, per sample): %s" % stats_line(errs))
        far = Counter()
        for _, f in guest.records["TRC"]:
            if 3 < num(f.get("err"), 0) < 1e6:
                far[f.get("net")] += 1
        if far:
            print("  copies most often more than 3 m off: %s" % ", ".join("net %s (%d)" % kv for kv in far.most_common(5)))
    trl = [f for _, f in guest.records["TRL"]] if guest else []
    if trl:
        print("  our own traffic while the host's showed: hidden max %d, still driving near us max %d" % (
            max(num(f.get("hidden"), 0) for f in trl), max(num(f.get("near"), 0) for f in trl)))
    notes = [s for _, s in guest.logs if "traffic: " in s and ("failed" in s or "knocked" in s)] if guest else []
    for s in notes[:6]:
        print("  note: %s" % s.split("traffic: ", 1)[-1][:160])


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
    guest = a if a.role == "guest" else b if b.role == "guest" else None
    if guest:
        npcs(guest)
    host = a if a.role == "host" else b if b.role == "host" else None
    traffic(host, guest)
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
