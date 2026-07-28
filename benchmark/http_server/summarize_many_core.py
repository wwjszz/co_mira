#!/usr/bin/env python3
"""Summarize the P/E-core-aware worker scaling benchmark."""

from __future__ import annotations

import argparse
from pathlib import Path

from summarize import SERVERS, LABELS, parse_case, render_line_chart, write_csv

CASES = ("w1-c1024", "w2-c1024", "w4-c1024", "w5-c1024", "w6-c1024", "w8-c1024")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="many-core benchmark directory")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)

    results = [
        parse_case(arguments.input / case, server)
        for case in CASES
        for server in SERVERS
    ]
    write_csv(results, arguments.output / "results.csv")
    workers = sorted({result.workers for result in results})
    labels = [str(worker) for worker in workers]
    subtitle = "1024 connections; 2 wrk threads; worker 6+ starts using E-cores"

    render_line_chart(
        arguments.output / "throughput-workers.svg",
        results,
        "Throughput across P-core and E-core workers",
        subtitle,
        "Requests / second",
        lambda result: result.rps,
        lambda value: f"{value / 1000:.0f}k",
        lambda result: result.workers,
        labels,
        False,
    )
    render_line_chart(
        arguments.output / "p99-workers.svg",
        results,
        "P99 latency across P-core and E-core workers",
        subtitle + "; lower is better",
        "P99 latency (ms)",
        lambda result: result.p99_us / 1000,
        lambda value: f"{value:.1f}",
        lambda result: result.workers,
        labels,
        False,
        True,
    )

    by_key = {(result.workers, result.server): result for result in results}
    lines = [
        "| Workers | " + " | ".join(LABELS[server] for server in SERVERS) + " |",
        "|---:|" + "---:|" * len(SERVERS),
    ]
    for worker in workers:
        values = " | ".join(
            f"{by_key[(worker, server)].rps:,.0f}" for server in SERVERS
        )
        lines.append(f"| {worker} | {values} |")
    (arguments.output / "throughput-table.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()