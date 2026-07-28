#!/usr/bin/env python3
"""Summarize wrk logs and render dependency-free SVG benchmark charts."""

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
COLORS = {
    "coroutine": "#2563eb",
    "callback": "#dc5a41",
    "epoll": "#178f65",
    "blocking": "#7c3aed",
}
LABELS = {
    "coroutine": "co_mira coroutine",
    "callback": "io_uring callback",
    "epoll": "epoll event-loop",
    "blocking": "blocking thread pool",
}
CASES = (
    "w2-c1",
    "w2-c16",
    "w2-c64",
    "w2-c256",
    "w2-c1024",
    "w1-c256",
    "w4-c256",
)


@dataclass(frozen=True)
class Result:
    case: str
    workers: int
    threads: int
    connections: int
    server: str
    rps: float
    avg_us: float
    p50_us: float
    p99_us: float
    error_runs: int
    cpu_percent: int
    max_rss_kb: int
    voluntary_switches: int


def duration_us(value: str, unit: str) -> float:
    scale = {"us": 1.0, "ms": 1_000.0, "s": 1_000_000.0}
    return float(value) * scale[unit]


def required(pattern: str, text: str, source: Path) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE)
    if match is None:
        raise ValueError(f"{source}: missing pattern {pattern!r}")
    return match


def parse_config(case_dir: Path) -> dict[str, str]:
    config: dict[str, str] = {}
    for line in (case_dir / "config.txt").read_text().splitlines():
        key, value = line.split("=", 1)
        config[key] = value
    return config


def parse_case(case_dir: Path, server: str) -> Result:
    config = parse_config(case_dir)
    rps_values: list[float] = []
    avg_values: list[float] = []
    p50_values: list[float] = []
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
        for values, percentile in (
            (avg_values, r"^\s+Latency\s+"),
            (p50_values, r"^\s+50%\s+"),
            (p99_values, r"^\s+99%\s+"),
        ):
            match = required(
                percentile + r"([0-9.]+)(us|ms|s)", text, source
            )
            values.append(duration_us(match[1], match[2]))
        if re.search(r"^\s*Socket errors:", text, re.MULTILINE):
            error_runs += 1

    time_source = case_dir / f"{server}-time.txt"
    time_text = time_source.read_text()
    cpu_percent = int(
        required(
            r"Percent of CPU this job got:\s+([0-9]+)%",
            time_text,
            time_source,
        )[1]
    )
    max_rss_kb = int(
        required(
            r"Maximum resident set size \(kbytes\):\s+([0-9]+)",
            time_text,
            time_source,
        )[1]
    )
    voluntary_switches = int(
        required(
            r"Voluntary context switches:\s+([0-9]+)",
            time_text,
            time_source,
        )[1]
    )

    return Result(
        case=case_dir.name,
        workers=int(config["workers"]),
        threads=int(config["threads"]),
        connections=int(config["connections"]),
        server=server,
        rps=statistics.median(rps_values),
        avg_us=statistics.median(avg_values),
        p50_us=statistics.median(p50_values),
        p99_us=statistics.median(p99_values),
        error_runs=error_runs,
        cpu_percent=cpu_percent,
        max_rss_kb=max_rss_kb,
        voluntary_switches=voluntary_switches,
    )


def write_csv(results: list[Result], destination: Path) -> None:
    with destination.open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(Result.__dataclass_fields__.keys())
        for result in results:
            writer.writerow(
                (
                    result.case,
                    result.workers,
                    result.threads,
                    result.connections,
                    result.server,
                    f"{result.rps:.2f}",
                    f"{result.avg_us:.2f}",
                    f"{result.p50_us:.2f}",
                    f"{result.p99_us:.2f}",
                    result.error_runs,
                    result.cpu_percent,
                    result.max_rss_kb,
                    result.voluntary_switches,
                )
            )


def nice_max(value: float) -> float:
    if value <= 0:
        return 1.0
    magnitude = 10 ** math.floor(math.log10(value))
    normalized = value / magnitude
    step = 1 if normalized <= 1 else 2 if normalized <= 2 else 5
    if normalized > 5:
        step = 10
    return step * magnitude


