#!/usr/bin/env python3
"""Function, line and call-site attribution from a callgrind.out file.

References: the valgrind manual, "The Callgrind Format". Complements
`callgrind_annotate --auto=yes` (full annotated source listing) with a compact
hot-spot view.
"""

import argparse
import os
import re
import sys


class Parser:
    """Streaming parser for one callgrind.out file (single- or multi-part)."""

    def __init__(self):
        self.events = []
        self.totals = [0] * 0
        self.fn_self = {}  # (file, name) -> [cost]
        self.fn_ob = {}  # (file, name) -> object basename (the .so / binary)
        self.line_cost = {}  # (file, name, line) -> [cost]
        self.call_sites = {}  # (file, name, line) -> {target: [count, ir]}

    def parse(self, path):
        fl_ids, fn_ids, ob_ids = {}, {}, {}  # compressed string tables
        fl = fn = ob = ""
        fl_stack = []
        pos = 0
        cfn = "?"
        call = None  # (count, target) pending the caller-side cost line
        events = None
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                key = line.split("=", 1)[0]
                if line.startswith("events:"):
                    events = line.split(":", 1)[1].split()
                    if not self.events:
                        self.events = events
                        self.totals = [0] * len(events)
                    continue
                if line.startswith("summary:"):
                    cost = [int(x) for x in line.split(":", 1)[1].split()]
                    self.totals = [t + c for t, c in zip(self.totals, cost)]
                    continue
                if key == "fl":
                    sid, name = self._def(line, fl_ids)
                    fl = name if name is not None else fl_ids.get(sid, fl)
                    continue
                if key == "fi":
                    sid, name = self._def(line, fl_ids)
                    fl_stack.append(fl)
                    fl = name if name is not None else fl_ids.get(sid, fl)
                    continue
                if key == "fe":
                    fl = fl_stack.pop() if fl_stack else fl
                    continue
                if key == "fn":
                    sid, name = self._def(line, fn_ids)
                    fn = name if name is not None else fn_ids.get(sid, fn)
                    pos = 0
                    continue
                if key in ("cfl", "cfi"):
                    # cfl/cfi carry the call target's FILE and define its id in
                    # the same table fl= draws from (shared string table)
                    sid, name = self._def(line, fl_ids)
                    if name is not None and sid is not None:
                        fl_ids[sid] = name
                    continue
                if key == "ob":
                    # object (the .so or binary) the current function lives in
                    sid, name = self._def(line, ob_ids)
                    ob = name if name is not None else ob_ids.get(sid, ob)
                    continue
                if key == "cob":
                    # call target's object; shares the ob string table
                    self._def(line, ob_ids)
                    continue
                if key == "cfn":
                    sid, name = self._def(line, fn_ids)
                    cfn = name if name is not None else fn_ids.get(sid, cfn)
                    continue
                if key == "calls":
                    n, _tgt = line.split("=", 1)[1].split()[:2]
                    call = (int(n), cfn)
                    continue
                # anything else non-numeric (jump/bb/...): ignore
                if not (line[0].isdigit() or line[0] in "+-*"):
                    continue

                # cost line: [position] cost...
                toks = line.split()
                if toks[0] == "*":
                    toks = toks[1:]
                elif toks[0].startswith("+"):
                    pos += int(toks[0][1:])
                    toks = toks[1:]
                elif toks[0].startswith("-"):
                    pos -= int(toks[0][1:])
                    toks = toks[1:]
                elif toks[0].isdigit() and len(toks) > len(self.events):
                    pos = int(toks[0])
                    toks = toks[1:]
                elif toks[0].isdigit() and len(toks) - 1 <= len(self.events):
                    # absolute position (callgrind default marks it explicitly);
                    # a bare continuation would be indistinguishable here, and
                    # callgrind never emits one with compress-positions=yes
                    pos = int(toks[0])
                    toks = toks[1:]
                cost = [int(x) for x in toks]
                cost += [0] * (len(self.events) - len(cost))
                key3 = (fl, fn, pos)
                if call is not None:
                    sites = self.call_sites.setdefault(key3, {})
                    ent = sites.setdefault(call[1], [0, 0])
                    ent[0] += call[0]
                    ent[1] += cost[0]
                    call = None
                else:
                    self.fn_self.setdefault((fl, fn), [0] * len(self.events))
                    self.fn_ob.setdefault((fl, fn), os.path.basename(ob))
                    cur = self.fn_self[(fl, fn)]
                    for i, c in enumerate(cost):
                        cur[i] += c
                    self.line_cost.setdefault(key3, [0] * len(self.events))
                    cur = self.line_cost[key3]
                    for i, c in enumerate(cost):
                        cur[i] += c
        return self

    @staticmethod
    def _def(line, table):
        body = line.split("=", 1)[1]
        m = re.match(r"\((\d+)\)\s*(.*)", body)
        if m:
            sid, name = int(m.group(1)), m.group(2).strip()
            if name:
                table[sid] = name
                return sid, name
            return sid, None
        return None, body.strip()


def _fmt(n):
    return f"{n:,}"


