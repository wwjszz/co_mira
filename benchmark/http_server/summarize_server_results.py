#!/usr/bin/env python3
"""Summarize a connection-scaling wrk matrix and render a compact SVG."""

from __future__ import annotations

import argparse
import csv
import html
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path


SERVERS = ("coroutine", "callback", "epoll", "blocking")
LABELS = {
    "coroutine": "co_mira coroutine",
    "callback": "io_uring callback",
    "epoll": "epoll event-loop",
    "blocking": "blocking thread pool",
}
COLORS = {
    "coroutine": "#2563eb",
    "callback": "#dc5a41",
    "epoll": "#178f65",
    "blocking": "#7c3aed",
}


@dataclass(frozen=True)
class Result:
    connections: int
    workers: int
    wrk_threads: int
    server: str
    rps: float
    average_us: float
    p99_us: float
    samples: int
    error_runs: int


def duration_us(value: str, unit: str) -> float:
    return float(value) * {"us": 1.0, "ms": 1_000.0, "s": 1_000_000.0}[unit]


def required(pattern: str, text: str, source: Path) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise ValueError(f"{source}: missing {pattern!r}")
    return match


def read_config(case_dir: Path) -> dict[str, str]:
    return dict(
        line.split("=", 1)
        for line in (case_dir / "config.txt").read_text().splitlines()
    )


def parse_result(case_dir: Path, server: str) -> Result:
    config = read_config(case_dir)
    rps_values: list[float] = []
    average_values: list[float] = []
    p99_values: list[float] = []
    error_runs = 0

    run_files = sorted(case_dir.glob(f"{server}-run-*.txt"))
    if not run_files:
        raise ValueError(f"{case_dir}: no runs for {server}")

    for source in run_files:
        text = source.read_text()
        rps_values.append(
            float(required(r"^Requests/sec:\s+([0-9.]+)", text, source)[1])
        )
        for destination, prefix in (
            (average_values, r"^\s+Latency\s+"),
            (p99_values, r"^\s+99%\s+"),
        ):
            match = required(
                prefix + r"([0-9.]+)(us|ms|s)", text, source
            )
            destination.append(duration_us(match[1], match[2]))
        if "Socket errors:" in text or "Non-2xx or 3xx responses:" in text:
            error_runs += 1

    return Result(
        connections=int(config["connections"]),
        workers=int(config["workers"]),
        wrk_threads=int(config["threads"]),
        server=server,
        rps=statistics.median(rps_values),
        average_us=statistics.median(average_values),
        p99_us=statistics.median(p99_values),
        samples=len(run_files),
        error_runs=error_runs,
    )


def select_cases(input_dir: Path) -> list[Path]:
    candidates = [
        path
        for path in input_dir.iterdir()
        if path.is_dir() and (path / "config.txt").is_file()
    ]
    by_connections: dict[int, Path] = {}
    for path in candidates:
        connections = int(read_config(path)["connections"])
        current = by_connections.get(connections)
        if current is None or (
            path.name.endswith("-validated")
            and not current.name.endswith("-validated")
        ):
            by_connections[connections] = path
    return [by_connections[value] for value in sorted(by_connections)]


def write_csv(results: list[Result], destination: Path) -> None:
    with destination.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(
            (
                "connections",
                "workers",
                "wrk_threads",
                "server",
                "median_requests_per_second",
                "median_average_us",
                "median_p99_us",
                "samples",
                "error_runs",
            )
        )
        for result in results:
            writer.writerow(
                (
                    result.connections,
                    result.workers,
                    result.wrk_threads,
                    result.server,
                    f"{result.rps:.2f}",
                    f"{result.average_us:.2f}",
                    f"{result.p99_us:.2f}",
                    result.samples,
                    result.error_runs,
                )
            )


