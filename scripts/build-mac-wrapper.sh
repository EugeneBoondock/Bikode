#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_NAME="Bikode"
APP_VERSION=""
BUNDLE_ID="com.boondocklabs.bikode"
SOURCE_DIR="$REPO_ROOT/bin/x64/Release"
OUTPUT_DIR="$REPO_ROOT/dist"
EXE_PATH=""
INI_PATH=""
ICON_PNG="$REPO_ROOT/res/BoondockLabs.png"
RUNTIME_DIR=""

usage() {
  cat <<'EOF'
Usage: build-mac-wrapper.sh [options]

Builds a lightweight macOS .app wrapper around the existing Windows Bikode
binary. This is a compatibility route, not a native macOS port.

Options:
  --source-dir PATH     Folder containing Bikode.exe and Bikode.ini
  --exe PATH            Explicit path to Bikode.exe
  --ini PATH            Explicit path to Bikode.ini
  --output-dir PATH     Folder to write the zip archive into
  --app-version VALUE   Version string to embed in the bundle name and plist
  --bundle-id VALUE     Bundle identifier (default: com.boondocklabs.bikode)
  --icon-png PATH       PNG file to convert into AppIcon.icns when possible
  --runtime-dir PATH    Optional bundled Wine runtime folder to embed inside the app
  --help                Show this help text
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source-dir)
      SOURCE_DIR="$2"
      shift 2
      ;;
    --exe)
      EXE_PATH="$2"
      shift 2
      ;;
    --ini)
      INI_PATH="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --app-version)
      APP_VERSION="$2"
      shift 2
      ;;
    --bundle-id)
      BUNDLE_ID="$2"
      shift 2
      ;;
    --icon-png)
      ICON_PNG="$2"
      shift 2
      ;;
    --runtime-dir)
      RUNTIME_DIR="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$EXE_PATH" ]]; then
  EXE_PATH="$SOURCE_DIR/Bikode.exe"
fi

if [[ -z "$INI_PATH" ]]; then
  INI_PATH="$SOURCE_DIR/Bikode.ini"
fi

if [[ ! -f "$EXE_PATH" ]]; then
  echo "Bikode.exe not found: $EXE_PATH" >&2
  exit 1
fi

if [[ ! -f "$INI_PATH" ]]; then
  echo "Warning: Bikode.ini not found. The wrapper will still be created." >&2
fi

if [[ -z "$APP_VERSION" ]]; then
  if command -v pwsh >/dev/null 2>&1; then
    APP_VERSION="$(pwsh -NoLogo -NoProfile -Command "(Get-Item '$EXE_PATH').VersionInfo.FileVersion" | tr -d '\r')"
  fi
fi

if [[ -z "$APP_VERSION" ]]; then
  APP_VERSION="0.0.0"
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/bikode-mac.XXXXXX")"
APP_DIR="$TMP_DIR/$APP_NAME.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
WINDOWS_DIR="$RESOURCES_DIR/windows"
RUNTIME_APP_DIR="$RESOURCES_DIR/runtime"
ZIP_PATH="$OUTPUT_DIR/${APP_NAME}-mac-wrapper-${APP_VERSION}.zip"
ICON_NAME="AppIcon"

cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

mkdir -p "$MACOS_DIR" "$RESOURCES_DIR" "$WINDOWS_DIR" "$OUTPUT_DIR"

cat > "$MACOS_DIR/bikode-launcher" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WINDOWS_DIR="$APP_ROOT/Resources/windows"
RUNTIME_DIR="$APP_ROOT/Resources/runtime"
EXE_PATH="$WINDOWS_DIR/Bikode.exe"
PREFIX_BASE="$HOME/Library/Application Support/Bikode"
WINE_PREFIX_PATH="$PREFIX_BASE/wineprefix"

show_message() {
  local message="$1"
  if command -v osascript >/dev/null 2>&1; then
    osascript -e "display dialog \"${message//\"/\\\"}\" buttons {\"OK\"} default button \"OK\" with title \"Bikode\""
  else
    echo "$message" >&2
  fi
}

