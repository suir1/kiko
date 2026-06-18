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
  compose logs --no-color --tail=250 >&2 || true
}

finish() {
  local status=$?
  if [[ "$status" -ne 0 ]]; then
    dump_logs
  fi
  cleanup
  exit "$status"
}

wait_for_service_exit() {
  local service="$1"
  local cid
  cid="$(compose ps -a -q "$service")"
  if [[ -z "$cid" ]]; then
    echo "$service container not found" >&2
    return 1
  fi

  for _ in {1..90}; do
    local running exit_code
    running="$(docker inspect -f '{{.State.Running}}' "$cid")"
    exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$cid")"
    if [[ "$running" == "false" ]]; then
      [[ "$exit_code" == "0" ]]
      return
    fi
    sleep 0.5
  done

  echo "$service did not finish in time" >&2
  return 1
}

assert_service_log_contains() {
  local service="$1"
  local needle="$2"
  local logs
  logs="$(compose logs --no-color "$service")"
  if ! grep -Fq "$needle" <<<"$logs"; then
    echo "expected $service logs to contain: $needle" >&2
    return 1
  fi
}

assert_service_log_not_contains() {
  local service="$1"
  local needle="$2"
  local logs
  logs="$(compose logs --no-color "$service")"
  if grep -Fq "$needle" <<<"$logs"; then
    echo "expected $service logs not to contain: $needle" >&2
    return 1
  fi
}

run_same_lan_direct() {
  echo "== same LAN: expect direct TCP =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-lan
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  compose run --rm --no-deps sender-lan
  wait_for_service_exit receiver-lan
  compose run --rm --no-deps verifier-lan
  assert_service_log_contains receiver-lan "direct connection established"
  assert_service_log_not_contains receiver-lan "direct failed, using relay"
  echo "PASS: same LAN direct TCP"
}

run_same_lan_isolated_relay_fallback() {
  echo "== same LAN isolated: expect relay fallback =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-lan-isolated
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  compose run --rm --no-deps sender-lan-isolated
  wait_for_service_exit receiver-lan-isolated
  compose run --rm --no-deps verifier-lan-isolated
  assert_service_log_contains receiver-lan-isolated "direct failed, using relay"
  assert_service_log_contains receiver-lan-isolated "pake handshake ok"
  echo "PASS: same LAN isolated relay fallback"
}

run_one_side_nat_direct() {
  echo "== one side NAT: expect direct TCP =="
  compose up -d --no-build relay nat-a
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-public
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  compose run --rm --no-deps sender-one-nat
  wait_for_service_exit receiver-public
  compose run --rm --no-deps verifier-one-nat
  assert_service_log_contains receiver-public "direct connection established"
  assert_service_log_not_contains receiver-public "direct failed, using relay"
  echo "PASS: one side NAT direct TCP"
}

run_double_nat_same_port_direct() {
  echo "== double NAT: expect synchronized same-port direct TCP =="
  compose up -d --no-build relay nat-a nat-b
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  compose run --rm --no-deps sender
  wait_for_service_exit receiver
  compose run --rm --no-deps verifier
  assert_service_log_contains receiver "direct connection established"
  assert_service_log_contains receiver "route detail: direct_success kind=public-same-port"
  assert_service_log_contains receiver "pake handshake ok"
  echo "PASS: double NAT synchronized same-port direct TCP"
}

run_outbound_physical_fallback() {
  echo "== fake VPN route: expect physical outbound fallback =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  local output
  output="$(compose run --rm --no-deps doctor-outbound-physical)"
  printf '%s\n' "$output"
  if ! grep -Fq '"outbound_path": "physical"' <<<"$output"; then
    echo "expected doctor output to choose physical outbound path" >&2
    return 1
  fi
  if ! grep -Fq '"outbound_reason": "default_failed_physical_ok"' <<<"$output"; then
    echo "expected doctor output to explain default failure and physical success" >&2
    return 1
  fi
  echo "PASS: fake VPN route physical outbound fallback"
}

