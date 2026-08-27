#!/usr/bin/env python3
"""Create iteration-vs-accuracy tables and plots from gem5 stats.txt."""

import argparse
import csv
import math
import os
from pathlib import Path
from typing import (
    Dict,
    Iterable,
    List,
    Optional,
)

DEFAULT_STATS_CANDIDATES = (
    Path("m5out/iter_vs_accuracy/stats.txt"),
    Path("m5out/iter_vs_accuarcy/stats.txt"),
    Path("m5out/iter_vs_accuarcy_300/stats.txt"),
)


def default_min_change() -> float:
    value = os.environ.get("MIN_CHANGE", "1e-6")
    try:
        return float(value)
    except ValueError:
        return 1e-6


def default_int_from_env(name: str, fallback: int) -> int:
    value = os.environ.get(name)
    if value is None:
        return fallback
    try:
        parsed = int(value)
    except ValueError:
        return fallback
    return parsed if parsed > 0 else fallback


def parse_float(value: str) -> Optional[float]:
    try:
        result = float(value)
    except ValueError:
        return None
    if math.isnan(result) or math.isinf(result):
        return None
    return result


def default_stats_path() -> Path:
    for candidate in DEFAULT_STATS_CANDIDATES:
        if candidate.exists():
            return candidate
    return DEFAULT_STATS_CANDIDATES[0]


def numeric_stat_from_line(line: str) -> Optional[tuple[str, float]]:
    body = line.split("#", 1)[0].strip()
    if not body or body.startswith("-"):
        return None

    fields = body.split()
    if len(fields) < 2:
        return None

    value = parse_float(fields[1])
    if value is None:
        return None
    return fields[0], value


def read_blocks(
    stats_path: Path, accuracy_stat: str, ipc_stat: str
) -> List[Dict[str, float]]:
    blocks: List[Dict[str, float]] = []
    current: Optional[Dict[str, float]] = None

    with stats_path.open(
        "r", encoding="utf-8", errors="replace"
    ) as stats_file:
        for line in stats_file:
            if "Begin Simulation Statistics" in line:
                current = {}
                continue

            if "End Simulation Statistics" in line:
                if current is not None:
                    if "accuracy" in current and "ipc" in current:
                        current["sample"] = float(len(blocks) + 1)
                        blocks.append(current)
                current = None
                continue

            if current is None:
                continue

            parsed = numeric_stat_from_line(line)
            if parsed is None:
                continue

            name, value = parsed
            if name == accuracy_stat or name.endswith(".myTageStats.accuracy"):
                current.setdefault("accuracy", value)
            elif name == ipc_stat:
                current["ipc"] = value
            elif "ipc" not in current and name.endswith(".commitStats0.ipc"):
                current["ipc"] = value
            elif name == "simInsts":
                current["simInsts"] = value
            elif name == "system.cpu.numCycles":
                current["numCycles"] = value
            elif name == "simTicks":
                current["simTicks"] = value

    return blocks


def select_records(
    blocks: List[Dict[str, float]], start: int, count: int
) -> List[Dict[str, float]]:
    if start < 1:
        raise ValueError("-n must be >= 1")
    if count < 1:
        raise ValueError("-m must be >= 1")

    begin = start - 1
    end = begin + count
    selected = blocks[begin:end]

    for offset, row in enumerate(selected):
        row["iter"] = float(start + offset)
    return selected


def write_csv(path: Path, rows: Iterable[Dict[str, float]]) -> None:
    fieldnames = [
        "iter",
        "sample",
        "accuracy",
        "ipc",
        "simInsts",
        "numCycles",
        "simTicks",
    ]
    digits_by_field = {
        "iter": 0,
        "sample": 0,
        "accuracy": 6,
        "ipc": 6,
        "simInsts": 0,
        "numCycles": 0,
        "simTicks": 0,
    }
    with path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.DictWriter(
            csv_file, fieldnames=fieldnames, extrasaction="ignore"
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    name: format_value(
                        row.get(name), digits=digits_by_field[name]
                    )
                    for name in fieldnames
                }
            )


