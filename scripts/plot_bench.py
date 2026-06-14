#!/usr/bin/env python3
"""Render the kbuf benchmark figures used in docs/BENCHMARKS.md and the README.

The dataset below is the recorded bare-metal run (i9-12900H, governor
`performance`, producer/consumer pinned to CPU 2/4) that `docs/BENCHMARKS.md`
writes up. It is the canonical run captured by
`scripts/run-baremetal-bench.sh` (raw console log: `.bench-baremetal.log`,
which is gitignored, so the numbers are reproduced here verbatim). Re-run that
script on a quiet host and update the tables below to refresh the figures.

Throughput and latency are the clean reference pass; the false-sharing numbers
are from a later pass of the same machine that added the atomic-RMW access
pattern. See the "Threats to validity" note in docs/BENCHMARKS.md on why this
laptop does not yield a stable stddev throughput pass.

Usage:
    python3 scripts/plot_bench.py            # writes PNGs into docs/img/
    python3 scripts/plot_bench.py --outdir X

Requires matplotlib (and numpy). No network, no kbuf module needed.
"""

import argparse
import os

import matplotlib

matplotlib.use("Agg")  # headless: write files, never open a window
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.patheffects as pe  # noqa: E402

# --- recorded dataset (see module docstring) --------------------------------

# Throughput in MB/s, 64 MiB per run, 5 runs each: (min, avg, max).
SIZES = [64, 256, 1024, 4096, 16384]
THROUGHPUT = {
    "mutex": [(26, 28, 30), (93, 100, 112), (363, 392, 422),
              (1347, 1473, 1564), (4332, 4582, 4809)],
    "spsc":  [(206, 213, 219), (800, 817, 840), (2279, 2539, 2654),
              (6846, 7182, 7648), (15098, 15423, 15642)],
    "mmap":  [(4876, 5002, 5156), (8354, 8850, 9341), (12238, 12522, 12923),
              (17953, 18325, 18726), (20648, 21030, 21269)],
    "pipe":  [(321, 341, 354), (934, 960, 1001), (2014, 2053, 2088),
              (4762, 4830, 4987), (5741, 6051, 6413)],
}

# mmap one-way latency percentiles, nanoseconds, 20 000 samples.
LATENCY_NS = {"p50": 2765, "p90": 3185, "p99": 3545, "max": 3811}

# False-sharing micro-bench wall-clock, per core: 200 Mi plain-store increments
# vs 25 Mi atomic read-modify-writes (LOCK XADD). An atomic RMW must own the
# line exclusively for every op, so sharing a line ping-pongs it between L1s on
# every increment -- a far larger penalty than the store-only burst pattern.
FALSE_SHARING_S = {
    "store-only": {"same line": 0.074, "separate lines": 0.061},
    "atomic-RMW": {"same line": 0.572, "separate lines": 0.117},
}

# --- shared visual identity -------------------------------------------------

# One colour per transport, used in every figure. mmap (the hero result) wears
# NVIDIA green; the rest are a calm, distinguishable supporting palette.
COLORS = {"mmap": "#76B900", "spsc": "#1F6FEB",
          "pipe": "#8957E5", "mutex": "#D1495B"}
LABELS = {"mmap": "mmap zero-copy ring",
          "spsc": "lock-free SPSC (syscall)",
          "pipe": "pipe(2) baseline",
          "mutex": "mutex (blocking syscall)"}

INK = "#1A1A1A"       # titles / primary text
SUBTLE = "#5A5A5A"    # subtitles / axis labels
FAINT = "#9A9A9A"     # source line / de-emphasised
SOURCE = "Source: bench/kbuf_bench.c on an i9-12900H (P-cores 2/4, governor performance)."

_WHITE_OUTLINE = [pe.withStroke(linewidth=2.4, foreground="white")]