def main():
    ap = argparse.ArgumentParser(
        description="Hot-spot attribution from a callgrind.out file.",
        epilog="Examples: python3 tools/prof/hotspots.py /tmp/callgrind.out --sort D1mr --lines 5"
        " | --callers expf",
    )
    ap.add_argument("outfile", nargs="?", help="callgrind.out path")
    ap.add_argument("--top", type=int, default=12, help="functions shown (default 12)")
    ap.add_argument("--lines", type=int, default=5, help="hot lines per file (default 5)")
    ap.add_argument("--calls", type=int, default=10, help="call sites shown (default 10)")
    ap.add_argument("--sort", default="Ir", help="event to sort by (default Ir)")
    ap.add_argument(
        "--callers",
        metavar="NAME",
        help="print the callers of every function whose name contains NAME, then exit",
    )
    ap.add_argument("--selftest", action="store_true", help="run the parser selftest")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(_selftest())
    if not args.outfile:
        ap.error("missing callgrind.out path")

    p = Parser().parse(args.outfile)
    ei = p.events.index(args.sort)
    tot = p.totals[ei] or 1

    print("totals: " + "  ".join(f"{e}={_fmt(c)}" for e, c in zip(p.events, p.totals)))

    if args.callers:
        print(f"\n== callers of *{args.callers}* (inclusive, by {args.sort}) ==")
        rows = [
            (site, tgt, ent)
            for site, tgts in p.call_sites.items()
            if os.path.exists(site[0])
            for tgt, ent in tgts.items()
            if args.callers in tgt
        ]
        for (fl, fn, ln), tgt, ent in sorted(rows, key=lambda x: -x[2][1])[: args.calls]:
            print(
                f"{_fmt(ent[1]):>13} {100 * ent[1] / tot:5.1f}%  {ent[0]:>7}x  "
                f"{fl}:{ln} {fn} -> {tgt}"
            )
        if not rows:
            print(f"  (no call site calls a function matching {args.callers!r})")
        return

    print(f"\n== functions (self, by {args.sort}) ==")
    for key, cost in sorted(p.fn_self.items(), key=lambda kv: -kv[1][ei])[: args.top]:
        name = f"{key[0]}:{key[1]}"
        obj = p.fn_ob.get(key)
        if key[0] == "???" and obj:  # name the owning library for symbol-only frames
            name += f"  [{obj}]"
        print(f"{_fmt(cost[ei]):>13} {100 * cost[ei] / tot:5.1f}%  {name}")

    files = []
    for key, _ in sorted(p.fn_self.items(), key=lambda kv: -kv[1][ei])[: args.top]:
        if key[0] not in files and os.path.exists(key[0]):
            files.append(key[0])
    print(f"\n== hot lines (self, by {args.sort}) ==")
    srcs = {}
    for f in files:
        print(f"--- {f} ---")
        if f not in srcs:
            with open(f, errors="replace") as fh:
                srcs[f] = fh.read().splitlines()
        rows = sorted(
            (kv for kv in p.line_cost.items() if kv[0][0] == f), key=lambda kv: -kv[1][ei]
        )
        for (fl, fn, ln), cost in rows[: args.lines]:
            src = srcs[f][ln - 1].strip() if ln - 1 < len(srcs[f]) else "?"
            print(f"{_fmt(cost[ei]):>13} {100 * cost[ei] / tot:5.1f}%  {fn}:{ln}: {src[:80]}")

    print(f"\n== call sites (inclusive, by {args.sort}) ==")
    flat = [
        (site, tgt, ent)
        for site, tgts in p.call_sites.items()
        if os.path.exists(site[0])
        for tgt, ent in tgts.items()
    ]
    for (fl, fn, ln), tgt, ent in sorted(flat, key=lambda x: -x[2][1])[: args.calls]:
        print(
            f"{_fmt(ent[1]):>13} {100 * ent[1] / tot:5.1f}%  {ent[0]:>7}x  {fl}:{ln} {fn} -> {tgt}"
        )


def _selftest():
    """Parse a synthetic callgrind.out; assert totals, lines and call edges."""
    mini = (
        "# callgrind format\n"
        "positions: line\n"
        "events: Ir Dr\n"
        "summary: 100 20\n"
        "ob=(1) /lib/libfoo.so\n"
        "fl=(1) a.c\n"
        "fn=(1) f1\n"
        "10 60 5\n"  # absolute position
        "+5 10\n"  # delta position
        "cfn=(2) f2\n"
        "calls=2 30\n"
        "* 30 3\n"  # same position; cost of the call
        "fn=(2) f2\n"
        "20 30 15\n"
    )
    import tempfile

    with tempfile.NamedTemporaryFile("w", suffix=".out", delete=False) as t:
        t.write(mini)
        path = t.name
    try:
        p = Parser().parse(path)
    finally:
        os.unlink(path)
    assert p.totals == [100, 20], p.totals
    assert p.fn_self[("a.c", "f1")] == [70, 5], p.fn_self
    assert p.fn_self[("a.c", "f2")] == [30, 15], p.fn_self
    assert p.fn_ob[("a.c", "f1")] == "libfoo.so", p.fn_ob
    assert p.line_cost[("a.c", "f1", 10)] == [60, 5]
    assert p.line_cost[("a.c", "f1", 15)] == [10, 0]
    sites = p.call_sites[("a.c", "f1", 15)]
    assert sites == {"f2": [2, 30]}, sites
    print("selftest: OK")
    return 0


if __name__ == "__main__":
    main()
