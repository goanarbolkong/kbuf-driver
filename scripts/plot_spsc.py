#!/usr/bin/env python3
"""Render docs/img/spsc_handoff.png from a real lock-free SPSC capture.

Like scripts/plot_trace.py this is generated live from the driver: it boots the
module in QEMU, switches /dev/kbuf0 to lock-free SPSC mode, and runs a producer
pinned to CPU 0 against a consumer pinned to CPU 1 (test_spsc). ftrace tags each
`kbuf_produce`/`kbuf_consume` event with the CPU it ran on, so the figure can
draw every message's hand-off from the producer core to the consumer core
through the release/acquire ring — and show that the occupancy barely leaves 1,
the opposite of the blocking path's backpressure plateau.

    python3 scripts/plot_spsc.py                  # boot QEMU, capture, plot
    python3 scripts/plot_spsc.py --from-file F     # replot a saved trace
    python3 scripts/plot_spsc.py --outdir X

The raw trace is cached at .qemu/kbuf-spsc.txt. Requires matplotlib; the live
capture additionally needs the QEMU verification stack.
"""

import argparse
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(REPO, "scripts"))
sys.path.insert(0, os.path.join(REPO, "verif"))

import plot_bench as pb       # noqa: E402  (shared visual identity)
import plot_trace as pt       # noqa: E402  (shared capture + parse)

_CACHE = os.path.join(REPO, ".qemu", "kbuf-spsc.txt")
_SCENARIO = "/scenarios/spsc_capture.sh"
SOURCE = ("Source: verif/scenarios/spsc_capture.sh → test_spsc (SPSC mode) "
          "kbuf:* ftrace, one QEMU boot; parsed by scripts/plot_spsc.py.")


def plot(events, t0, outdir):
    us = lambda e: (e["t"] - t0) * 1e6  # noqa: E731  (microseconds)

    prod = [(us(e), e["cpu"]) for e in events if e["ev"] == "kbuf_produce"]
    cons = [(us(e), e["cpu"]) for e in events if e["ev"] == "kbuf_consume"]
    pcpu = prod[0][1] if prod else 0
    ccpu = cons[0][1] if cons else 1
    n = min(len(prod), len(cons))
    span = max(us(events[-1]), 1.0)

    pb._style()
    fig, ax = plt.subplots(figsize=(10.0, 4.4))

    y_prod, y_cons = 1.0, 0.0
    # Lane backgrounds make the two cores visually distinct.
    ax.axhspan(y_prod - 0.16, y_prod + 0.16, color=pb.COLORS["mmap"],
               alpha=0.07, linewidth=0, zorder=0)
    ax.axhspan(y_cons - 0.16, y_cons + 0.16, color=pb.COLORS["spsc"],
               alpha=0.07, linewidth=0, zorder=0)

    # One hand-off line per message: producer release-store (CPU a, top) to the
    # matching consumer acquire-load (CPU b, bottom). SPSC is strict FIFO, so
    # the i-th produce pairs with the i-th consume.
    handoff_label = "release → acquire hand-off"
    for i in range(n):
        ax.plot([prod[i][0], cons[i][0]], [y_prod, y_cons],
                color="#8A94A6", alpha=0.55, linewidth=0.9, zorder=2,
                label=handoff_label if i == 0 else None)

    ax.scatter([t for t, _ in prod], [y_prod] * len(prod), marker="v", s=46,
               color=pb.COLORS["mmap"], edgecolor="white", linewidth=0.6,
               zorder=4, label=f"kbuf_produce  ·  CPU {pcpu}  ({len(prod)})")
    ax.scatter([t for t, _ in cons], [y_cons] * len(cons), marker="^", s=46,
               color=pb.COLORS["spsc"], edgecolor="white", linewidth=0.6,
               zorder=4, label=f"kbuf_consume  ·  CPU {ccpu}  ({len(cons)})")

    ax.text(span * 0.5, y_prod + 0.27,
            f"producer  ·  core {pcpu}  ·  smp_store_release(prod_idx)",
            ha="center", va="bottom", fontsize=10, color=pb.SUBTLE)
    ax.text(span * 0.5, y_cons - 0.27,
            f"consumer  ·  core {ccpu}  ·  smp_load_acquire(prod_idx)",
            ha="center", va="top", fontsize=10, color=pb.SUBTLE)

    ax.set_xlim(-span * 0.02, span * 1.02)
    ax.set_ylim(-0.75, 1.75)
    ax.set_yticks([])
    ax.set_xlabel("time since first event  (µs)")
    for side in ("left",):
        ax.spines[side].set_visible(False)
    ax.grid(False)

    pb._titles(ax, "Lock-free SPSC: every message hands off across cores",
               f"{n} messages produced on CPU {pcpu}, consumed on CPU {ccpu} "
               f"in {span:.0f} µs — no lock, ring occupancy stays ≈ 1.")
    ax.legend(loc="upper right", ncol=3, borderaxespad=0.6, frameon=True,
              facecolor="white", framealpha=0.8, edgecolor="none")

    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.text(0.008, 0.006, SOURCE, ha="left", va="bottom", fontsize=8,
             color=pb.FAINT, style="italic")
    path = os.path.join(outdir, "spsc_handoff.png")
    fig.savefig(path)
    plt.close(fig)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outdir", default=os.path.join(REPO, "docs", "img"))
    ap.add_argument("--from-file", default=None,
                    help="replot from a saved trace instead of booting QEMU")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    if args.from_file:
        with open(args.from_file) as fh:
            text = fh.read()
    else:
        text = pt.capture_trace(scenario=_SCENARIO, cache=_CACHE)

    events, t0 = pt.parse_events(text)
    print("wrote", plot(events, t0, args.outdir))


if __name__ == "__main__":
    main()