def _style():
    plt.rcParams.update({
        "figure.dpi": 150,
        "savefig.dpi": 150,
        "savefig.bbox": "tight",
        "font.family": "DejaVu Sans",
        "font.size": 11,
        "text.color": INK,
        "axes.edgecolor": "#BFBFBF",
        "axes.linewidth": 0.9,
        "axes.labelcolor": SUBTLE,
        "axes.labelsize": 11,
        "axes.grid": True,
        "axes.grid.axis": "y",
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "grid.color": "#D8D8D8",
        "grid.linewidth": 0.8,
        "grid.alpha": 0.7,
        "xtick.color": SUBTLE,
        "ytick.color": SUBTLE,
        "xtick.labelsize": 10,
        "ytick.labelsize": 10,
        "legend.fontsize": 10,
        "legend.frameon": False,
    })


def _titles(ax, title, subtitle):
    """Left-aligned bold title with a lighter subtitle stacked above the axes."""
    ax.set_title(title, loc="left", fontsize=13.5, fontweight="bold",
                 color=INK, pad=30)
    ax.annotate(subtitle, xy=(0, 1), xycoords="axes fraction",
                xytext=(0, 9), textcoords="offset points",
                ha="left", va="bottom", fontsize=10.5, color=SUBTLE)


def _source(fig):
    fig.text(0.008, 0.006, SOURCE, ha="left", va="bottom",
             fontsize=8, color=FAINT, style="italic")


def _finish(fig, path):
    fig.tight_layout(rect=(0, 0.035, 1, 1))
    _source(fig)
    fig.savefig(path)
    plt.close(fig)
    return path


def _xticklabels(sizes):
    return [f"{s} B" if s < 1024 else f"{s // 1024} KiB" for s in sizes]


def plot_throughput(outdir):
    fig, ax = plt.subplots(figsize=(8.2, 5.2))
    for name in ("mmap", "spsc", "pipe", "mutex"):
        lo = [v[0] for v in THROUGHPUT[name]]
        avg = [v[1] for v in THROUGHPUT[name]]
        hi = [v[2] for v in THROUGHPUT[name]]
        ax.fill_between(SIZES, lo, hi, color=COLORS[name], alpha=0.16, zorder=2,
                        linewidth=0)
        ax.plot(SIZES, avg, marker="o", color=COLORS[name], label=LABELS[name],
                linewidth=2.4, markersize=6, markeredgecolor="white",
                markeredgewidth=0.8, zorder=3)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZES)
    ax.set_xticklabels(_xticklabels(SIZES))
    ax.set_xlabel("message size")
    ax.set_ylabel("throughput  (MB/s, log scale)")
    ax.set_xlim(SIZES[0] * 0.85, SIZES[-1] * 1.18)
    _titles(ax, "Throughput scales with message size",
            "Four transports, 64 MiB per run; shaded band = min/max of 5 runs.")
    ax.legend(loc="lower right", borderaxespad=0.8)
    return _finish(fig, os.path.join(outdir, "throughput.png"))


