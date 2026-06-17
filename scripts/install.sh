#!/usr/bin/env sh
set -eu

repo="${KIKO_REPO:-suir1/kiko}"
version="${KIKO_VERSION:-}"
install_dir="${KIKO_INSTALL_DIR:-$HOME/.local/bin}"

need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required command not found: $1" >&2
    exit 1
  fi
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
  echo "error: could not determine latest kiko release" >&2
  echo "hint: set KIKO_VERSION=v0.1.1-alpha and retry" >&2
  exit 1
fi

case "$(uname -s)" in
  Darwin) asset="macos" ;;
  Linux)
    case "$(uname -m)" in
      x86_64 | amd64) asset="linux-x64" ;;
      aarch64 | arm64) asset="linux-arm64" ;;
      *)
        echo "error: unsupported Linux architecture: $(uname -m)" >&2
        exit 1
        ;;
    esac
    ;;
  *)
    echo "error: unsupported OS: $(uname -s)" >&2
    exit 1
    ;;
esac

archive="kiko-$version-$asset.tar.gz"
url="https://github.com/$repo/releases/download/$version/$archive"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT INT TERM

echo "Downloading $url"
curl -fL "$url" -o "$tmp_dir/$archive"
tar -xzf "$tmp_dir/$archive" -C "$tmp_dir"

mkdir -p "$install_dir"
cp "$tmp_dir/kiko-$version-$asset/kiko" "$install_dir/kiko"
chmod +x "$install_dir/kiko"

echo "Installed kiko $version to $install_dir/kiko"
case ":$PATH:" in
  *":$install_dir:"*) ;;
  *) echo "Add $install_dir to PATH to run 'kiko' from anywhere." ;;
esac
