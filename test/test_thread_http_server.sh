#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 SERVER CURL" >&2
  exit 2
fi

server=$1
curl=$2
temporary=$(mktemp -d)
log="$temporary/server.log"
body="$temporary/body.txt"
headers="$temporary/headers.txt"

"$server" 0 16 >"$log" 2>&1 &
pid=$!

cleanup() {
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$temporary"
}
trap cleanup EXIT INT TERM

port=
attempt=0
while [ "$attempt" -lt 100 ]; do
  port=$(sed -n \
    's/.*listening on http:\/\/[^:]*:\([0-9][0-9]*\) with.*/\1/p' \
    "$log" | head -n 1)
  if [ -n "$port" ] &&
     "$curl" -fsS "http://127.0.0.1:$port/benchmark" >/dev/null 2>&1; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.05
done

if [ -z "$port" ] || [ "$attempt" -eq 100 ]; then
  echo "thread HTTP server did not become ready"
  sed -n '1,200p' "$log"
  exit 1
fi

"$curl" -fsS -D "$headers" \
  "http://127.0.0.1:$port/benchmark" -o "$body"
grep -qx 'OK' "$body"
grep -qi 'Content-Type: text/plain; charset=utf-8' "$headers"
grep -qi 'Content-Length: 3' "$headers"
grep -qi 'Connection: keep-alive' "$headers"

"$curl" -fsSI "http://127.0.0.1:$port/benchmark" |
  grep -qi 'Content-Length: 3'

not_found=$("$curl" -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:$port/missing")
[ "$not_found" = 404 ]

seq 1 200 | xargs -P 16 -I '{}' \
  "$curl" -fsS "http://127.0.0.1:$port/benchmark" -o /dev/null

kill -TERM "$pid"
attempt=0
while kill -0 "$pid" 2>/dev/null && [ "$attempt" -lt 100 ]; do
  attempt=$((attempt + 1))
  sleep 0.05
done

if kill -0 "$pid" 2>/dev/null; then
  echo "thread HTTP server did not stop after SIGTERM"
  sed -n '1,200p' "$log"
  exit 1
fi

wait "$pid"
trap - EXIT INT TERM
rm -rf "$temporary"
echo "thread HTTP integration test passed"