def plot_speedup(outdir):
    fig, ax = plt.subplots(figsize=(8.2, 5.2))
    base = [v[1] for v in THROUGHPUT["mutex"]]
    # Per-series label offsets keep annotations from colliding where the SPSC
    # and pipe lines run close: SPSC labels sit below their markers, the other
    # two above.
    offsets = {"mmap": (0, 12), "spsc": (0, -16), "pipe": (0, 12)}
    for name in ("mmap", "spsc", "pipe"):
        avg = [v[1] for v in THROUGHPUT[name]]
        speedup = [a / b for a, b in zip(avg, base)]
        ax.plot(SIZES, speedup, marker="o", color=COLORS[name],
                label=LABELS[name], linewidth=2.4, markersize=6,
                markeredgecolor="white", markeredgewidth=0.8, zorder=3)
        for x, y in zip(SIZES, speedup):
            ax.annotate(f"{y:.0f}×", (x, y), textcoords="offset points",
                        xytext=offsets[name], ha="center", fontsize=9,
                        fontweight="bold", color=COLORS[name],
                        path_effects=_WHITE_OUTLINE, zorder=4)
    ax.axhline(1.0, color=COLORS["mutex"], linestyle=(0, (5, 4)),
               linewidth=1.3, zorder=1, label="mutex baseline (1×)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZES)
    ax.set_xticklabels(_xticklabels(SIZES))
    ax.set_xlabel("message size")
    ax.set_ylabel("speedup over mutex path  (×, log scale)")
    ax.set_xlim(SIZES[0] * 0.85, SIZES[-1] * 1.18)
    _titles(ax, "The win is largest on small messages",
            "Where per-message syscall and lock cost dominate the transfer.")
    ax.legend(loc="upper right", borderaxespad=0.8)
    return _finish(fig, os.path.join(outdir, "speedup.png"))


def plot_latency(outdir):
    fig, ax = plt.subplots(figsize=(7.4, 4.8))
    keys = ["p50", "p90", "p99", "max"]
    vals_us = [LATENCY_NS[k] / 1000.0 for k in keys]
    bars = ax.bar(keys, vals_us, color=COLORS["mmap"], width=0.62,
                  edgecolor="white", linewidth=1.0, zorder=3)
    for b, v in zip(bars, vals_us):
        ax.annotate(f"{v:.2f} µs",
                    (b.get_x() + b.get_width() / 2, v),
                    textcoords="offset points", xytext=(0, 5), ha="center",
                    fontsize=10, fontweight="bold", color=INK)
    ax.set_ylabel("one-way latency  (µs)")
    ax.set_ylim(0, max(vals_us) * 1.22)
    ax.margins(x=0.06)
    _titles(ax, "mmap ring one-way latency",
            "20 000 samples, mlock'd pages, busy-poll consumer; tail ≈ 1.4× the median.")
    return _finish(fig, os.path.join(outdir, "latency.png"))


def plot_false_sharing(outdir):
    fig, ax = plt.subplots(figsize=(7.6, 5.2))
    groups = list(FALSE_SHARING_S.keys())  # store-only, atomic-RMW
    x = list(range(len(groups)))
    width = 0.34
    same = [FALSE_SHARING_S[g]["same line"] for g in groups]
    sep = [FALSE_SHARING_S[g]["separate lines"] for g in groups]
    b1 = ax.bar([i - width / 2 for i in x], same, width, color=COLORS["mutex"],
                edgecolor="white", linewidth=1.0, label="same cache line",
                zorder=3)
    b2 = ax.bar([i + width / 2 for i in x], sep, width, color=COLORS["mmap"],
                edgecolor="white", linewidth=1.0, label="separate cache lines",
                zorder=3)
    for bars in (b1, b2):
        for b in bars:
            ax.annotate(f"{b.get_height():.3f} s",
                        (b.get_x() + b.get_width() / 2, b.get_height()),
                        textcoords="offset points", xytext=(0, 4),
                        ha="center", fontsize=9, color=SUBTLE)
    for i, g in enumerate(groups):
        sp = (FALSE_SHARING_S[g]["same line"] /
              FALSE_SHARING_S[g]["separate lines"])
        ax.annotate(f"{sp:.2f}×  penalty", (i, max(same[i], sep[i])),
                    textcoords="offset points", xytext=(0, 30), ha="center",
                    fontsize=11, fontweight="bold", color=INK,
                    path_effects=_WHITE_OUTLINE)
    ax.set_yscale("log")
    ax.set_ylim(min(same + sep) * 0.55, max(same + sep) * 4.2)
    ax.set_xticks(x)
    ax.set_xticklabels(groups, fontsize=11)
    ax.set_ylabel("wall-clock  (s, log scale — lower is better)")
    _titles(ax, "Cache-line separation pays off under contention",
            "Two pinned cores incrementing counters; same line vs padded apart.")
    ax.legend(loc="upper left", borderaxespad=0.8)
    return _finish(fig, os.path.join(outdir, "false_sharing.png"))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    default_out = os.path.normpath(os.path.join(here, "..", "docs", "img"))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--outdir", default=default_out,
                    help="directory for PNG output (default: docs/img)")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    _style()
    for fn in (plot_throughput, plot_speedup, plot_latency, plot_false_sharing):
        print("wrote", fn(args.outdir))


if __name__ == "__main__":
    main()