def write_table(results: list[Result], destination: Path) -> None:
    connections = sorted({result.connections for result in results})
    by_key = {
        (result.connections, result.server): result for result in results
    }
    lines = [
        "| wrk | co_mira coroutine | io_uring callback | epoll | blocking |",
        "|---|---:|---:|---:|---:|",
    ]
    for value in connections:
        cells = [
            f"`-t1 -c{value}`",
            *[
                f"{by_key[(value, server)].rps:,.0f}"
                for server in SERVERS
            ],
        ]
        lines.append("| " + " | ".join(cells) + " |")
    destination.write_text("\n".join(lines) + "\n")


def render_svg(results: list[Result], destination: Path) -> None:
    width, height = 980, 560
    left, right, top, bottom = 92, 38, 92, 88
    plot_width = width - left - right
    plot_height = height - top - bottom
    connections = sorted({result.connections for result in results})
    maximum = max(result.rps for result in results)
    y_max = math.ceil(maximum * 1.12 / 20_000) * 20_000
    by_key = {
        (result.connections, result.server): result for result in results
    }

    def x_at(index: int) -> float:
        if len(connections) == 1:
            return left + plot_width / 2
        return left + index * plot_width / (len(connections) - 1)

    def y_at(value: float) -> float:
        return top + plot_height * (1 - value / y_max)

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}">',
        "<style>"
        "text{font-family:Inter,system-ui,-apple-system,sans-serif}"
        ".title{font-size:24px;font-weight:700;fill:#17202a}"
        ".subtitle{font-size:13px;fill:#66727d}"
        ".axis{font-size:12px;fill:#56636a}"
        ".legend{font-size:12px;font-weight:600;fill:#34404a}"
        "</style>",
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        '<text class="title" x="40" y="38">HTTP throughput on the '
        "2-vCPU Vlab VM</text>",
        '<text class="subtitle" x="40" y="62">Release build · loopback · '
        "1 server worker · median requests/second</text>",
    ]

    for tick in range(6):
        value = y_max * tick / 5
        y = y_at(value)
        parts.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{width-right}" '
            f'y2="{y:.1f}" stroke="#e5e9ed" stroke-width="1"/>'
        )
        parts.append(
            f'<text class="axis" x="{left-12}" y="{y+4:.1f}" '
            f'text-anchor="end">{value/1000:.0f}k</text>'
        )

    for index, value in enumerate(connections):
        x = x_at(index)
        parts.append(
            f'<text class="axis" x="{x:.1f}" y="{height-bottom+34}" '
            f'text-anchor="middle">wrk -t1 -c{value}</text>'
        )

    parts.append(
        f'<text class="axis" x="20" y="{top+plot_height/2:.1f}" '
        'text-anchor="middle" transform="rotate(-90 20 '
        f'{top+plot_height/2:.1f})">Requests / second</text>'
    )

    for server in SERVERS:
        points = [
            (
                x_at(index),
                y_at(by_key[(connections[index], server)].rps),
            )
            for index in range(len(connections))
        ]
        encoded = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        color = COLORS[server]
        parts.append(
            f'<polyline points="{encoded}" fill="none" stroke="{color}" '
            'stroke-width="3" stroke-linejoin="round" '
            'stroke-linecap="round"/>'
        )
        for x, y in points:
            parts.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="4.5" '
                f'fill="#fff" stroke="{color}" stroke-width="3"/>'
            )

    legend_x = left
    legend_y = height - 25
    for server in SERVERS:
        color = COLORS[server]
        label = html.escape(LABELS[server])
        parts.append(
            f'<line x1="{legend_x}" y1="{legend_y}" '
            f'x2="{legend_x+25}" y2="{legend_y}" stroke="{color}" '
            'stroke-width="3"/>'
        )
        parts.append(
            f'<text class="legend" x="{legend_x+33}" '
            f'y="{legend_y+4}">{label}</text>'
        )
        legend_x += 205

    parts.append("</svg>")
    destination.write_text("\n".join(parts) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)

    cases = select_cases(arguments.input)
    results = [
        parse_result(case, server) for case in cases for server in SERVERS
    ]
    write_csv(results, arguments.output / "results.csv")
    write_table(results, arguments.output / "throughput-table.md")
    render_svg(results, arguments.output / "throughput.svg")


if __name__ == "__main__":
    main()
