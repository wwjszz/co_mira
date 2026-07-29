#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
output_dir=${OUTPUT_DIR:-"$script_dir/../../benchmark-results/wrk-scale-$(date +%Y%m%d-%H%M%S)"}

runs=${RUNS:-3}
warmup=${WARMUP:-3s}
duration=${DURATION:-10s}

# Keep the two server workers and front loop on 0,2,4. Increase the wrk CPU
# set with its thread count, using non-overlapping host-informed vCPU IDs.
cases=(
  "t2-c1024|2|1024|12,14"
  "t4-c1024|4|1024|8,10,12,14"
  "t8-c1024|8|1024|6,8,10,12,14,16,20,24"
  "t2-c2048|2|2048|12,14"
  "t4-c2048|4|2048|8,10,12,14"
  "t8-c2048|8|2048|6,8,10,12,14,16,20,24"
  "t2-c4096|2|4096|12,14"
)

mkdir -p "$output_dir"

index=0
for spec in "${cases[@]}"; do
  IFS='|' read -r name threads connections client_cpus <<<"$spec"
  if ((index % 2 == 0)); then
    order=forward
  else
    order=reverse
  fi

  echo "## $name ($order)"
  OUTPUT_DIR="$output_dir/$name" \
  BUILD_DIR="${BUILD_DIR:-"$script_dir/../../build-benchmark"}" \
  WORKERS=2 \
  THREADS="$threads" \
  CONNECTIONS="$connections" \
  SERVER_CPUS=0,2,4 \
  CLIENT_CPUS="$client_cpus" \
  RUNS="$runs" \
  WARMUP="$warmup" \
  DURATION="$duration" \
  ORDER="$order" \
  PIN_SERVER_THREADS=1 \
    "$script_dir/benchmark.sh"

  ((index += 1))
done

echo "wrk scaling results: $output_dir"
