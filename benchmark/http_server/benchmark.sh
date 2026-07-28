#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build_dir=${BUILD_DIR:-"$repo_dir/build-benchmark"}
wrk=${WRK:-"$HOME/.local/wrk/root/usr/bin/wrk"}
wrk_library_dir=${WRK_LIBRARY_DIR:-"$HOME/.local/wrk/root/usr/lib/x86_64-linux-gnu"}
output_dir=${OUTPUT_DIR:-"$repo_dir/benchmark-results/$(date +%Y%m%d-%H%M%S)"}

server_cpus=${SERVER_CPUS:-0,2,4}
client_cpus=${CLIENT_CPUS:-6,8}
workers=${WORKERS:-2}
threads=${THREADS:-2}
connections=${CONNECTIONS:-64}
warmup=${WARMUP:-5s}
duration=${DURATION:-15s}
runs=${RUNS:-5}
order=${ORDER:-forward}
pin_server_threads=${PIN_SERVER_THREADS:-1}
servers=${SERVERS:-all}

mkdir -p "$output_dir"

server_pid=
server_wrapper_pid=
cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill -TERM "$server_pid" 2>/dev/null || true
  fi
  if [[ -n "$server_wrapper_pid" ]]; then
    wait "$server_wrapper_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

wait_until_ready() {
  local port=$1
  for _ in $(seq 1 100); do
    if curl --silent --fail --output /dev/null \
        "http://127.0.0.1:$port/benchmark"; then
      return
    fi
    sleep 0.05
  done
  echo "server on port $port did not become ready" >&2
  return 1
}

run_wrk() {
  local duration_arg=$1
  local output=$2
  LD_LIBRARY_PATH="$wrk_library_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    taskset -c "$client_cpus" "$wrk" \
      -t"$threads" -c"$connections" -d"$duration_arg" --latency \
      "http://127.0.0.1:$3/benchmark" >"$output"
}

