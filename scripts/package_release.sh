#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="$ROOT_DIR/dist"

BLENDER_ZIP="$DIST_DIR/abcexport-blender-addon.zip"
HOUDINI_ZIP="$DIST_DIR/abcexport-houdini-plugin.zip"

rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

echo "Building the converter..."
"$ROOT_DIR/converter/build.sh"

echo "Packaging the Blender add-on..."
find "$ROOT_DIR/blender/abc_export" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
(cd "$ROOT_DIR/blender" && zip -r -X "$BLENDER_ZIP" abc_export -x "*.pyc")

echo "Packaging the Houdini plugin..."
HOUDINI_STAGE="$DIST_DIR/abcexport-houdini-plugin"
mkdir -p "$HOUDINI_STAGE/converter/bin"
cp "$ROOT_DIR/houdini/abc_export.1.0.hda" "$HOUDINI_STAGE/"
cp "$ROOT_DIR/converter/mesh2abc.cpp" "$HOUDINI_STAGE/converter/"
cp "$ROOT_DIR/converter/CMakeLists.txt" "$HOUDINI_STAGE/converter/"
cp "$ROOT_DIR/converter/build.sh" "$HOUDINI_STAGE/converter/"
cp "$ROOT_DIR/converter/build.bat" "$HOUDINI_STAGE/converter/"
cp "$ROOT_DIR/converter/bin/mesh2abc" "$HOUDINI_STAGE/converter/bin/"
chmod +x "$HOUDINI_STAGE/converter/build.sh" "$HOUDINI_STAGE/converter/bin/mesh2abc"
cp "$ROOT_DIR/LICENSE" "$HOUDINI_STAGE/"
(cd "$DIST_DIR" && ditto -c -k --norsrc --noextattr --noqtn --noacl --keepParent "abcexport-houdini-plugin" "$HOUDINI_ZIP")

echo
echo "Release packages ready:"
echo "  $BLENDER_ZIP"
echo "  $HOUDINI_ZIP"
