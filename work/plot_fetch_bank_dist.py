#!/usr/bin/env python3
"""Plot 16-byte fetch-bank distribution stats from a gem5 stats.txt file."""

import argparse
import csv
import html
import re
from pathlib import Path

BUCKETS = ["0-15", "16-31", "32-47", "48-63"]
SERIES = [
    (
        "fetchBufferStartOffsetDist",
        "fetch_buffer_requests",
        "Fetch buffer request starts",
    ),
    (
        "decoupledBpuFtqIcacheStartOffsetDist",
        "ftq_span_starts",
        "FTQ span starts",
    ),
    (
        "decoupledBpuFtqIcacheReadBankDist",
        "ftq_read_banks",
        "FTQ touched/read banks",
    ),
]


def parse_stats(stats_path):
    values = {
        csv_name: {bucket: 0.0 for bucket in BUCKETS}
        for _, csv_name, _ in SERIES
    }
    patterns = [
        (
            re.compile(r".*" + re.escape(stat_name) + r"::(\d+-\d+)$"),
            csv_name,
        )
        for stat_name, csv_name, _ in SERIES
    ]

    with stats_path.open(encoding="utf-8") as stats_file:
        for line in stats_file:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            name, raw_value = parts[0], parts[1]
            try:
                value = float(raw_value)
            except ValueError:
                continue
            for pattern, csv_name in patterns:
                match = pattern.match(name)
                if match and match.group(1) in values[csv_name]:
                    values[csv_name][match.group(1)] = value
                    break

    return values


def write_csv(values, csv_path):
    with csv_path.open("w", encoding="utf-8", newline="") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(["bucket"] + [csv_name for _, csv_name, _ in SERIES])
        for bucket in BUCKETS:
            writer.writerow(
                [bucket]
                + [int(values[csv_name][bucket]) for _, csv_name, _ in SERIES]
            )


def svg_text(x, y, text, size=13, anchor="middle", weight="400"):
    return (
        f'<text x="{x}" y="{y}" font-family="Arial, sans-serif" '
        f'font-size="{size}" font-weight="{weight}" '
        f'text-anchor="{anchor}" fill="#202124">{html.escape(text)}</text>'
    )


def write_svg(values, svg_path):
    width = 980
    panel_h = 210
    top = 54
    left = 82
    chart_w = 820
    chart_h = 120
    gap = 46
    colors = ["#2f6fbb", "#d2691e", "#2f8f6f"]
    height = top + len(SERIES) * panel_h + 24
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        svg_text(
            width / 2,
            30,
            "Fetch / FTQ 16-byte bank distribution",
            size=20,
            weight="700",
        ),
    ]

    for idx, (_, csv_name, title) in enumerate(SERIES):
        y0 = top + idx * panel_h
        data = [values[csv_name][bucket] for bucket in BUCKETS]
        max_value = max(data) if max(data) > 0 else 1.0
        total = sum(data)

        lines.append(
            svg_text(
                left, y0 + 8, title, size=16, anchor="start", weight="700"
            )
        )
        lines.append(
            f'<line x1="{left}" y1="{y0 + chart_h + 24}" '
            f'x2="{left + chart_w}" y2="{y0 + chart_h + 24}" '
            'stroke="#c7cbd1" stroke-width="1"/>'
        )
        lines.append(
            f'<line x1="{left}" y1="{y0 + 24}" x2="{left}" '
            f'y2="{y0 + chart_h + 24}" stroke="#c7cbd1" stroke-width="1"/>'
        )
        lines.append(
            svg_text(
                left - 10, y0 + 28, str(int(max_value)), size=11, anchor="end"
            )
        )
        lines.append(
            svg_text(left - 10, y0 + chart_h + 28, "0", size=11, anchor="end")
        )

        slot = chart_w / len(BUCKETS)
        bar_w = min(108, slot * 0.58)
        for bidx, bucket in enumerate(BUCKETS):
            value = data[bidx]
            bar_h = chart_h * value / max_value
            x = left + bidx * slot + (slot - bar_w) / 2
            y = y0 + 24 + chart_h - bar_h
            pct = 0.0 if total == 0 else 100.0 * value / total
            lines.append(
                f'<rect x="{x:.1f}" y="{y:.1f}" width="{bar_w:.1f}" '
                f'height="{bar_h:.1f}" rx="3" fill="{colors[idx]}"/>'
            )
            lines.append(
                svg_text(x + bar_w / 2, y - 6, str(int(value)), size=12)
            )
            lines.append(
                svg_text(x + bar_w / 2, y0 + chart_h + 46, bucket, size=13)
            )
            lines.append(
                svg_text(
                    x + bar_w / 2, y0 + chart_h + 64, f"{pct:.1f}%", size=11
                )
            )

        lines.append(
            svg_text(
                left + chart_w + 18,
                y0 + chart_h + 24,
                f"total {int(total)}",
                size=12,
                anchor="start",
            )
        )
        if idx != len(SERIES) - 1:
            lines.append(
                f'<line x1="42" y1="{y0 + panel_h - gap / 2}" '
                f'x2="{width - 42}" y2="{y0 + panel_h - gap / 2}" '
                'stroke="#eef0f2" stroke-width="1"/>'
            )

    lines.append("</svg>")
    svg_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "path",
        help="gem5 output directory or stats.txt path",
    )
    parser.add_argument("--csv", help="CSV output path")
    parser.add_argument("--svg", help="SVG output path")
    args = parser.parse_args()

    input_path = Path(args.path)
    stats_path = (
        input_path / "stats.txt" if input_path.is_dir() else input_path
    )
    out_dir = stats_path.parent
    csv_path = Path(args.csv) if args.csv else out_dir / "fetch_bank_dist.csv"
    svg_path = Path(args.svg) if args.svg else out_dir / "fetch_bank_dist.svg"

    values = parse_stats(stats_path)
    write_csv(values, csv_path)
    write_svg(values, svg_path)
    print(f"[fetch-bank] csv: {csv_path}")
    print(f"[fetch-bank] svg: {svg_path}")


if __name__ == "__main__":
    main()
