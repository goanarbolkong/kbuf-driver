#!/usr/bin/env python3
"""Render docs/img/trace_timeline.png from a real kbuf tracepoint capture.

Unlike the benchmark figures (whose dataset is a recorded bare-metal run), this
one is generated *live from the driver itself*: it boots the module in QEMU,
enables the `kbuf:*` ftrace tracepoints, runs a fast producer against a slow
consumer on the 8-slot blocking ring, and plots the ring occupancy that the
`kbuf_produce`/`kbuf_consume` events report over time. The plateau at the ring
depth is the producer blocking on backpressure — exactly what Phase 7's
observability is meant to make visible.

    python3 scripts/plot_trace.py                 # boot QEMU, capture, plot
    python3 scripts/plot_trace.py --from-file F   # replot a saved trace
    python3 scripts/plot_trace.py --outdir X

The raw trace is cached at .qemu/kbuf-trace.txt so the figure can be re-rendered
offline. Requires matplotlib; the live capture additionally needs the QEMU
verification stack (qemu-system-x86, busybox-static, a kernel image).
"""

import argparse
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))
sys.path.insert(0, os.path.join(REPO, "verif"))

import plot_bench as pb  # noqa: E402  (shared visual identity)

_TRACE_CACHE = os.path.join(REPO, ".qemu", "kbuf-trace.txt")
_SCENARIO = "/scenarios/trace_capture.sh"
SOURCE = ("Source: verif/scenarios/trace_capture.sh → kbuf:* tracepoints "
          "(ftrace), one QEMU boot; parsed by scripts/plot_trace.py.")

# e.g. "  test_producer-95  [000] .....  0.420536: kbuf_produce: kbuf0 ... occ=1"
_LINE = re.compile(
    r"\[(?P<cpu>\d+)\]\s+\S+\s+(?P<t>\d+\.\d+):\s+"
    r"(?P<ev>kbuf_produce|kbuf_consume|kbuf_wakeup):\s+kbuf\d+\s+(?P<rest>.*)")


def capture_trace(scenario: str = _SCENARIO, cache: str = _TRACE_CACHE) -> str:
    """Boot QEMU, run a trace scenario, return (and cache) the raw ftrace text.

    Shared by scripts/plot_spsc.py for the lock-free capture.
    """
    from pathlib import Path

    from kbufverif import build_image
    from kbufverif.qemu import QemuRunner, resolve_kernel

    repo = Path(REPO)
    initrd = build_image(repo, repo / ".qemu")
    runner = QemuRunner(resolve_kernel(repo), initrd)
    res = runner.boot(f"kbuf.cmd={scenario} kbuf.timeout=30", timeout=180)
    m = re.search(r"KBUF_TRACE_BEGIN(.*?)KBUF_TRACE_END", res.serial, re.S)
    if not m:
        raise RuntimeError("trace markers not found in serial output")
    text = m.group(1)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "w") as fh:
        fh.write(text)
    return text


def parse_events(text):
    """Return (events, t0). events: list of dicts with t (s), ev, occ, woke."""
    events = []
    for line in text.splitlines():
        m = _LINE.search(line)
        if not m:
            continue
        rest = m.group("rest")
        occ = re.search(r"occ=(\d+)", rest)
        events.append({
            "t": float(m.group("t")),
            "cpu": int(m.group("cpu")),
            "ev": m.group("ev"),
            "occ": int(occ.group(1)) if occ else None,
            "readers": "readers" in rest,
        })
    if not events:
        raise RuntimeError("no kbuf tracepoint events parsed")
    events.sort(key=lambda e: e["t"])
    return events, events[0]["t"]


def plot(events, t0, outdir):
    ms = lambda e: (e["t"] - t0) * 1e3  # noqa: E731

    occ_pts = [(ms(e), e["occ"], e["ev"]) for e in events if e["occ"] is not None]
    cap = max(o for _, o, _ in occ_pts)
    # Step series for occupancy (pre-pad a zero so the ramp starts at empty).
    xs = [0.0] + [t for t, _, _ in occ_pts]
    ys = [0] + [o for _, o, _ in occ_pts]

    prod = [(t, o) for t, o, ev in occ_pts if ev == "kbuf_produce"]
    cons = [(t, o) for t, o, ev in occ_pts if ev == "kbuf_consume"]

    pb._style()
    fig, ax = plt.subplots(figsize=(9.0, 5.0))

    ax.fill_between(xs, ys, step="post", color=pb.COLORS["mmap"], alpha=0.13,
                    linewidth=0, zorder=1)
    ax.step(xs, ys, where="post", color=pb.COLORS["mmap"], linewidth=1.6,
            zorder=2)

    # Shade the spans where the ring is full == the producer is blocked.
    full_label_done = False
    for i in range(len(xs) - 1):
        if ys[i] >= cap:
            ax.axvspan(xs[i], xs[i + 1], color=pb.COLORS["mutex"], alpha=0.08,
                       linewidth=0, zorder=0,
                       label=None if full_label_done else "producer blocked (ring full)")
            full_label_done = True

    ax.axhline(cap, color=pb.COLORS["mutex"], linestyle=(0, (5, 4)),
               linewidth=1.2, zorder=3, label=f"ring full ({cap} slots)")

    ax.scatter([t for t, _ in prod], [o for _, o in prod], marker="^", s=22,
               color=pb.COLORS["mmap"], edgecolor="white", linewidth=0.5,
               zorder=4, label=f"produce  ({len(prod)})")
    ax.scatter([t for t, _ in cons], [o for _, o in cons], marker="v", s=22,
               color=pb.COLORS["spsc"], edgecolor="white", linewidth=0.5,
               zorder=4, label=f"consume  ({len(cons)})")

    wakeups = sum(1 for e in events if e["ev"] == "kbuf_wakeup")
    ax.annotate("ring saturates almost immediately —\n"
                "the producer is paced by the slow consumer",
                xy=(xs[len(xs) // 2], cap), xytext=(xs[-1] * 0.30, cap - 3.2),
                fontsize=10, color=pb.INK, ha="left",
                arrowprops=dict(arrowstyle="->", color=pb.SUBTLE, lw=1.1))

    ax.set_xlim(0, xs[-1] * 1.02)
    ax.set_ylim(0, cap + 0.8)
    ax.set_yticks(range(0, cap + 1))
    ax.set_xlabel("time since first event  (ms)")
    ax.set_ylabel("ring occupancy  (full slots)")
    pb._titles(ax, "What the tracepoints see: backpressure in real time",
               f"Live kbuf:* ftrace capture in QEMU — {len(prod)} produce, "
               f"{len(cons)} consume, {wakeups} wakeups on an 8-slot ring.")
    ax.legend(loc="lower left", ncol=1, borderaxespad=0.9, frameon=True,
              facecolor="white", framealpha=0.78, edgecolor="none")

    fig.tight_layout(rect=(0, 0.035, 1, 1))
    fig.text(0.008, 0.006, SOURCE, ha="left", va="bottom", fontsize=8,
             color=pb.FAINT, style="italic")
    path = os.path.join(outdir, "trace_timeline.png")
    fig.savefig(path)
    plt.close(fig)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outdir",
                    default=os.path.join(REPO, "docs", "img"),
                    help="directory for PNG output (default: docs/img)")
    ap.add_argument("--from-file", default=None,
                    help="replot from a saved trace instead of booting QEMU")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    if args.from_file:
        with open(args.from_file) as fh:
            text = fh.read()
    else:
        text = capture_trace()

    events, t0 = parse_events(text)
    print("wrote", plot(events, t0, args.outdir))


if __name__ == "__main__":
    main()
