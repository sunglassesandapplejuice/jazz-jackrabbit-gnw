#!/usr/bin/env python3
"""Trim the menu prefix from a gwemu timeline recording and rebase it.

A recording made with ``GNW_TIMELINE_RECORD=rec.tl`` contains the menu
navigation, the planet intro and the level.  The harness starts a level
directly (``-l world:level``), so the menu part has to go.  The output is
still a normal gwemu timeline, and ``harness/build/jj_host -T`` reads it
directly.

The cut point is the press that confirms the level (the A tap on the
difficulty screen).  That press is kept and moved to 0.5 s, so the
harness's difficulty screen gets it, and the recorded planet intro and
gameplay keep their original relative timing.  ``-l`` still shows the
difficulty selector, so this is what makes the replay line up.

Usage:
    # cut at the A press before the planet-intro gap
    python3 tools/timeline_trim.py rec.tl --auto -o level1.tl

    # cut at an explicit guest time (the A tap on the difficulty screen)
    python3 tools/timeline_trim.py rec.tl --from 41.200 -o level1.tl

    # stop before the first pause-menu press
    python3 tools/timeline_trim.py rec.tl --auto --to 357.419 -o level1.tl

    # inspect the recording to pick a cut point / event index
    python3 tools/timeline_trim.py rec.tl --list

    # replay in the harness
    harness/build/jj_host Jazz.jj -l 0:0 -T level1.tl -t 300000

``--auto`` looks at quiet gaps in the first ``--window`` seconds and cuts
at the A press *before* the gap.  It prefers an A press whose gap is
followed by real gameplay (different direction buttons), so a long idle
in the menu does not win over the planet intro.  If the player idled
during the intro or paused, use ``--list`` and an explicit ``--from``.

Pause-menu time is not recorded the same way as gameplay: the port
subtracts menu time, so a replay with pause presses desyncs.  Either do
not pause while recording, or drop everything from the first ``pause``
press with ``--to`` (``--list`` shows the timestamp).

The difficulty selector starts on MEDIUM.  If the recording used another
difficulty, pass ``--difficulty easy|hard|turbo`` and the trimmer inserts
the UP/DOWN taps before the start press.  Use ``--no-start`` when
replaying from a savestate (``-L``) or a normal boot instead of ``-l``;
then the timestamps are simply rebased to 0, so pass ``--from`` at the
first gameplay input rather than at the level-start press.
"""
import argparse
import sys

# The level-start press is moved here; each extra difficulty tap adds 0.2 s.
START_AT = 0.5
TAP = 0.1
GAP = 0.2


def parse_time(s):
    """Parse "SS.fff" or "[MM:]SS.fff" into seconds."""
    if ":" in s:
        minutes, rest = s.split(":", 1)
        return int(minutes) * 60.0 + float(rest)
    return float(s)


class Event:
    __slots__ = ("t", "act", "args", "lineno")

    def __init__(self, t, act, args, lineno):
        self.t = t
        self.act = act
        self.args = args
        self.lineno = lineno


def read_timeline(path):
    events = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            body = line.split("#", 1)[0].strip()
            if not body:
                continue
            toks = body.split()
            if len(toks) < 2:
                raise ValueError("%s:%d: expected '<time> <action>'" %
                                 (path, lineno))
            t = parse_time(toks[0])
            events.append(Event(t, toks[1], toks[2:], lineno))
    return events


def fmt_time(t):
    return "%.3f" % t


# Buttons that are menu/navigation, not proof that gameplay has started.
NON_GAMEPLAY_BUTTONS = {"a", "start", "select", "pause", "pwr", "game"}


def is_a_press(ev):
    return ev.act == "down" and any(a.lower() == "a" for a in ev.args)


def looks_like_gameplay(events, start_idx, seconds=5.0):
    """True if the input after a quiet gap looks like play, not a menu.

    Menu navigation is mostly A presses.  Gameplay uses at least two
    different direction/action buttons within a few seconds.
    """
    t0 = events[start_idx].t
    buttons = set()
    for ev in events[start_idx:]:
        if ev.t > t0 + seconds:
            break
        if ev.act != "down":
            continue
        for a in ev.args:
            b = a.lower()
            if b not in NON_GAMEPLAY_BUTTONS:
                buttons.add(b)
    return len(buttons) >= 2


def find_auto_cut(events, window, min_gap):
    """Return (cut_time, gap, index) for the best quiet gap in the window.

    The cut is the press *before* the gap: that is the level-start press
    (or the last intro skip), and everything after it is kept.  A press
    is recorded as a ``down`` line followed by a ``release`` line, so if
    the event before the gap is the release, walk back to its down.

    A long idle in the menu can be bigger than the planet intro, so
    prefer the first A press whose quiet gap is followed by gameplay
    (different direction buttons), then the largest A gap, then the
    largest gap of any kind.
    """
    candidates = []
    for i in range(1, len(events)):
        if events[i].t > window:
            break
        gap = events[i].t - events[i - 1].t
        if gap < min_gap:
            continue
        cut_idx = i - 1
        if events[cut_idx].act == "release" and events[cut_idx].args:
            button = events[cut_idx].args[0].lower()
            for j in range(cut_idx - 1, -1, -1):
                ev = events[j]
                if ev.act == "down" and button in \
                        [a.lower() for a in ev.args]:
                    cut_idx = j
                    break
        candidates.append((gap, cut_idx, i))
    if not candidates:
        return 0.0, 0.0, None
    a_candidates = [c for c in candidates if is_a_press(events[c[1]])]
    gameplay = [c for c in a_candidates if looks_like_gameplay(events, c[2])]
    if gameplay:
        gap, cut_idx, _ = gameplay[0]
    elif a_candidates:
        gap, cut_idx, _ = max(a_candidates)
    else:
        gap, cut_idx, _ = max(candidates)
    return events[cut_idx].t, gap, cut_idx


