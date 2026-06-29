#!/usr/bin/env sh
set -eu

root="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
installer="$root/scripts/install.sh"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

assert_contains() {
  file="$1"
  text="$2"
  if ! grep -Fq "$text" "$file"; then
    echo "expected '$text' in $file" >&2
    cat "$file" >&2
    exit 1
  fi
}

run_dry() {
  os="$1"
  arch="$2"
  out="$3"
  env \
    KIKO_VERSION=v9.9.9-test \
    KIKO_INSTALL_DIR="$tmp_dir/bin" \
    KIKO_INSTALL_DRY_RUN=1 \
    KIKO_TEST_UNAME_S="$os" \
    KIKO_TEST_UNAME_M="$arch" \
    "$installer" >"$out"
}

run_dry Linux x86_64 "$tmp_dir/linux-x64.out"
assert_contains "$tmp_dir/linux-x64.out" "asset=linux-x64"
assert_contains "$tmp_dir/linux-x64.out" "archive=kiko-v9.9.9-test-linux-x64.tar.gz"

run_dry Linux aarch64 "$tmp_dir/linux-arm64.out"
assert_contains "$tmp_dir/linux-arm64.out" "asset=linux-arm64"
assert_contains "$tmp_dir/linux-arm64.out" "archive=kiko-v9.9.9-test-linux-arm64.tar.gz"

env \
  KIKO_VERSION=v9.9.9-test \
  KIKO_INSTALL_DRY_RUN=1 \
  KIKO_TEST_UNAME_S=Linux \
  KIKO_TEST_UNAME_M=aarch64 \
  TERMUX_VERSION=0.119 \
  PREFIX="$tmp_dir/termux-prefix" \
  "$installer" >"$tmp_dir/termux-arm64.out"
assert_contains "$tmp_dir/termux-arm64.out" "asset=android-arm64"
assert_contains "$tmp_dir/termux-arm64.out" "archive=kiko-v9.9.9-test-android-arm64.tar.gz"
assert_contains "$tmp_dir/termux-arm64.out" "install_dir=$tmp_dir/termux-prefix/bin"
assert_contains "$tmp_dir/termux-arm64.out" "android=1"
assert_contains "$tmp_dir/termux-arm64.out" "termux=1"

if env \
  KIKO_VERSION=v9.9.9-test \
  KIKO_INSTALL_DRY_RUN=1 \
  KIKO_TEST_UNAME_S=Linux \
  KIKO_TEST_UNAME_M=aarch64 \
  KIKO_ASSET=linux-arm64 \
  TERMUX_VERSION=0.119 \
  PREFIX="$tmp_dir/termux-prefix" \
  "$installer" >"$tmp_dir/termux-linux.out" 2>"$tmp_dir/termux-linux.err"; then
  echo "expected linux-arm64 override on Termux to fail" >&2
  exit 1
fi
assert_contains "$tmp_dir/termux-linux.err" "refusing to install linux-arm64 on Android/Termux"
assert_contains "$tmp_dir/termux-linux.err" "Termux source build:"

env \
  KIKO_VERSION=v9.9.9-test \
  KIKO_BUILD_FROM_SOURCE=1 \
  KIKO_INSTALL_DRY_RUN=1 \
  KIKO_TEST_UNAME_S=Linux \
  KIKO_TEST_UNAME_M=aarch64 \
  TERMUX_VERSION=0.119 \
  PREFIX="$tmp_dir/termux-prefix" \
  "$installer" >"$tmp_dir/termux-source.out"
assert_contains "$tmp_dir/termux-source.out" "mode=source"
assert_contains "$tmp_dir/termux-source.out" "source_ref=v9.9.9-test"
assert_contains "$tmp_dir/termux-source.out" "install_dir=$tmp_dir/termux-prefix/bin"
assert_contains "$tmp_dir/termux-source.out" "install_deps=1"

env \
  KIKO_BUILD_FROM_SOURCE=1 \
  KIKO_SOURCE_REF=main \
  KIKO_INSTALL_DEPS=0 \
  KIKO_INSTALL_DRY_RUN=1 \
  KIKO_TEST_UNAME_S=Linux \
  KIKO_TEST_UNAME_M=aarch64 \
  TERMUX_VERSION=0.119 \
  PREFIX="$tmp_dir/termux-prefix" \
  "$installer" >"$tmp_dir/termux-source-ref.out"
assert_contains "$tmp_dir/termux-source-ref.out" "version=main"
assert_contains "$tmp_dir/termux-source-ref.out" "source_ref=main"
assert_contains "$tmp_dir/termux-source-ref.out" "install_deps=0"

run_dry Darwin arm64 "$tmp_dir/macos-arm64.out"
assert_contains "$tmp_dir/macos-arm64.out" "asset=macos-arm64"
assert_contains "$tmp_dir/macos-arm64.out" "archive=kiko-v9.9.9-test-macos-arm64.tar.gz"

env \
  KIKO_VERSION=v9.9.9-test \
  KIKO_INSTALL_DIR="$tmp_dir/bin" \
  KIKO_INSTALL_DRY_RUN=1 \
  KIKO_ADD_TO_PATH=1 \
  KIKO_TEST_UNAME_S=Darwin \
  KIKO_TEST_UNAME_M=arm64 \
  "$installer" >"$tmp_dir/add-path.out"
assert_contains "$tmp_dir/add-path.out" "add_to_path=1"

if run_dry Linux riscv64 "$tmp_dir/unsupported.out" 2>"$tmp_dir/unsupported.err"; then
  echo "expected unsupported architecture to fail" >&2
  exit 1
fi
assert_contains "$tmp_dir/unsupported.err" "unsupported Linux architecture: riscv64"

echo "install script dry-run checks passed"
