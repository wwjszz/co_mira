#!/usr/bin/env python3
"""Summarize the wrk thread/connection scaling benchmark."""

from __future__ import annotations

import argparse
from pathlib import Path

import summarize
from summarize import LABELS, SERVERS, parse_case, write_csv

CASES = ("t2-c1024", "t4-c1024", "t2-c2048", "t4-c2048")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="wrk scaling directory")
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)

    results = [
        parse_case(arguments.input / case, server)
        for case in CASES
        for server in SERVERS
    ]
    write_csv(results, arguments.output / "results.csv")

    summarize.CASES = CASES
    summarize.render_overview(
        arguments.output / "throughput-overview.svg", results
    )

    by_key = {(result.case, result.server): result for result in results}
    lines = [
        "| wrk | "
        + " | ".join(LABELS[server] for server in SERVERS)
        + " |",
        "|---|"
        + "---:|" * len(SERVERS),
    ]
    for case in CASES:
        values = " | ".join(
            f"{by_key[(case, server)].rps:,.0f}" for server in SERVERS
        )
        lines.append(f"| `{case}` | {values} |")
    (arguments.output / "throughput-table.md").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