def difficulty_taps(difficulty):
    if difficulty == "easy":
        return ["up"]
    if difficulty == "hard":
        return ["down"]
    if difficulty == "turbo":
        return ["down", "down"]
    return []


def main(argv):
    ap = argparse.ArgumentParser(
        description="Trim a gwemu timeline recording to its gameplay part.")
    ap.add_argument("input", help="recorded timeline (GNW_TIMELINE_RECORD)")
    ap.add_argument("-o", "--output", help="output file (default: stdout)")
    ap.add_argument("--from", dest="from_time", metavar="TIME",
                    help="cut at this guest time (SS.fff or MM:SS.fff)")
    ap.add_argument("--to", dest="to_time", metavar="TIME",
                    help="drop events at or after this guest time "
                         "(e.g. the first pause-menu press)")
    ap.add_argument("--from-event", type=int, metavar="N",
                    help="cut at event index N (see --list)")
    ap.add_argument("--auto", action="store_true",
                    help="cut at the A press before the planet-intro gap")
    ap.add_argument("--marker", metavar="BUTTON",
                    help="cut at the last '<time> down BUTTON' event")
    ap.add_argument("--window", type=float, default=120.0, metavar="SECONDS",
                    help="--auto: only look at the first SECONDS (default 120)")
    ap.add_argument("--min-gap", type=float, default=1.0, metavar="SECONDS",
                    help="--auto: require a gap of at least SECONDS (default 1)")
    ap.add_argument("--no-start", action="store_true",
                    help="do not move the start press to 0.5 s; rebase to 0")
    ap.add_argument("--difficulty", default="medium",
                    choices=("easy", "medium", "hard", "turbo"),
                    help="difficulty selected in the recording (default medium)")
    ap.add_argument("--list", action="store_true",
                    help="print the events and exit")
    args = ap.parse_args(argv)

    events = read_timeline(args.input)

    if args.list:
        prev = None
        for i, ev in enumerate(events):
            delta = "" if prev is None else "  +%.3f" % (ev.t - prev)
            print("%5d  %10s  %-10s %s%s" %
                  (i, fmt_time(ev.t), ev.act, " ".join(ev.args), delta))
            prev = ev.t
        return 0

    if args.from_event is not None:
        if not 0 <= args.from_event < len(events):
            ap.error("--from-event out of range")
        cut = events[args.from_event].t
    elif args.marker:
        cut = None
        for ev in events:
            if ev.act == "down" and args.marker.lower() in \
                    [a.lower() for a in ev.args]:
                cut = ev.t
        if cut is None:
            ap.error("marker button %r not found" % args.marker)
    elif args.from_time is not None:
        cut = parse_time(args.from_time)
    elif args.auto:
        cut, gap, index = find_auto_cut(events, args.window, args.min_gap)
        if index is None:
            print("timeline_trim: no gap >= %.3f s in the first %.3f s; "
                  "not trimming" % (args.min_gap, args.window),
                  file=sys.stderr)
        else:
            ev = events[index]
            print("timeline_trim: cutting at %.3f s (event %d: %s %s, "
                  "%.3f s quiet gap after it)" %
                  (cut, index, ev.act, " ".join(ev.args), gap),
                  file=sys.stderr)
            if ev.act != "down" or \
                    not any(a.lower() == "a" for a in ev.args):
                print("timeline_trim: warning: cut event is not an A press; "
                      "check with --list or use --from", file=sys.stderr)
    else:
        cut = 0.0

    taps = difficulty_taps(args.difficulty)
    start_at = START_AT + len(taps) * (TAP + GAP)
    shift = 0.0 if args.no_start else start_at
    to_time = parse_time(args.to_time) if args.to_time is not None else None

    out = []
    out.append("# trimmed from %s at %.3f s\n" % (args.input, cut))
    if to_time is not None:
        out.append("# dropped events at or after %.3f s\n" % to_time)
    if not args.no_start:
        out.append("# level-start press moved to %.3f s (difficulty %s)\n" %
                   (start_at, args.difficulty))
        t = START_AT
        for button in taps:
            out.append("%s down %s\n" % (fmt_time(t), button))
            out.append("%s release %s\n" % (fmt_time(t + TAP), button))
            t += TAP + GAP

    kept = 0
    for ev in events:
        if ev.t + 1e-9 < cut:
            continue
        if to_time is not None and ev.t + 1e-9 >= to_time:
            continue
        kept += 1
        if ev.act == "quit":
            out.append("%s quit\n" % fmt_time(ev.t - cut + shift))
            continue
        args_text = " ".join(ev.args)
        out.append("%s %s%s\n" %
                   (fmt_time(ev.t - cut + shift), ev.act,
                    (" " + args_text) if args_text else ""))

    text = "".join(out)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
        print("timeline_trim: %d/%d events -> %s (level-start press at %.3f s)" %
              (kept, len(events), args.output,
               0.0 if args.no_start else start_at), file=sys.stderr)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
