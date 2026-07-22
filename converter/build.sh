#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"

# When launched from Finder/.app the shell PATH is not inherited, so Homebrew
# (in /opt/homebrew or /usr/local) is invisible. Make sure it is on PATH.
export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:$PATH"
if command -v brew >/dev/null 2>&1; then
  eval "$(brew shellenv)" || true
fi

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew is required but was not found."
  echo "Install Homebrew from https://brew.sh, then run this script again."
  exit 1
fi

for formula in cmake alembic hdf5 imath zlib; do
  if ! brew list --versions "$formula" >/dev/null 2>&1; then
    echo "$formula is not installed. Installing with Homebrew..."
    brew install "$formula"
  fi
done

mkdir -p "$ROOT_DIR/build"
cd "$ROOT_DIR/build"

cmake -G "Unix Makefiles" ..
make

mkdir -p "$ROOT_DIR/bin"
if [ -f "$ROOT_DIR/build/mesh2abc" ]; then
  cp "$ROOT_DIR/build/mesh2abc" "$ROOT_DIR/bin/mesh2abc"
fi
chmod +x "$ROOT_DIR/bin/mesh2abc"

echo
echo "Build complete: $ROOT_DIR/bin/mesh2abc"
