#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE_FILE="$ROOT/docker-compose.netlab.yml"
PROJECT="kiko-netlab-${RANDOM}-${RANDOM}"

compose() {
  docker compose -p "$PROJECT" -f "$COMPOSE_FILE" "$@"
}

cleanup() {
  compose down -v --remove-orphans >/dev/null 2>&1 || true
}

dump_logs() {
  echo "---- docker netlab containers ----" >&2
  compose ps >&2 || true
  echo "---- docker netlab logs ----" >&2
  compose logs --no-color --tail=200 >&2 || true
}

finish() {
  local status=$?
  if [[ "$status" -ne 0 ]]; then
    dump_logs
  fi
  cleanup
  exit "$status"
}

wait_for_receiver() {
  local cid
  cid="$(compose ps -a -q receiver)"
  if [[ -z "$cid" ]]; then
    echo "receiver container not found" >&2
    return 1
  fi

  for _ in {1..60}; do
    local running exit_code
    running="$(docker inspect -f '{{.State.Running}}' "$cid")"
    exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$cid")"
    if [[ "$running" == "false" ]]; then
      [[ "$exit_code" == "0" ]]
      return
    fi
    sleep 0.5
  done

  echo "receiver did not finish in time" >&2
  return 1
}

trap finish EXIT

if [[ "${NETLAB_SKIP_BUILD:-0}" != "1" ]]; then
  compose build relay
fi
compose up -d --no-build relay nat-a nat-b
sleep "${NETLAB_INFRA_DELAY:-2}"
compose up -d --no-build receiver
sleep "${NETLAB_RECEIVER_DELAY:-2}"
compose run --rm --no-deps sender
wait_for_receiver
compose run --rm --no-deps verifier

echo "PASS: docker netlab double NAT relay fallback"
