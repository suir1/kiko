#!/usr/bin/env bash
set -euo pipefail

max_glibc="${KIKO_MAX_GLIBC:-2.35}"
max_glibcxx="${KIKO_MAX_GLIBCXX:-3.4.30}"

version_greater_than() {
  local actual="$1"
  local allowed="$2"
  [[ "$actual" != "$allowed" ]] &&
    [[ "$(printf '%s\n%s\n' "$actual" "$allowed" | sort -V | tail -n 1)" == "$actual" ]]
}

required_version() {
  local binary="$1"
  local prefix="$2"
  strings "$binary" |
    sed -nE "s/^${prefix}_([0-9]+(\.[0-9]+)+)$/\1/p" |
    sort -Vu |
    tail -n 1
}

if [[ "$#" -eq 0 ]]; then
  echo "usage: check_linux_abi.sh BINARY..." >&2
  exit 2
fi

for binary in "$@"; do
  if [[ ! -x "$binary" ]]; then
    echo "error: executable not found: $binary" >&2
    exit 1
  fi

  glibc="$(required_version "$binary" GLIBC)"
  glibcxx="$(required_version "$binary" GLIBCXX)"
  if [[ -z "$glibc" || -z "$glibcxx" ]]; then
    echo "error: could not determine ABI requirements for $binary" >&2
    exit 1
  fi

  echo "$binary: GLIBC_$glibc GLIBCXX_$glibcxx"
  if version_greater_than "$glibc" "$max_glibc"; then
    echo "error: $binary requires GLIBC_$glibc; maximum is GLIBC_$max_glibc" >&2
    exit 1
  fi
  if version_greater_than "$glibcxx" "$max_glibcxx"; then
    echo "error: $binary requires GLIBCXX_$glibcxx; maximum is GLIBCXX_$max_glibcxx" >&2
    exit 1
  fi
done
