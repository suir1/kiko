#!/usr/bin/env sh
set -eu

repo="${KIKO_REPO:-suir1/kiko}"
version="${KIKO_VERSION:-}"
dry_run="${KIKO_INSTALL_DRY_RUN:-}"
asset_override="${KIKO_ASSET:-}"
allow_android_linux="${KIKO_ALLOW_LINUX_ON_ANDROID:-}"
termux_prefix="${PREFIX:-}"

is_termux=0
if [ -n "${TERMUX_VERSION:-}" ] || [ -d "/data/data/com.termux/files/usr" ]; then
  is_termux=1
fi

is_android=0
if [ -n "${ANDROID_ROOT:-}" ] || [ "$is_termux" = "1" ]; then
  is_android=1
fi

if [ -n "${KIKO_INSTALL_DIR:-}" ]; then
  install_dir="$KIKO_INSTALL_DIR"
elif [ "$is_termux" = "1" ] && [ -n "$termux_prefix" ]; then
  install_dir="$termux_prefix/bin"
else
  install_dir="$HOME/.local/bin"
fi

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command not found: $1" >&2
    exit 1
  fi
}

die() {
  echo "error: $*" >&2
  exit 1
}

need curl

termux_source_hint() {
  cat >&2 <<'EOF'
Termux source build:
  pkg update
  pkg install -y git clang cmake ninja pkg-config libsodium zstd
  git clone https://github.com/suir1/kiko.git
  cd kiko
  cmake --preset system-deps
  cmake --build build --target kiko
  mkdir -p "$PREFIX/bin"
  cp build/kiko "$PREFIX/bin/kiko"
  chmod +x "$PREFIX/bin/kiko"

If you have a custom Android/Termux release asset, retry with KIKO_ASSET=android-arm64.
EOF
}

if [ -z "$version" ]; then
  version="$(
    curl -fsSL \
      -H "Accept: application/vnd.github+json" \
      -H "User-Agent: kiko-install" \
      "https://api.github.com/repos/$repo/releases" |
      sed -n 's/^[[:space:]]*"tag_name": "\(v[^"]*\)".*/\1/p' |
      head -n 1
  )" || version=""
fi

if [ -z "$version" ]; then
  version="$(
    curl -fsSL "https://github.com/$repo/releases.atom" |
      sed -n 's:.*<title>\(v[^<]*\)</title>.*:\1:p' |
      head -n 1
  )" || version=""
fi

if [ -z "$version" ]; then
  echo "error: could not determine latest kiko release from https://github.com/$repo/releases" >&2
  echo "hint: set KIKO_VERSION=v0.1.6-alpha and retry" >&2
  exit 1
fi

os_name="${KIKO_TEST_UNAME_S:-$(uname -s)}"
arch_name="${KIKO_TEST_UNAME_M:-$(uname -m)}"

if [ -n "$asset_override" ]; then
  asset="$asset_override"
else
  case "$os_name" in
    Darwin)
      case "$arch_name" in
        arm64) asset="macos-arm64" ;;
        x86_64) asset="macos-x64" ;;
        *) die "unsupported macOS architecture: $arch_name" ;;
      esac
      ;;
    Linux)
      if [ "$is_android" = "1" ]; then
        case "$arch_name" in
          aarch64 | arm64) asset="android-arm64" ;;
          *) die "unsupported Android/Termux architecture: $arch_name" ;;
        esac
      else
        case "$arch_name" in
          x86_64 | amd64) asset="linux-x64" ;;
          aarch64 | arm64) asset="linux-arm64" ;;
          *) die "unsupported Linux architecture: $arch_name" ;;
        esac
      fi
      ;;
    *)
      die "unsupported OS: $os_name"
      ;;
  esac
fi

if [ "$is_android" = "1" ] && [ -z "$allow_android_linux" ]; then
  case "$asset" in
    linux-*)
      echo "error: refusing to install $asset on Android/Termux; glibc Linux binaries are not compatible with Termux." >&2
      termux_source_hint
      exit 1
      ;;
  esac
fi

archive="kiko-$version-$asset.tar.gz"
url="https://github.com/$repo/releases/download/$version/$archive"

if [ -n "$dry_run" ]; then
  echo "version=$version"
  echo "asset=$asset"
  echo "archive=$archive"
  echo "url=$url"
  echo "install_dir=$install_dir"
  echo "android=$is_android"
  echo "termux=$is_termux"
  exit 0
fi

if [ "$is_android" = "1" ] && [ "$asset" = "android-arm64" ]; then
  if ! curl -fsI "$url" >/dev/null 2>&1; then
    echo "error: Android/Termux prebuilt package is not available for $version ($archive)." >&2
    termux_source_hint
    exit 1
  fi
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

echo "Downloading $url"
if ! curl -fL "$url" -o "$tmp_dir/$archive"; then
  echo "error: failed to download $archive" >&2
  echo "hint: check that release $version has a $asset package at https://github.com/$repo/releases/tag/$version" >&2
  if [ "$is_android" = "1" ]; then
    termux_source_hint
  fi
  exit 1
fi

need tar
if ! tar -xzf "$tmp_dir/$archive" -C "$tmp_dir"; then
  echo "error: failed to extract $archive" >&2
  exit 1
fi

source_path="$tmp_dir/kiko-$version-$asset/kiko"
if [ ! -f "$source_path" ]; then
  echo "error: release archive did not contain kiko-$version-$asset/kiko" >&2
  exit 1
fi

mkdir -p "$install_dir"
cp "$source_path" "$install_dir/kiko"
chmod +x "$install_dir/kiko"

echo "Installed kiko $version to $install_dir/kiko"
case ":$PATH:" in
  *":$install_dir:"*) ;;
  *) echo "Add $install_dir to PATH to run 'kiko' from anywhere." ;;
esac