def write_markdown(path: Path, rows: List[Dict[str, float]]) -> None:
    with path.open("w", encoding="utf-8") as md_file:
        md_file.write(
            "| iter | sample | accuracy | ipc | simInsts | numCycles |\n"
        )
        md_file.write("| ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for row in rows:
            md_file.write(
                (
                    "| {iter} | {sample} | {accuracy} | {ipc} | "
                    "{simInsts} | {numCycles} |\n"
                ).format(
                    iter=format_value(row.get("iter"), digits=0),
                    sample=format_value(row.get("sample"), digits=0),
                    accuracy=format_value(row.get("accuracy"), digits=6),
                    ipc=format_value(row.get("ipc"), digits=6),
                    simInsts=format_value(row.get("simInsts"), digits=0),
                    numCycles=format_value(row.get("numCycles"), digits=0),
                )
            )


def format_value(value: Optional[float], digits: int = 6) -> str:
    if value is None:
        return ""
    if digits == 0:
        return str(int(value))
    return f"{value:.{digits}f}"


def setup_matplotlib():
    cache_dir = Path(
        os.environ.get("MPLCONFIGDIR", "/tmp/matplotlib-gem5-plots")
    )
    cache_dir.mkdir(parents=True, exist_ok=True)
    os.environ.setdefault("MPLCONFIGDIR", str(cache_dir))

    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    return plt


def write_plot(
    path: Path,
    rows: List[Dict[str, float]],
    y_key: str,
    title: str,
    y_label: str,
) -> None:
    plt = setup_matplotlib()

    xs = [row["iter"] for row in rows]
    ys = [row[y_key] for row in rows]

    fig, ax = plt.subplots(figsize=(11, 5.5), dpi=140)
    ax.plot(xs, ys, color="#1f77b4", linewidth=1.8)
    ax.set_title(title)
    ax.set_xlabel("Iteration")
    ax.set_ylabel(y_label)
    ax.grid(True, alpha=0.28)
    ax.margins(x=0.01)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def write_ipc_accuracy_plot(path: Path, rows: List[Dict[str, float]]) -> None:
    plt = setup_matplotlib()

    xs = [row["iter"] for row in rows]
    ipc = [row["ipc"] for row in rows]
    accuracy = [row["accuracy"] for row in rows]

    fig, ax_ipc = plt.subplots(figsize=(11, 5.5), dpi=140)
    ax_acc = ax_ipc.twinx()

    ipc_line = ax_ipc.plot(
        xs,
        ipc,
        color="#1f77b4",
        linewidth=1.8,
        label="IPC",
    )
    acc_line = ax_acc.plot(
        xs,
        accuracy,
        color="#d62728",
        linewidth=1.5,
        label="Accuracy",
    )

    ax_ipc.set_title("IPC and MyTAGE Accuracy vs Iteration")
    ax_ipc.set_xlabel("Iteration")
    ax_ipc.set_ylabel("IPC", color="#1f77b4")
    ax_acc.set_ylabel("Accuracy", color="#d62728")
    ax_ipc.tick_params(axis="y", labelcolor="#1f77b4")
    ax_acc.tick_params(axis="y", labelcolor="#d62728")
    ax_ipc.grid(True, alpha=0.28)
    ax_ipc.margins(x=0.01)

    lines = ipc_line + acc_line
    ax_ipc.legend(
        lines, [line.get_label() for line in lines], loc="lower right"
    )

    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)


def first_accuracy_change_below(
    rows: List[Dict[str, float]], min_change: float
) -> Optional[tuple[int, float]]:
    for previous, current in zip(rows, rows[1:]):
        delta_iter = current["iter"] - previous["iter"]
        if delta_iter <= 0:
            continue
        derivative = (current["accuracy"] - previous["accuracy"]) / delta_iter
        if abs(derivative) < min_change:
            return int(current["iter"]), derivative
    return None


def linear_slope(
    points: List[Dict[str, float]], y_key: str
) -> Optional[float]:
    count = len(points)
    if count < 2:
        return None

    xs = [point["iter"] for point in points]
    ys = [point[y_key] for point in points]
    mean_x = sum(xs) / count
    mean_y = sum(ys) / count

    denominator = sum((x - mean_x) ** 2 for x in xs)
    if denominator == 0:
        return None

    numerator = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    return numerator / denominator