find_wine() {
  local candidate
  for candidate in \
    "$RUNTIME_DIR/bin/wine64" \
    "$RUNTIME_DIR/bin/wine" \
    "${WINE_BIN:-}" \
    "$(command -v wine64 2>/dev/null || true)" \
    "$(command -v wine 2>/dev/null || true)" \
    "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
    "/opt/homebrew/bin/wine64" \
    "/usr/local/bin/wine64" \
    "/opt/homebrew/bin/wine" \
    "/usr/local/bin/wine"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

if [[ ! -f "$EXE_PATH" ]]; then
  show_message "Bikode.exe is missing from the app bundle."
  exit 1
fi

if ! WINE_CMD="$(find_wine)"; then
  show_message "Bikode on macOS currently needs Wine or CrossOver. Install one of them, then relaunch Bikode."
  exit 1
fi

mkdir -p "$PREFIX_BASE"
export WINEPREFIX="$WINE_PREFIX_PATH"
export WINEDLLOVERRIDES="mscoree,mshtml="

cd "$WINDOWS_DIR"
exec "$WINE_CMD" "$EXE_PATH"
EOF
chmod +x "$MACOS_DIR/bikode-launcher"

cat > "$CONTENTS_DIR/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>
  <string>$APP_NAME</string>
  <key>CFBundleDisplayName</key>
  <string>$APP_NAME</string>
  <key>CFBundleIdentifier</key>
  <string>$BUNDLE_ID</string>
  <key>CFBundleVersion</key>
  <string>$APP_VERSION</string>
  <key>CFBundleShortVersionString</key>
  <string>$APP_VERSION</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleExecutable</key>
  <string>bikode-launcher</string>
  <key>CFBundleIconFile</key>
  <string>$ICON_NAME</string>
  <key>LSMinimumSystemVersion</key>
  <string>12.0</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF

cp "$EXE_PATH" "$WINDOWS_DIR/Bikode.exe"
if [[ -f "$INI_PATH" ]]; then
  cp "$INI_PATH" "$WINDOWS_DIR/Bikode.ini"
fi
cp "$REPO_ROOT/LICENSE" "$RESOURCES_DIR/LICENSE.txt"
if [[ -n "$RUNTIME_DIR" ]]; then
  if [[ ! -d "$RUNTIME_DIR" ]]; then
    echo "Runtime directory not found: $RUNTIME_DIR" >&2
    exit 1
  fi
  cp -R "$RUNTIME_DIR" "$RUNTIME_APP_DIR"
fi

cat > "$RESOURCES_DIR/README-mac.txt" <<'EOF'
Bikode macOS wrapper
====================

This package launches the Windows Bikode build through Wine or CrossOver.
It is not a native macOS port.

Requirements:
- No extra install if the app was built with an embedded runtime
- Otherwise Wine or CrossOver installed on the Mac

What ships in this bundle:
- The same Bikode.exe used for Windows releases
- A thin macOS launcher app that starts that binary

Update flow:
- Replace Bikode.exe and Bikode.ini with the latest Windows release outputs
- Rebuild the wrapper zip
EOF

if [[ -f "$ICON_PNG" && "$(command -v sips 2>/dev/null || true)" && "$(command -v iconutil 2>/dev/null || true)" ]]; then
  ICONSET_DIR="$TMP_DIR/${ICON_NAME}.iconset"
  mkdir -p "$ICONSET_DIR"
  for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$ICON_PNG" --out "$ICONSET_DIR/icon_${size}x${size}.png" >/dev/null 2>&1
    double=$((size * 2))
    sips -z "$double" "$double" "$ICON_PNG" --out "$ICONSET_DIR/icon_${size}x${size}@2x.png" >/dev/null 2>&1
  done
  iconutil -c icns "$ICONSET_DIR" -o "$RESOURCES_DIR/${ICON_NAME}.icns"
fi

rm -f "$ZIP_PATH"
ditto -c -k --sequesterRsrc --keepParent "$APP_DIR" "$ZIP_PATH"

echo "Created macOS wrapper: $ZIP_PATH"
