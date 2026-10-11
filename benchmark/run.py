#!/usr/bin/env python3
"""Run the benchmarks sequentially and save CSV results and six plots."""

import argparse
import csv
import os
from pathlib import Path
import secrets
import subprocess
import tempfile

os.environ.setdefault("MPLCONFIGDIR", str(Path(tempfile.gettempdir()) / "concurrency-matplotlib"))
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "benchmark/results"
CHARTS = ROOT / "benchmark/charts"
STACKS = {
    "lock_free_stack_leaking": "Treiber · leaking",
    "lock_free_stack_keep_thread_count": "Treiber · thread count",
    "lock_free_stack_hazard_pointer_simple": "Treiber · simple HP",
    "lock_free_stack_folly": "Treiber · Folly HP",
    "ebs_stack_folly": "Elimination · Folly HP",
    "timestamped_stack": "Timestamped · cyclic arena",
    "mutex_stack": "Mutex",
}
WAITS = (0, 100, 1000, 10000)
OPERATIONS = {"all": "All operations", "push": "Push", "pop": "Pop"}
METRICS = [f"{op}_{stat}_ns" for op in OPERATIONS for stat in ("mean", "p95")]


def read_samples(directory, stack, wait, expected_count=10_000_000):
    samples = {op: np.fromfile(directory / f"{stack}_wait_{wait}.{op}.bin", dtype="<i8")
               for op in ("push", "pop")}
    count = sum(values.size for values in samples.values())
    if count != expected_count:
        raise ValueError(f"{stack}, wait {wait}: expected {expected_count} timings, got {count}")
    samples["all"] = np.concatenate((samples["push"], samples["pop"]))
    row = {"stack": stack, "max_wait_ns": wait}
    for op, values in samples.items():
        if not values.size:
            raise ValueError(f"{stack}, wait {wait}: no {op} timings")
        row[f"{op}_mean_ns"] = float(values.mean())
        rank = (values.size * 95 + 99) // 100 - 1
        values.partition(rank)
        row[f"{op}_p95_ns"] = int(values[rank])
    return row


def write_csv(path, rows):
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def run_benchmarks(binary, runs):
    measurements = []
    for run in range(runs):
        seed = secrets.randbits(64)
        print(f"Run {run + 1}/{runs}", flush=True)
        with tempfile.TemporaryDirectory(prefix="stack-samples-") as directory:
            subprocess.run([str(binary.resolve()), "--benchmark_repetitions=1"], cwd=ROOT,
                           env={**os.environ, "STACK_BENCHMARK_SEED": str(seed),
                                "STACK_BENCHMARK_SAMPLES_DIR": directory}, check=True)
            for stack in STACKS:
                for wait in WAITS:
                    measurements.append({"run": run + 1, "seed": seed,
                                         **read_samples(Path(directory), stack, wait)})
    write_csv(RESULTS / "runs.csv", measurements)
    summary = []
    for stack in STACKS:
        for wait in WAITS:
            rows = [r for r in measurements if r["stack"] == stack and r["max_wait_ns"] == wait]
            row = {"stack": stack, "max_wait_ns": wait, "runs": runs}
            for metric in METRICS:
                values = [r[metric] for r in rows]
                row[f"{metric}_mean"] = float(np.mean(values))
                row[f"{metric}_stddev"] = float(np.std(values, ddof=1)) if runs > 1 else 0
            summary.append(row)
    write_csv(RESULTS / "summary.csv", summary)
    return summary


def plot(summary):
    indexed = {(r["stack"], int(r["max_wait_ns"])): r for r in summary}
    colors = ("#0072B2", "#E69F00", "#009E73", "#CC79A7")
    runs = int(summary[0]["runs"])
    plt.rcParams.update({"svg.fonttype": "none", "font.size": 10})
    for op, title in OPERATIONS.items():
        for stat in ("mean", "p95"):
            metric = f"{op}_{stat}_ns"
            fig, ax = plt.subplots(figsize=(12, 7.2))
            for i, wait in enumerate(WAITS):
                rows = [indexed[(stack, wait)] for stack in STACKS]
                ax.barh(np.arange(len(STACKS)) + (i - 1.5) * 0.18,
                        [float(r[f"{metric}_mean"]) for r in rows], height=0.16,
                        xerr=[float(r[f"{metric}_stddev"]) for r in rows], capsize=2,
                        color=colors[i], label=f"Max wait {wait:,} ns")
            ax.set_yticks(np.arange(len(STACKS)), list(STACKS.values()))
            ax.invert_yaxis()
            ax.set_xlim(left=0)
            ax.set_xlabel("Call latency (ns) · lower is faster")
            ax.set_title(f"{title} · {stat} latency\n{runs} runs · error bars: ±1 SD · 10 threads")
            ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.11), ncol=4, frameon=False)
            fig.subplots_adjust(left=0.26, right=0.97, top=0.88, bottom=0.16)
            fig.savefig(CHARTS / f"{op}-{stat}.svg")
            plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-vcpkg/treiber")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--plot-only", action="store_true", help="Use the existing summary CSV")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("runs must be positive")
    RESULTS.mkdir(exist_ok=True)
    CHARTS.mkdir(exist_ok=True)
    if args.plot_only:
        with (RESULTS / "summary.csv").open() as source:
            summary = list(csv.DictReader(source))
    else:
        summary = run_benchmarks(args.binary, args.runs)
    plot(summary)
    print("Saved CSV results in benchmark/results/ and plots in benchmark/charts/")
