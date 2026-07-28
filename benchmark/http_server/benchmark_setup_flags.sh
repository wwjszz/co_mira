#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script_dir="$repo_dir/benchmark/http_server"
output_dir=${OUTPUT_DIR:-"$repo_dir/benchmark-results/setup-flags-$(date +%Y%m%d-%H%M%S)"}
work_root=${WORK_ROOT:-"/tmp/co-mira-setup-flags-$UID"}

rounds=${ROUNDS:-5}
warmup=${WARMUP:-2s}
duration=${DURATION:-8s}

if [[ "$work_root" != /tmp/co-mira-setup-flags-* ]]; then
  echo "WORK_ROOT must stay under /tmp/co-mira-setup-flags-*" >&2
  exit 2
fi
if ((rounds < 1 || rounds > 5)); then
  echo "ROUNDS must be in the range 1..5" >&2
  exit 2
fi

rm -rf "$work_root"
mkdir -p "$work_root/source" "$output_dir"
rsync -a \
  --exclude=.git \
  --exclude='build*' \
  --exclude=benchmark-results \
  "$repo_dir/" "$work_root/source/"

header="$work_root/source/include/scheduler.hpp"
template="$work_root/scheduler.hpp.template"
cp "$header" "$template"

# name|IORING_SETUP_* numeric bitmask
variants=(
  "default|0"
  "single|4096"
  "compatible|768"
  "coop|4864"
  "defer|12800"
)

for spec in "${variants[@]}"; do
  IFS='|' read -r name flags <<<"$spec"
  cp "$template" "$header"
  perl -0pi -e \
    "s/constexpr unsigned setup_flags =\n      IORING_SETUP_COOP_TASKRUN \| IORING_SETUP_TASKRUN_FLAG;/constexpr unsigned setup_flags = $flags;/" \
    "$header"
  grep -Fq "constexpr unsigned setup_flags = $flags;" "$header"

  build_dir="$work_root/build-$name"
  cmake -S "$work_root/source" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCO_MIRA_BUILD_EXAMPLES=ON
  cmake --build "$build_dir" -j"$(nproc)" --target co_http_server
done

orders=(
  "default single compatible coop defer"
  "defer coop compatible single default"
  "single defer default coop compatible"
  "coop compatible default defer single"
  "compatible default coop single defer"
)

for ((round = 1; round <= rounds; ++round)); do
  read -r -a names <<<"${orders[$((round - 1))]}"
  for name in "${names[@]}"; do
    echo "## round $round/$rounds: $name"
    SERVERS=coroutine \
    BUILD_DIR="$work_root/build-$name" \
    OUTPUT_DIR="$output_dir/$name/run-$round" \
    WORKERS=2 \
    THREADS=2 \
    CONNECTIONS=1024 \
    SERVER_CPUS=0,2,4 \
    CLIENT_CPUS=12,14 \
    RUNS=1 \
    WARMUP="$warmup" \
    DURATION="$duration" \
    PIN_SERVER_THREADS=1 \
      "$script_dir/benchmark.sh"
  done
done

echo "setup flag results: $output_dir"