class SvgChart:
    width = 1080
    height = 620
    left = 100
    right = 35
    top = 108
    bottom = 86

    def __init__(self, title: str, subtitle: str, y_label: str):
        self.title = title
        self.subtitle = subtitle
        self.y_label = y_label
        self.items: list[str] = []

    @property
    def plot_width(self) -> float:
        return self.width - self.left - self.right

    @property
    def plot_height(self) -> float:
        return self.height - self.top - self.bottom

    def text(
        self,
        x: float,
        y: float,
        value: str,
        *,
        size: int = 14,
        anchor: str = "middle",
        weight: int = 400,
        fill: str = "#263238",
        transform: str = "",
    ) -> None:
        transform_attr = f' transform="{transform}"' if transform else ""
        self.items.append(
            f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" '
            f'font-size="{size}" font-weight="{weight}" fill="{fill}"'
            f'{transform_attr}>{html.escape(value)}</text>'
        )

    def axes(self, y_max: float, y_format) -> None:
        for index in range(6):
            value = y_max * index / 5
            y = self.top + self.plot_height * (1 - index / 5)
            self.items.append(
                f'<line x1="{self.left}" y1="{y:.1f}" '
                f'x2="{self.width - self.right}" y2="{y:.1f}" '
                'stroke="#d9e0e4" stroke-width="1"/>'
            )
            self.text(
                self.left - 12,
                y + 5,
                y_format(value),
                anchor="end",
                size=13,
                fill="#56636a",
            )
        self.text(
            24,
            self.top + self.plot_height / 2,
            self.y_label,
            size=14,
            weight=600,
            transform=(
                f"rotate(-90 24 "
                f"{self.top + self.plot_height / 2:.1f})"
            ),
        )

    def legend(self) -> None:
        x = self.left
        y = 82
        for server in SERVERS:
            self.items.append(
                f'<line x1="{x}" y1="{y - 5}" x2="{x + 25}" '
                f'y2="{y - 5}" stroke="{COLORS[server]}" '
                'stroke-width="4"/>'
            )
            self.text(
                x + 33,
                y,
                LABELS[server],
                anchor="start",
                size=13,
            )
            x += 220

    def save(self, destination: Path) -> None:
        header = (
            f'<svg xmlns="http://www.w3.org/2000/svg" '
            f'width="{self.width}" height="{self.height}" '
            f'viewBox="0 0 {self.width} {self.height}">'
            '<rect width="100%" height="100%" fill="#ffffff"/>'
            '<style>text{font-family:Inter,Segoe UI,Arial,sans-serif}</style>'
        )
        title = (
            f'<text x="{self.left}" y="28" text-anchor="start" '
            'font-family="Inter,Segoe UI,Arial,sans-serif" '
            'font-size="22" font-weight="700" fill="#172126">'
            f"{html.escape(self.title)}</text>"
            f'<text x="{self.left}" y="52" text-anchor="start" '
            'font-family="Inter,Segoe UI,Arial,sans-serif" '
            'font-size="13" fill="#647178">'
            f"{html.escape(self.subtitle)}</text>"
        )
        destination.write_text(header + title + "".join(self.items) + "</svg>")


def render_line_chart(
    destination: Path,
    results: list[Result],
    title: str,
    subtitle: str,
    y_label: str,
    value_getter,
    y_format,
    x_getter,
    x_labels: list[str],
    log_x: bool,
    log_y: bool = False,
) -> None:
    chart = SvgChart(title, subtitle, y_label)
    y_values = [value_getter(result) for result in results]
    if log_y:
        log_y_low = math.log10(min(y_values)) - 0.08
        log_y_high = math.log10(max(y_values)) + 0.08
        for exponent in range(
            math.ceil(log_y_low), math.floor(log_y_high) + 1
        ):
            value = 10**exponent
            y = chart.top + chart.plot_height * (
                1
                - (math.log10(value) - log_y_low)
                / (log_y_high - log_y_low)
            )
            chart.items.append(
                f'<line x1="{chart.left}" y1="{y:.1f}" '
                f'x2="{chart.width - chart.right}" y2="{y:.1f}" '
                'stroke="#d9e0e4" stroke-width="1"/>'
            )
            chart.text(
                chart.left - 12,
                y + 5,
                y_format(value),
                anchor="end",
                size=13,
                fill="#56636a",
            )
        chart.text(
            24,
            chart.top + chart.plot_height / 2,
            chart.y_label,
            size=14,
            weight=600,
            transform=(
                f"rotate(-90 24 "
                f"{chart.top + chart.plot_height / 2:.1f})"
            ),
        )

        def y_position(value: float) -> float:
            return chart.top + chart.plot_height * (
                1
                - (math.log10(value) - log_y_low)
                / (log_y_high - log_y_low)
            )

    else:
        y_max = nice_max(max(y_values) * 1.05)
        chart.axes(y_max, y_format)

        def y_position(value: float) -> float:
            return chart.top + chart.plot_height * (1 - value / y_max)

    chart.legend()

    x_values = sorted({x_getter(result) for result in results})
    if log_x:
        low = math.log10(min(x_values))
        high = math.log10(max(x_values))

        def x_position(value: float) -> float:
            if high == low:
                return chart.left + chart.plot_width / 2
            return chart.left + chart.plot_width * (
                (math.log10(value) - low) / (high - low)
            )

    else:
        low = min(x_values)
        high = max(x_values)

        def x_position(value: float) -> float:
            if high == low:
                return chart.left + chart.plot_width / 2
            return chart.left + chart.plot_width * (
                (value - low) / (high - low)
            )

    for value, label in zip(x_values, x_labels):
        x = x_position(value)
        chart.items.append(
            f'<line x1="{x:.1f}" y1="{chart.top}" x2="{x:.1f}" '
            f'y2="{chart.top + chart.plot_height}" '
            'stroke="#edf1f3" stroke-width="1"/>'
        )
        chart.text(
            x,
            chart.top + chart.plot_height + 27,
            label,
            size=13,
            fill="#56636a",
        )

    chart.text(
        chart.left + chart.plot_width / 2,
        chart.height - 20,
        "Connections" if log_x else "Workers",
        size=14,
        weight=600,
    )

    for server in SERVERS:
        points = sorted(
            (result for result in results if result.server == server),
            key=x_getter,
        )
        coordinates = [
            (
                x_position(x_getter(result)),
                y_position(value_getter(result)),
            )
            for result in points
        ]
        chart.items.append(
            f'<polyline points="{" ".join(f"{x:.1f},{y:.1f}" for x, y in coordinates)}" '
            f'fill="none" stroke="{COLORS[server]}" stroke-width="3"/>'
        )
        for x, y in coordinates:
            chart.items.append(
                f'<circle cx="{x:.1f}" cy="{y:.1f}" r="5" '
                f'fill="#ffffff" stroke="{COLORS[server]}" '
                'stroke-width="3"/>'
            )
    chart.save(destination)


