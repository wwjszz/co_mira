#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
  echo "usage: $0 SERVER ASSET_ROOT CURL" >&2
  exit 2
fi

server=$1
asset_root=$2
curl=$3
temporary=$(mktemp -d)
log="$temporary/server.log"
home="$temporary/home.html"
headers="$temporary/headers.txt"
download="$temporary/download.txt"

"$server" 0 2 "$asset_root" >"$log" 2>&1 &
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
     "$curl" -fsS "http://127.0.0.1:$port/api/status" >/dev/null 2>&1; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 0.05
done

if [ -z "$port" ] || [ "$attempt" -eq 100 ]; then
  echo "server did not become ready"
  sed -n '1,200p' "$log"
  exit 1
fi

"$curl" -fsS "http://127.0.0.1:$port/" -o "$home"
grep -q "Small runtime" "$home"
"$curl" -fsS "http://127.0.0.1:$port/assets/style.css" |
  grep -q -- "--green"
"$curl" -fsS "http://127.0.0.1:$port/assets/app.js" |
  grep -q "refreshStatus"
"$curl" -fsS "http://127.0.0.1:$port/api/status?fresh=1" |
  grep -q '"status": "ok"'

"$curl" -fsS -D "$headers" \
  "http://127.0.0.1:$port/benchmark" \
  -o "$download"
grep -qx 'OK' "$download"
grep -qi 'Content-Type: text/plain; charset=utf-8' "$headers"
grep -qi 'Content-Length: 3' "$headers"
grep -qi 'Cache-Control: no-store' "$headers"

"$curl" -fsS -D "$headers" \
  "http://127.0.0.1:$port/download/co-mira-starter.txt" \
  -o "$download"
grep -qi \
  'Content-Disposition: attachment; filename="co-mira-starter.txt"' \
  "$headers"
cmp "$download" "$asset_root/downloads/co-mira-starter.txt"

"$curl" -fsSI "http://127.0.0.1:$port/download/sample-data.json" |
  grep -qi 'Content-Type: application/json'

not_found=$("$curl" -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:$port/missing")
[ "$not_found" = 404 ]

traversal=$("$curl" --path-as-is -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:$port/../CMakeLists.txt")
[ "$traversal" = 404 ]

method=$("$curl" -sS -X POST -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:$port/")
[ "$method" = 405 ]

seq 1 200 | xargs -P 16 -I '{}' \
  "$curl" -fsS "http://127.0.0.1:$port/benchmark?request={}" \
  -o /dev/null

kill -TERM "$pid"
attempt=0
while kill -0 "$pid" 2>/dev/null && [ "$attempt" -lt 100 ]; do
  attempt=$((attempt + 1))
  sleep 0.05
done

if kill -0 "$pid" 2>/dev/null; then
  echo "server did not stop after SIGTERM"
  sed -n '1,240p' "$log"
  exit 1
fi

wait "$pid"
trap - EXIT INT TERM
rm -rf "$temporary"
echo "HTTP integration test passed"
