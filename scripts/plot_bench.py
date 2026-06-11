#!/usr/bin/env python3
"""Render the kbuf benchmark figures used in docs/BENCHMARKS.md and the README.

The dataset below is the recorded bare-metal run (i9-12900H, governor
`performance`, producer/consumer pinned to CPU 2/4) that `docs/BENCHMARKS.md`
writes up. It is the canonical run captured by
`scripts/run-baremetal-bench.sh` (raw console log: `.bench-baremetal.log`,
which is gitignored, so the numbers are reproduced here verbatim). Re-run that
script on a quiet host and update the tables below to refresh the figures.

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

# False-sharing micro-bench wall-clock, 200 Mi increments/core.
FALSE_SHARING_S = {"same line": 0.060, "separate lines": 0.047}

# Consistent identity per transport across every figure.
COLORS = {"mutex": "#c0392b", "spsc": "#2e86de",
          "mmap": "#27ae60", "pipe": "#8e44ad"}
LABELS = {"mutex": "mutex (blocking syscall)",
          "spsc": "lock-free SPSC (syscall)",
          "mmap": "mmap zero-copy ring",
          "pipe": "pipe(2) baseline"}


def _style():
    plt.rcParams.update({
        "figure.dpi": 130,
        "savefig.dpi": 130,
        "font.size": 11,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "axes.axisbelow": True,
        "axes.spines.top": False,
        "axes.spines.right": False,
    })


def _xticklabels(sizes):
    return [f"{s} B" if s < 1024 else f"{s // 1024} KiB" for s in sizes]


def plot_throughput(outdir):
    fig, ax = plt.subplots(figsize=(8, 5))
    for name in ("mmap", "spsc", "pipe", "mutex"):
        lo = [v[0] for v in THROUGHPUT[name]]
        avg = [v[1] for v in THROUGHPUT[name]]
        hi = [v[2] for v in THROUGHPUT[name]]
        ax.plot(SIZES, avg, marker="o", color=COLORS[name],
                label=LABELS[name], linewidth=2, zorder=3)
        ax.fill_between(SIZES, lo, hi, color=COLORS[name], alpha=0.18, zorder=2)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZES)
    ax.set_xticklabels(_xticklabels(SIZES))
    ax.set_xlabel("message size")
    ax.set_ylabel("throughput (MB/s, log scale)")
    ax.set_title("kbuf throughput vs message size\n"
                 "i9-12900H, two P-cores, 64 MiB/run (band = min/max of 5)")
    ax.legend(frameon=False)
    fig.tight_layout()
    path = os.path.join(outdir, "throughput.png")
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_speedup(outdir):
    fig, ax = plt.subplots(figsize=(8, 5))
    base = [v[1] for v in THROUGHPUT["mutex"]]
    for name in ("mmap", "spsc", "pipe"):
        avg = [v[1] for v in THROUGHPUT[name]]
        speedup = [a / b for a, b in zip(avg, base)]
        ax.plot(SIZES, speedup, marker="o", color=COLORS[name],
                label=LABELS[name], linewidth=2)
        for x, y in zip(SIZES, speedup):
            ax.annotate(f"{y:.0f}×", (x, y), textcoords="offset points",
                        xytext=(0, 7), ha="center", fontsize=9,
                        color=COLORS[name])
    ax.axhline(1.0, color="#c0392b", linestyle="--", linewidth=1.2,
               label="mutex baseline (1×)")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZES)
    ax.set_xticklabels(_xticklabels(SIZES))
    ax.set_xlabel("message size")
    ax.set_ylabel("speedup over mutex syscall path (×, log scale)")
    ax.set_title("Relative speedup vs the blocking mutex path\n"
                 "biggest win at small messages, where syscall cost dominates")
    ax.legend(frameon=False)
    fig.tight_layout()
    path = os.path.join(outdir, "speedup.png")
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_latency(outdir):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    keys = ["p50", "p90", "p99", "max"]
    vals_us = [LATENCY_NS[k] / 1000.0 for k in keys]
    bars = ax.bar(keys, vals_us, color="#27ae60", width=0.6, zorder=3)
    for b, v in zip(bars, vals_us):
        ax.annotate(f"{v:.2f} µs", (b.get_x() + b.get_width() / 2, v),
                    textcoords="offset points", xytext=(0, 4), ha="center",
                    fontsize=10)
    ax.set_ylabel("one-way latency (µs)")
    ax.set_ylim(0, max(vals_us) * 1.25)
    ax.set_title("mmap ring one-way latency percentiles\n"
                 "20 000 samples, mlock'd pages, busy-poll consumer")
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    path = os.path.join(outdir, "latency.png")
    fig.savefig(path)
    plt.close(fig)
    return path


def plot_false_sharing(outdir):
    fig, ax = plt.subplots(figsize=(6, 4.5))
    keys = list(FALSE_SHARING_S.keys())
    vals = [FALSE_SHARING_S[k] for k in keys]
    colors = ["#c0392b", "#27ae60"]
    bars = ax.bar(keys, vals, color=colors, width=0.55, zorder=3)
    for b, v in zip(bars, vals):
        ax.annotate(f"{v:.3f} s", (b.get_x() + b.get_width() / 2, v),
                    textcoords="offset points", xytext=(0, 4), ha="center",
                    fontsize=10)
    speedup = FALSE_SHARING_S["same line"] / FALSE_SHARING_S["separate lines"]
    ax.set_ylabel("wall-clock (s, lower is better)")
    ax.set_ylim(0, max(vals) * 1.25)
    ax.set_title("False sharing: two cores incrementing counters\n"
                 f"separating onto distinct cache lines = {speedup:.2f}× faster")
    ax.grid(axis="x", visible=False)
    fig.tight_layout()
    path = os.path.join(outdir, "false_sharing.png")
    fig.savefig(path)
    plt.close(fig)
    return path


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
