#!/usr/bin/env bash

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"
server_host="127.0.0.1"
server_port="$((18000 + (${RANDOM:-0} % 10000)))"
clock_url="http://${server_host}:${server_port}/clock"
server_log="$(mktemp /tmp/memory-clock-server.XXXXXX.log)"

server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]] && kill -0 "$server_pid" 2>/dev/null; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
}

trap cleanup EXIT INT TERM

cd "$repo_root"
/usr/bin/python3 "$script_dir/clock_server.py" --host "$server_host" --port "$server_port" >"$server_log" 2>&1 &
server_pid="$!"

ready=0
for _ in $(seq 1 150); do
  if ! kill -0 "$server_pid" 2>/dev/null; then
    break
  fi
  status="$(curl -s -o /dev/null -w '%{http_code}' "$clock_url" || true)"
  if [[ "$status" == "401" || "$status" == "403" || "$status" == "200" ]]; then
    ready=1
    break
  fi
  sleep 0.1
done

if [[ "$ready" != "1" ]]; then
  echo "server did not become ready: $clock_url" >&2
  if [[ -s "$server_log" ]]; then
    echo "--- server log ---" >&2
    cat "$server_log" >&2
  fi
  exit 1
fi

dump_dir="$(/usr/bin/python3 "$script_dir/dump_clock_pages.py" --url "$clock_url")"
printf '%s\n' "$dump_dir"

eog "$dump_dir"
