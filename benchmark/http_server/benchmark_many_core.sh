#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
output_dir=${OUTPUT_DIR:-"$script_dir/../../benchmark-results/many-core-$(date +%Y%m%d-%H%M%S)"}
build_dir=${BUILD_DIR:-"$script_dir/../../build-benchmark"}

runs=${RUNS:-3}
warmup=${WARMUP:-3s}
duration=${DURATION:-10s}
client_cpus=${CLIENT_CPUS:-12,14}
mode=${MODE:-all}

# Host Windows reports CPUs 0-15 as eight P-core SMT threads and 16-31 as
# sixteen E-cores. WSL affinity is virtual, so this is a host-informed layout,
# not a bare-metal P/E pinning guarantee. Keep 12 and 14 for wrk, use one ID
# from each remaining P-core pair first, then spread E workers across clusters.
worker_cases=(
  "w1-c1024|1|2|1024|0,2"
  "w2-c1024|2|2|1024|0,2,4"
  "w4-c1024|4|2|1024|0,2,4,6,8"
  "w5-c1024|5|2|1024|0,2,4,6,8,10"
  "w6-c1024|6|2|1024|0,2,4,6,8,10,16"
  "w8-c1024|8|2|1024|0,2,4,6,8,10,16,20,24"
)
connection_cases=(
  "w8-c64|8|2|64|0,2,4,6,8,10,16,20,24"
  "w8-c256|8|2|256|0,2,4,6,8,10,16,20,24"
  "w8-c1024|8|2|1024|0,2,4,6,8,10,16,20,24"
  "w8-c2048|8|2|2048|0,2,4,6,8,10,16,20,24"
)

case "$mode" in
workers)
  cases=("${worker_cases[@]}")
  ;;
connections)
  cases=("${connection_cases[@]}")
  ;;
all)
  cases=(
    "${worker_cases[@]}"
    "${connection_cases[0]}"
    "${connection_cases[1]}"
    "${connection_cases[3]}"
  )
  ;;
*)
  echo "MODE must be workers, connections or all" >&2
  exit 2
  ;;
esac

mkdir -p "$output_dir"

index=0
for spec in "${cases[@]}"; do
  IFS='|' read -r name workers threads connections server_cpus <<<"$spec"
  if ((index % 2 == 0)); then
    order=forward
  else
    order=reverse
  fi

  echo "## $name ($order)"
  OUTPUT_DIR="$output_dir/$name" \
  BUILD_DIR="$build_dir" \
  WORKERS="$workers" \
  THREADS="$threads" \
  CONNECTIONS="$connections" \
  SERVER_CPUS="$server_cpus" \
  CLIENT_CPUS="$client_cpus" \
  RUNS="$runs" \
  WARMUP="$warmup" \
  DURATION="$duration" \
  ORDER="$order" \
  PIN_SERVER_THREADS=1 \
    "$script_dir/benchmark.sh"

  ((index += 1))
done

echo "many-core results: $output_dir"