run_outbound_manual_overrides() {
  echo "== fake VPN route: expect manual outbound overrides =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"

  local avoid_output
  avoid_output="$(compose run --rm --no-deps doctor-outbound-avoid-vpn)"
  printf '%s\n' "$avoid_output"
  if ! grep -Fq '"outbound_path": "physical"' <<<"$avoid_output"; then
    echo "expected --avoid-vpn to choose physical outbound path" >&2
    return 1
  fi
  if ! grep -Fq '"outbound_reason": "avoid_vpn"' <<<"$avoid_output"; then
    echo "expected --avoid-vpn reason" >&2
    return 1
  fi
  if ! grep -Fq '"bound_interface": "eth0"' <<<"$avoid_output"; then
    echo "expected --avoid-vpn to bind eth0" >&2
    return 1
  fi

  local bind_output
  bind_output="$(compose run --rm --no-deps doctor-outbound-bind-eth0)"
  printf '%s\n' "$bind_output"
  if ! grep -Fq '"outbound_path": "forced"' <<<"$bind_output"; then
    echo "expected --bind-interface to force outbound path" >&2
    return 1
  fi
  if ! grep -Fq '"outbound_reason": "user_forced_interface"' <<<"$bind_output"; then
    echo "expected --bind-interface reason" >&2
    return 1
  fi
  if ! grep -Fq '"bound_interface": "eth0"' <<<"$bind_output"; then
    echo "expected --bind-interface eth0 to bind eth0" >&2
    return 1
  fi

  echo "PASS: fake VPN manual outbound overrides"
}

run_outbound_physical_transfer() {
  echo "== fake VPN route transfer: expect relay over physical outbound path =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-outbound-physical
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  local sender_output
  sender_output="$(compose run --rm --no-deps sender-outbound-physical)"
  printf '%s\n' "$sender_output"
  wait_for_service_exit receiver-outbound-physical
  compose run --rm --no-deps verifier-outbound-physical
  if ! grep -Fq "outbound interface: eth0 (default_failed_physical_ok)" <<<"$sender_output"; then
    echo "expected sender output to choose eth0 physical outbound path" >&2
    return 1
  fi
  assert_service_log_contains receiver-outbound-physical "outbound interface: eth0 (default_failed_physical_ok)"
  assert_service_log_contains receiver-outbound-physical "direct skipped, using relay"
  assert_service_log_contains receiver-outbound-physical "opening 4 parallel relay connections"
  echo "PASS: fake VPN route transfer over physical outbound path"
}

run_outbound_sender_only_transfer() {
  echo "== fake VPN sender only: expect sender physical, receiver default =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-outbound-physical-sender-only
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  local sender_output
  sender_output="$(compose run --rm --no-deps sender-outbound-physical-sender-only)"
  printf '%s\n' "$sender_output"
  wait_for_service_exit receiver-outbound-physical-sender-only
  compose run --rm --no-deps verifier-outbound-sender-only
  if ! grep -Fq "outbound interface: eth0 (default_failed_physical_ok)" <<<"$sender_output"; then
    echo "expected sender-only scenario sender to choose eth0" >&2
    return 1
  fi
  assert_service_log_contains receiver-outbound-physical-sender-only "outbound path: default (default)"
  assert_service_log_not_contains receiver-outbound-physical-sender-only "outbound interface: eth0"
  assert_service_log_contains receiver-outbound-physical-sender-only "direct skipped, using relay"
  echo "PASS: fake VPN sender-only relay transfer"
}

run_outbound_receiver_only_transfer() {
  echo "== fake VPN receiver only: expect sender default, receiver physical =="
  compose up -d --no-build relay
  sleep "${NETLAB_INFRA_DELAY:-2}"
  compose up -d --no-build receiver-outbound-physical-receiver-only
  sleep "${NETLAB_RECEIVER_DELAY:-2}"
  local sender_output
  sender_output="$(compose run --rm --no-deps sender-outbound-physical-receiver-only)"
  printf '%s\n' "$sender_output"
  wait_for_service_exit receiver-outbound-physical-receiver-only
  compose run --rm --no-deps verifier-outbound-receiver-only
  if ! grep -Fq "outbound path: default (default)" <<<"$sender_output"; then
    echo "expected receiver-only scenario sender to keep default outbound path" >&2
    return 1
  fi
  if grep -Fq "outbound interface: eth0" <<<"$sender_output"; then
    echo "expected receiver-only scenario sender not to bind eth0" >&2
    return 1
  fi
  assert_service_log_contains receiver-outbound-physical-receiver-only "outbound interface: eth0 (default_failed_physical_ok)"
  assert_service_log_contains receiver-outbound-physical-receiver-only "direct skipped, using relay"
  echo "PASS: fake VPN receiver-only relay transfer"
}

trap finish EXIT

if [[ "${NETLAB_SKIP_BUILD:-0}" != "1" ]]; then
  compose build relay
fi
run_outbound_physical_fallback
run_outbound_manual_overrides
run_outbound_physical_transfer
run_outbound_sender_only_transfer
run_outbound_receiver_only_transfer
run_same_lan_direct
run_same_lan_isolated_relay_fallback
run_one_side_nat_direct
run_double_nat_same_port_direct

echo "PASS: docker netlab matrix"
