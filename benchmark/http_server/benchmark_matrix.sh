#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
output_dir=${OUTPUT_DIR:-"$script_dir/../../benchmark-results/matrix-$(date +%Y%m%d-%H%M%S)"}

runs=${RUNS:-3}
warmup=${WARMUP:-3s}
duration=${DURATION:-10s}
client_cpus=${CLIENT_CPUS:-10,12}

# name|workers|wrk threads|connections|server CPUs
cases=(
  "w2-c1|2|1|1|0,2,4"
  "w2-c16|2|2|16|0,2,4"
  "w2-c64|2|2|64|0,2,4"
  "w2-c256|2|2|256|0,2,4"
  "w2-c1024|2|2|1024|0,2,4"
  "w1-c256|1|2|256|0,2"
  "w4-c256|4|2|256|0,2,4,6,8"
)

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
  WORKERS="$workers" \
  THREADS="$threads" \
  CONNECTIONS="$connections" \
  SERVER_CPUS="$server_cpus" \
  CLIENT_CPUS="$client_cpus" \
  RUNS="$runs" \
  WARMUP="$warmup" \
  DURATION="$duration" \
  ORDER="$order" \
    "$script_dir/benchmark.sh"

  ((index += 1))
done

echo "matrix results: $output_dir"
