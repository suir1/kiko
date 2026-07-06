#!/usr/bin/env sh
set -eu

repo="${KIKO_REPO:-suir1/kiko}"
version="${KIKO_VERSION:-}"
dry_run="${KIKO_INSTALL_DRY_RUN:-}"
asset_override="${KIKO_ASSET:-}"
allow_android_linux="${KIKO_ALLOW_LINUX_ON_ANDROID:-}"
build_from_source="${KIKO_BUILD_FROM_SOURCE:-}"
source_ref="${KIKO_SOURCE_REF:-}"
install_deps="${KIKO_INSTALL_DEPS:-}"
termux_prefix="${PREFIX:-}"
add_to_path="${KIKO_ADD_TO_PATH:-}"

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

if [ -z "$install_deps" ] && [ "$is_termux" = "1" ]; then
  install_deps=1
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

truthy() {
  case "$1" in
    1 | true | TRUE | yes | YES | on | ON) return 0 ;;
    *) return 1 ;;
  esac
}

shell_quote() {
  printf "%s" "$1" | sed "s/'/'\\\\''/g; 1s/^/'/; \$s/\$/'/"
}

path_profile_file() {
  if [ -n "${KIKO_PATH_PROFILE:-}" ]; then
    printf "%s\n" "$KIKO_PATH_PROFILE"
    return
  fi
  shell_name="$(basename "${SHELL:-}")"
  case "$shell_name" in
    zsh) printf "%s\n" "$HOME/.zshrc" ;;
    bash) printf "%s\n" "$HOME/.bashrc" ;;
    *) printf "%s\n" "$HOME/.profile" ;;
  esac
}

should_add_to_path() {
  if truthy "$add_to_path"; then
    return 0
  fi
  if [ "$add_to_path" = "prompt" ] && [ -r /dev/tty ] && [ -w /dev/tty ]; then
    printf "Add %s to PATH for future shells? [y/N] " "$install_dir" >/dev/tty
    IFS= read -r answer </dev/tty || answer=""
    case "$answer" in
      y | Y | yes | YES) return 0 ;;
    esac
  fi
  return 1
}

add_install_dir_to_path() {
  if ! should_add_to_path; then
    case ":$PATH:" in
      *":$install_dir:"*) ;;
      *) echo "Add $install_dir to PATH to run 'kiko' from anywhere, or rerun with KIKO_ADD_TO_PATH=1." ;;
    esac
    return
  fi

  profile="$(path_profile_file)"
  mkdir -p "$(dirname "$profile")"
  if [ -f "$profile" ] && grep -Fq "$install_dir" "$profile"; then
    echo "$install_dir is already mentioned in $profile"
    return
  fi

  quoted_dir="$(shell_quote "$install_dir")"
  {
    echo ""
    echo "# kiko installer PATH"
    echo "case \":\$PATH:\" in"
    echo "  *\":$install_dir:\"*) ;;"
    echo "  *) export PATH=$quoted_dir:\$PATH ;;"
    echo "esac"
  } >>"$profile"
  echo "Added $install_dir to PATH in $profile"
  echo "Restart your shell, or run: export PATH=$quoted_dir:\$PATH"
}

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

Automatic source build:
  curl -fsSL https://raw.githubusercontent.com/suir1/kiko/main/scripts/install.sh | KIKO_BUILD_FROM_SOURCE=1 sh

If you have a custom Android/Termux release asset, retry with KIKO_ASSET=android-arm64.
EOF
}

source_build() {
  if [ "$is_termux" = "1" ] && [ "$install_deps" != "0" ]; then
    need pkg
    echo "Installing Termux build dependencies..."
    pkg update
    pkg install -y git clang cmake ninja pkg-config libsodium zstd
  fi

  need git
  need cmake
  need ninja

  build_tmp="$(mktemp -d)"
  trap 'rm -rf "$build_tmp"' EXIT INT TERM

  repo_url="https://github.com/$repo.git"
  echo "Cloning $repo_url ($source_ref)"
  if ! git clone --depth 1 --branch "$source_ref" "$repo_url" "$build_tmp/source"; then
    echo "error: failed to clone source ref $source_ref from $repo_url" >&2
    exit 1
  fi

  echo "Configuring source build..."
  if ! (cd "$build_tmp/source" && cmake --preset system-deps); then
    echo "error: source configure failed" >&2
    exit 1
  fi

  echo "Building kiko..."
  if ! (cd "$build_tmp/source" && cmake --build build --target kiko); then
    echo "error: source build failed" >&2
    exit 1
  fi

  source_path="$build_tmp/source/build/kiko"
  if [ ! -f "$source_path" ]; then
    echo "error: source build did not produce build/kiko" >&2
    exit 1
  fi

  mkdir -p "$install_dir"
  cp "$source_path" "$install_dir/kiko"
  chmod +x "$install_dir/kiko"

  echo "Installed kiko from source ($source_ref) to $install_dir/kiko"
  add_install_dir_to_path
}

if [ -z "$version" ] && { [ -z "$build_from_source" ] || [ -z "$source_ref" ]; }; then
  need curl
  version="$(
    curl -fsSL \
      -H "Accept: application/vnd.github+json" \
      -H "User-Agent: kiko-install" \
      "https://api.github.com/repos/$repo/releases" |
      sed -n 's/^[[:space:]]*"tag_name": "\(v[^"]*\)".*/\1/p' |
      head -n 1
  )" || version=""
fi

if [ -z "$version" ] && { [ -z "$build_from_source" ] || [ -z "$source_ref" ]; }; then
  need curl
  version="$(
    curl -fsSL "https://github.com/$repo/releases.atom" |
      sed -n 's:.*<title>\(v[^<]*\)</title>.*:\1:p' |
      head -n 1
  )" || version=""
fi

if [ -z "$version" ] && [ -n "$source_ref" ]; then
  version="$source_ref"
fi

if [ -z "$version" ]; then
  echo "error: could not determine latest kiko release from https://github.com/$repo/releases" >&2
  echo "hint: set KIKO_VERSION=v0.2.0-alpha and retry" >&2
  exit 1
fi

if [ -z "$source_ref" ]; then
  source_ref="$version"
fi

os_name="${KIKO_TEST_UNAME_S:-$(uname -s)}"
arch_name="${KIKO_TEST_UNAME_M:-$(uname -m)}"

if [ -n "$build_from_source" ]; then
  if [ -n "$dry_run" ]; then
    echo "version=$version"
    echo "mode=source"
    echo "source_ref=$source_ref"
    echo "install_dir=$install_dir"
    echo "install_deps=$install_deps"
    echo "add_to_path=${add_to_path:-0}"
    echo "android=$is_android"
    echo "termux=$is_termux"
    exit 0
  fi

  source_build
  exit 0
fi

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
  echo "add_to_path=${add_to_path:-0}"
  echo "android=$is_android"
  echo "termux=$is_termux"
  exit 0
fi

need curl

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
add_install_dir_to_path
