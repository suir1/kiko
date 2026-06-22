#!/usr/bin/env sh
set -eu

repo="${KIKO_REPO:-suir1/kiko}"
version="${KIKO_VERSION:-}"
install_dir="${KIKO_INSTALL_DIR:-$HOME/.local/bin}"
dry_run="${KIKO_INSTALL_DRY_RUN:-}"

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
need tar

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

case "$os_name" in
  Darwin)
    case "$arch_name" in
      arm64) asset="macos-arm64" ;;
      x86_64) asset="macos-x64" ;;
      *) die "unsupported macOS architecture: $arch_name" ;;
    esac
    ;;
  Linux)
    case "$arch_name" in
      x86_64 | amd64) asset="linux-x64" ;;
      aarch64 | arm64) asset="linux-arm64" ;;
      *) die "unsupported Linux architecture: $arch_name" ;;
    esac
    ;;
  *)
    die "unsupported OS: $os_name"
    ;;
esac

archive="kiko-$version-$asset.tar.gz"
url="https://github.com/$repo/releases/download/$version/$archive"

if [ -n "$dry_run" ]; then
  echo "version=$version"
  echo "asset=$asset"
  echo "archive=$archive"
  echo "url=$url"
  echo "install_dir=$install_dir"
  exit 0
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

echo "Downloading $url"
if ! curl -fL "$url" -o "$tmp_dir/$archive"; then
  echo "error: failed to download $archive" >&2
  echo "hint: check that release $version has a $asset package at https://github.com/$repo/releases/tag/$version" >&2
  exit 1
fi

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