def first_windowed_slope_below(
    rows: List[Dict[str, float]],
    min_change: float,
    window: int,
    consecutive: int,
) -> Optional[tuple[int, int, float]]:
    if window < 2 or consecutive < 1:
        return None

    streak = 0
    first_streak_end_iter: Optional[int] = None
    last_slope = 0.0

    for end_index in range(window, len(rows) + 1):
        window_rows = rows[end_index - window : end_index]
        slope = linear_slope(window_rows, "accuracy")
        if slope is None:
            streak = 0
            first_streak_end_iter = None
            continue

        if abs(slope) < min_change:
            if streak == 0:
                first_streak_end_iter = int(window_rows[-1]["iter"])
            streak += 1
            last_slope = slope
            if streak >= consecutive and first_streak_end_iter is not None:
                return (
                    first_streak_end_iter,
                    int(window_rows[-1]["iter"]),
                    last_slope,
                )

        else:
            streak = 0
            first_streak_end_iter = None

    return None


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Parse gem5 stats.txt blocks and create a table plus "
            "accuracy/IPC plots. -n is the 1-based first stats block, "
            "-m is the number of blocks to take."
        )
    )
    parser.add_argument(
        "-n",
        "--start",
        type=int,
        default=1,
        help="1-based first stats block to use",
    )
    parser.add_argument(
        "-m",
        "--count",
        type=int,
        required=True,
        help="number of stats blocks to use",
    )
    parser.add_argument(
        "--stats",
        type=Path,
        default=default_stats_path(),
        help="input gem5 stats.txt path",
    )
    parser.add_argument(
        "--outdir",
        type=Path,
        default=None,
        help="output directory; defaults to the stats.txt directory",
    )
    parser.add_argument(
        "--prefix",
        default="iter_vs_accuracy",
        help="output file prefix",
    )
    parser.add_argument(
        "--accuracy-stat",
        default="system.cpu.branchPred.myTageStats.accuracy",
        help=(
            "exact accuracy stat name; any *.myTageStats.accuracy also "
            "matches"
        ),
    )
    parser.add_argument(
        "--ipc-stat",
        default="system.cpu.ipc",
        help="IPC stat name; commitStats0.ipc is used as fallback",
    )
    parser.add_argument(
        "--min-change",
        type=float,
        default=default_min_change(),
        help=(
            "print the first iter where abs(d(accuracy)/d(iter)) is below "
            "this threshold; default: MIN_CHANGE env or 1e-6"
        ),
    )
    parser.add_argument(
        "--slope-window",
        type=int,
        default=default_int_from_env("SLOPE_WINDOW", 20),
        help=(
            "window size for linear-fit accuracy slope; default: "
            "SLOPE_WINDOW env or 20"
        ),
    )
    parser.add_argument(
        "--consecutive",
        type=int,
        default=default_int_from_env("CONSECUTIVE", 5),
        help=(
            "number of consecutive windows required below --min-change; "
            "default: CONSECUTIVE env or 5"
        ),
    )
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    stats_path = args.stats
    if not stats_path.exists():
        print(f"error: stats file not found: {stats_path}", flush=True)
        return 2

    outdir = args.outdir if args.outdir is not None else stats_path.parent
    outdir.mkdir(parents=True, exist_ok=True)

    blocks = read_blocks(stats_path, args.accuracy_stat, args.ipc_stat)
    if not blocks:
        print(
            "error: no stats blocks with both myTageStats.accuracy and "
            "IPC were found",
            flush=True,
        )
        return 3

    try:
        rows = select_records(blocks, args.start, args.count)
    except ValueError as error:
        print(f"error: {error}", flush=True)
        return 2

    if not rows:
        print(
            f"error: start block {args.start} is beyond available "
            f"parsed blocks ({len(blocks)})",
            flush=True,
        )
        return 4

    requested_end = args.start + args.count - 1
    actual_end = int(rows[-1]["sample"])
    if actual_end < requested_end:
        print(
            f"warning: requested blocks {args.start}..{requested_end}, "
            f"but only {len(blocks)} parsed blocks are available; "
            f"using {args.start}..{actual_end}",
            flush=True,
        )

    csv_path = outdir / f"{args.prefix}.csv"
    md_path = outdir / f"{args.prefix}.md"
    accuracy_plot_path = outdir / f"{args.prefix}_accuracy.png"
    ipc_plot_path = outdir / f"{args.prefix}_ipc.png"

    write_csv(csv_path, rows)
    write_markdown(md_path, rows)
    write_plot(
        accuracy_plot_path,
        rows,
        "accuracy",
        "MyTAGE Accuracy vs Iteration",
        "Accuracy",
    )
    write_ipc_accuracy_plot(ipc_plot_path, rows)

    print(f"parsed blocks : {len(blocks)}")
    print(f"used blocks   : {args.start}..{actual_end} ({len(rows)} rows)")
    change_hit = first_accuracy_change_below(rows, args.min_change)
    if change_hit is None:
        print(
            "single step  : no iter found for "
            f"|d(accuracy)/d(iter)| < {args.min_change:g}"
        )
    else:
        iter_no, derivative = change_hit
        print(
            f"single step  : iter={iter_no}, "
            f"d(accuracy)/d(iter)={derivative:.12g} < {args.min_change:g}"
        )
    window_hit = first_windowed_slope_below(
        rows,
        args.min_change,
        args.slope_window,
        args.consecutive,
    )
    if window_hit is None:
        print(
            f"window slope : no {args.consecutive} consecutive windows found "
            f"with |slope| < {args.min_change:g} "
            f"(window={args.slope_window})"
        )
    else:
        first_iter, confirmed_iter, slope = window_hit
        print(
            f"window slope : first_iter={first_iter}, "
            f"confirmed_iter={confirmed_iter}, "
            f"slope={slope:.12g} < {args.min_change:g} "
            f"(window={args.slope_window}, consecutive={args.consecutive})"
        )
    print(f"csv          : {csv_path}")
    print(f"markdown     : {md_path}")
    print(f"accuracy png : {accuracy_plot_path}")
    print(f"ipc png      : {ipc_plot_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