pin_server_thread_set() {
  local name=$1
  local execution_threads=$((workers + 1))
  local expected_threads=$execution_threads
  local -a cpus tids
  IFS=',' read -r -a cpus <<<"$server_cpus"
  if [[ ${#cpus[@]} -ne $execution_threads ]]; then
    echo "$name needs $execution_threads server CPUs, got ${#cpus[@]}" >&2
    return 1
  fi
  if [[ "$name" == coroutine ]]; then
    expected_threads=$((execution_threads + 1))
  fi

  for _ in $(seq 1 100); do
    mapfile -t tids < <(
      find "/proc/$server_pid/task" -mindepth 1 -maxdepth 1 \
        -printf '%f\n' 2>/dev/null | sort -n
    )
    if [[ ${#tids[@]} -eq $expected_threads ]]; then
      break
    fi
    sleep 0.01
  done
  if [[ ${#tids[@]} -ne $expected_threads ]]; then
    echo "$name expected $expected_threads threads, found ${#tids[@]}" >&2
    return 1
  fi

  : >"$output_dir/$name-affinity.txt"
  if [[ "$name" == coroutine ]]; then
    taskset -pc "${cpus[0]}" "${tids[0]}" \
      >>"$output_dir/$name-affinity.txt"
    printf 'role=coordinator tid=%s cpu=%s\n' \
      "${tids[0]}" "${cpus[0]}" \
      >>"$output_dir/$name-affinity.txt"

    local front_index=$((${#tids[@]} - 1))
    taskset -pc "${cpus[0]}" "${tids[$front_index]}" \
      >>"$output_dir/$name-affinity.txt"
    printf 'role=front tid=%s cpu=%s\n' \
      "${tids[$front_index]}" "${cpus[0]}" \
      >>"$output_dir/$name-affinity.txt"

    for ((index = 0; index < workers; ++index)); do
      taskset -pc "${cpus[$((index + 1))]}" "${tids[$((index + 1))]}" \
        >>"$output_dir/$name-affinity.txt"
      printf 'role=worker%s tid=%s cpu=%s\n' "$index" \
        "${tids[$((index + 1))]}" "${cpus[$((index + 1))]}" \
        >>"$output_dir/$name-affinity.txt"
    done
    return
  fi

  for index in "${!tids[@]}"; do
    taskset -pc "${cpus[$index]}" "${tids[$index]}" \
      >>"$output_dir/$name-affinity.txt"
    if ((index == 0)); then
      role=front
    else
      role=worker$((index - 1))
    fi
    printf 'role=%s tid=%s cpu=%s\n' "$role" \
      "${tids[$index]}" "${cpus[$index]}" \
      >>"$output_dir/$name-affinity.txt"
  done
}
run_server() {
  local name=$1
  local binary=$2
  local port=$3

  echo "== $name =="
  if [[ -x /usr/bin/time ]]; then
    /usr/bin/time -v -o "$output_dir/$name-time.txt" \
      taskset -c "$server_cpus" \
      "$binary" "$port" "$workers" \
      >"$output_dir/$name-server.log" 2>&1 &
    server_wrapper_pid=$!

    for _ in $(seq 1 100); do
      server_pid=$(pgrep -P "$server_wrapper_pid" | head -n 1 || true)
      if [[ -n "$server_pid" ]]; then
        break
      fi
      sleep 0.01
    done
  else
    taskset -c "$server_cpus" \
      "$binary" "$port" "$workers" \
      >"$output_dir/$name-server.log" 2>&1 &
    server_wrapper_pid=$!
    server_pid=$server_wrapper_pid
  fi
  if [[ -z "$server_pid" ]]; then
    echo "could not locate the $name server process" >&2
    return 1
  fi

  wait_until_ready "$port"
  if [[ "$pin_server_threads" == 1 ]]; then
    pin_server_thread_set "$name"
  fi
  run_wrk "$warmup" "$output_dir/$name-warmup.txt" "$port"
  for run in $(seq 1 "$runs"); do
    echo "  run $run/$runs"
    run_wrk "$duration" "$output_dir/$name-run-$run.txt" "$port"
  done

  kill -TERM "$server_pid"
  wait "$server_wrapper_pid"
  server_pid=
  server_wrapper_pid=
}

cat >"$output_dir/config.txt" <<EOF
server_cpus=$server_cpus
client_cpus=$client_cpus
workers=$workers
threads=$threads
connections=$connections
warmup=$warmup
duration=$duration
runs=$runs
order=$order
pin_server_threads=$pin_server_threads
servers=$servers
EOF

case "$servers" in
coroutine)
  run_server coroutine "$build_dir/example/co_http_server" 18080
  exit 0
  ;;
callback)
  run_server callback "$build_dir/example/callback_http_server" 18081
  exit 0
  ;;
epoll)
  run_server epoll "$build_dir/example/epoll_http_server" 18082
  exit 0
  ;;
blocking)
  run_server blocking "$build_dir/example/thread_http_server" 18083
  exit 0
  ;;
all)
  ;;
*)
  echo "SERVERS must be all, coroutine, callback, epoll or blocking" >&2
  exit 2
  ;;
esac
case "$order" in
forward)
  run_server coroutine "$build_dir/example/co_http_server" 18080
  run_server callback "$build_dir/example/callback_http_server" 18081
  run_server epoll "$build_dir/example/epoll_http_server" 18082
  run_server blocking "$build_dir/example/thread_http_server" 18083
  ;;
reverse)
  run_server blocking "$build_dir/example/thread_http_server" 18083
  run_server epoll "$build_dir/example/epoll_http_server" 18082
  run_server callback "$build_dir/example/callback_http_server" 18081
  run_server coroutine "$build_dir/example/co_http_server" 18080
  ;;
*)
  echo "ORDER must be forward or reverse" >&2
  exit 2
  ;;
esac

echo "results: $output_dir"