def render_overview(destination: Path, results: list[Result]) -> None:
    chart = SvgChart(
        "HTTP server throughput overview",
        "Median of 3 x 10 s wrk runs; higher is better",
        "Requests / second",
    )
    y_max = nice_max(max(result.rps for result in results) * 1.05)
    chart.axes(y_max, lambda value: f"{value / 1000:.0f}k")
    chart.legend()

    group_width = chart.plot_width / len(CASES)
    bar_width = group_width * 0.17
    by_key = {(result.case, result.server): result for result in results}
    for case_index, case in enumerate(CASES):
        center = chart.left + group_width * (case_index + 0.5)
        for server_index, server in enumerate(SERVERS):
            result = by_key[(case, server)]
            x = center + (server_index - 1.5) * bar_width
            height = chart.plot_height * result.rps / y_max
            y = chart.top + chart.plot_height - height
            chart.items.append(
                f'<rect x="{x - bar_width * 0.42:.1f}" y="{y:.1f}" '
                f'width="{bar_width * 0.84:.1f}" height="{height:.1f}" '
                f'fill="{COLORS[server]}"/>'
            )
        chart.text(
            center,
            chart.top + chart.plot_height + 27,
            case,
            size=12,
            fill="#56636a",
        )
    chart.save(destination)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="benchmark matrix directory")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parent / "results",
    )
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)

    results = [
        parse_case(arguments.input / case, server)
        for case in CASES
        for server in SERVERS
    ]
    write_csv(results, arguments.output / "results.csv")
    render_overview(arguments.output / "throughput-overview.svg", results)

    connection_results = [
        result
        for result in results
        if result.workers == 2 and result.case.startswith("w2-")
    ]
    connections = sorted({result.connections for result in connection_results})
    connection_labels = [str(value) for value in connections]
    render_line_chart(
        arguments.output / "throughput-connections.svg",
        connection_results,
        "Throughput as connection count rises",
        "2 server workers; median of 3 x 10 s wrk runs",
        "Requests / second",
        lambda result: result.rps,
        lambda value: f"{value / 1000:.0f}k",
        lambda result: result.connections,
        connection_labels,
        True,
    )
    render_line_chart(
        arguments.output / "p99-connections.svg",
        connection_results,
        "P99 latency as connection count rises",
        "2 server workers; lower is better",
        "P99 latency (ms)",
        lambda result: result.p99_us / 1000,
        lambda value: f"{value:.1f}",
        lambda result: result.connections,
        connection_labels,
        True,
        True,
    )

    worker_results = [
        result
        for result in results
        if result.connections == 256 and result.case in {
            "w1-c256",
            "w2-c256",
            "w4-c256",
        }
    ]
    workers = sorted({result.workers for result in worker_results})
    render_line_chart(
        arguments.output / "throughput-workers.svg",
        worker_results,
        "Throughput as server workers scale",
        "256 connections and 2 wrk threads; higher is better",
        "Requests / second",
        lambda result: result.rps,
        lambda value: f"{value / 1000:.0f}k",
        lambda result: result.workers,
        [str(value) for value in workers],
        False,
    )


if __name__ == "__main__":
    main()
