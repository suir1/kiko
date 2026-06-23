#!/usr/bin/env sh
set -eu

ROLE="${NETLAB_ROLE:?NETLAB_ROLE is required}"
CODE="${NETLAB_CODE:-netlab-code}"
RELAY="${NETLAB_RELAY:-172.28.0.10:9000}"
TEXT="${NETLAB_TEXT:-hello from kiko docker netlab}"
TIMEOUT="${NETLAB_TIMEOUT:-45}"
DATA_DIR="${NETLAB_DATA_DIR:-/data}"
OUT_DIR="${NETLAB_OUT_DIR:-/out}"
COMMON_FLAGS="${NETLAB_COMMON_FLAGS---no-lan --no-local}"
SEND_FLAGS="${NETLAB_SEND_FLAGS-$COMMON_FLAGS --no-qrcode}"
RECV_FLAGS="${NETLAB_RECV_FLAGS-$COMMON_FLAGS}"
DOCTOR_FLAGS="${NETLAB_DOCTOR_FLAGS:-}"
CONNECTIONS="${NETLAB_CONNECTIONS:-}"

for peer_ip in $(printf '%s' "${NETLAB_BLOCK_TCP_PEERS:-}" | tr ',' ' '); do
  [ -n "$peer_ip" ] || continue
  iptables -A OUTPUT -p tcp -d "$peer_ip" -j REJECT --reject-with tcp-reset
  iptables -A INPUT -p tcp -s "$peer_ip" -j REJECT --reject-with tcp-reset
done

if [ "${NETLAB_FAKE_VPN:-0}" = "1" ]; then
  vpn_if="${NETLAB_FAKE_VPN_IF:-tun0}"
  ip link add "$vpn_if" type dummy 2>/dev/null || true
  ip link set "$vpn_if" up
fi

if [ -n "${NETLAB_GATEWAY:-}" ]; then
  ip route replace default via "$NETLAB_GATEWAY"
fi

if [ "${NETLAB_ROUTE_RELAY_VIA_FAKE_VPN:-0}" = "1" ]; then
  vpn_if="${NETLAB_FAKE_VPN_IF:-tun0}"
  relay_host="${RELAY%:*}"
  relay_host="${relay_host#[}"
  relay_host="${relay_host%]}"
  ip route replace "$relay_host/32" dev "$vpn_if"
fi

if [ -n "${NETLAB_NETEM_DELAY:-}${NETLAB_NETEM_LOSS:-}${NETLAB_NETEM_RATE:-}" ]; then
  netem_if="${NETLAB_NETEM_IF:-eth0}"
  set -- tc qdisc replace dev "$netem_if" root netem
  if [ -n "${NETLAB_NETEM_DELAY:-}" ]; then
    set -- "$@" delay "$NETLAB_NETEM_DELAY"
  fi
  if [ -n "${NETLAB_NETEM_LOSS:-}" ]; then
    set -- "$@" loss "$NETLAB_NETEM_LOSS"
  fi
  if [ -n "${NETLAB_NETEM_RATE:-}" ]; then
    set -- "$@" rate "$NETLAB_NETEM_RATE"
  fi
  "$@"
fi

case "$ROLE" in
  relay)
    exec kiko relay --listen 0.0.0.0:9000
    ;;
  doctor)
    set -- kiko doctor --relay "$RELAY" --json
    # Scenario flags are intentionally simple whitespace-separated CLI words.
    # shellcheck disable=SC2086
    set -- "$@" $DOCTOR_FLAGS
    exec timeout "$TIMEOUT" "$@"
    ;;
  receiver)
    rm -rf "$OUT_DIR"/*
    set -- kiko recv "$CODE" --relay "$RELAY" --out "$OUT_DIR"
    # Scenario flags are intentionally simple whitespace-separated CLI words.
    # shellcheck disable=SC2086
    set -- "$@" $RECV_FLAGS
    exec timeout "$TIMEOUT" "$@"
    ;;
  sender)
    mkdir -p "$DATA_DIR"
    printf '%s\n' "$TEXT" > "$DATA_DIR/input.txt"
    sleep "${NETLAB_START_DELAY:-1}"
    set -- kiko send "$DATA_DIR/input.txt" --relay "$RELAY" --code "$CODE"
    if [ -n "$CONNECTIONS" ]; then
      set -- "$@" --connections "$CONNECTIONS"
    fi
    # Scenario flags are intentionally simple whitespace-separated CLI words.
    # shellcheck disable=SC2086
    set -- "$@" $SEND_FLAGS
    exec timeout "$TIMEOUT" "$@"
    ;;
  verifier)
    printf '%s\n' "$TEXT" > /tmp/expected.txt
    test -f "$OUT_DIR/input.txt"
    cmp /tmp/expected.txt "$OUT_DIR/input.txt"
    ;;
  *)
    echo "unknown NETLAB_ROLE=$ROLE" >&2
    exit 2
    ;;
esac
