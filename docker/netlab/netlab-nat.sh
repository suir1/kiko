#!/usr/bin/env sh
set -eu

PUBLIC_PREFIX="${PUBLIC_PREFIX:-172.28.0.}"

if [ -z "${PUBLIC_IF:-}" ]; then
  PUBLIC_IF="$(ip -o -4 addr show scope global | awk -v prefix="$PUBLIC_PREFIX" 'index($4, prefix) == 1 { print $2; exit }')"
fi

if [ -z "${PRIVATE_IF:-}" ]; then
  PRIVATE_IF="$(ip -o -4 addr show scope global | awk -v public_if="$PUBLIC_IF" '$2 != public_if { print $2; exit }')"
fi

if [ -z "$PUBLIC_IF" ] || [ -z "$PRIVATE_IF" ]; then
  echo "failed to detect NAT interfaces: private=$PRIVATE_IF public=$PUBLIC_IF" >&2
  ip -o -4 addr show scope global >&2 || true
  exit 2
fi

sysctl -w net.ipv4.ip_forward=1 >/dev/null

iptables -t nat -F
iptables -F FORWARD
iptables -P FORWARD DROP
iptables -t nat -A POSTROUTING -o "$PUBLIC_IF" -j MASQUERADE
iptables -A FORWARD -i "$PRIVATE_IF" -o "$PUBLIC_IF" -j ACCEPT
iptables -A FORWARD -i "$PUBLIC_IF" -o "$PRIVATE_IF" -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT

sleep infinity
