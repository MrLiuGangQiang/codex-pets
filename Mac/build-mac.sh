#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")"
BUILD_VERSION="${GITHUB_RUN_NUMBER:-1}"
DIST_DIR="$ROOT_DIR/dist"
APP_DIR="$DIST_DIR/CodeXPets.app"
ZIP_PATH="$DIST_DIR/CodeXPets-v${VERSION}-macos-universal.zip"
DMG_PATH="$DIST_DIR/CodeXPets-v${VERSION}-macos-universal.dmg"
DMG_STAGING_DIR="$DIST_DIR/.dmg-staging"
DMG_MOUNT_DIR=""
CHECKSUM_PATH="$DIST_DIR/SHA256SUMS-macos.txt"
SKIP_TESTS=0

cleanup() {
  if [[ -n "${DMG_MOUNT_DIR:-}" ]]; then
    hdiutil detach "$DMG_MOUNT_DIR" -quiet >/dev/null 2>&1 || true
    rmdir "$DMG_MOUNT_DIR" >/dev/null 2>&1 || true
  fi
  rm -rf "$DMG_STAGING_DIR"
}
trap cleanup EXIT

if [[ "${1:-}" == "--skip-tests" ]]; then
  SKIP_TESTS=1
fi

if [[ "$SKIP_TESTS" -eq 0 ]]; then
  swift test --package-path "$SCRIPT_DIR" -c release
fi

swift build --package-path "$SCRIPT_DIR" -c release \
  --product CodeXPets --arch arm64 --arch x86_64
BIN_DIR="$(swift build --package-path "$SCRIPT_DIR" -c release \
  --product CodeXPets --arch arm64 --arch x86_64 --show-bin-path)"
BINARY="$BIN_DIR/CodeXPets"

if [[ ! -x "$BINARY" ]]; then
  echo "Missing release binary: $BINARY" >&2
  exit 1
fi

mkdir -p "$DIST_DIR"
rm -rf "$APP_DIR" "$DMG_STAGING_DIR"
rm -f "$ZIP_PATH" "$DMG_PATH" "$CHECKSUM_PATH"
mkdir -p "$APP_DIR/Contents/MacOS" "$APP_DIR/Contents/Resources"
cp "$BINARY" "$APP_DIR/Contents/MacOS/CodeXPets"
chmod +x "$APP_DIR/Contents/MacOS/CodeXPets"
cp -R "$SCRIPT_DIR/Sources/CodeXPetsMac/Resources/." "$APP_DIR/Contents/Resources/"

sed -e "s/__VERSION__/$VERSION/g" \
    -e "s/__BUILD_VERSION__/$BUILD_VERSION/g" \
    "$SCRIPT_DIR/Info.plist" > "$APP_DIR/Contents/Info.plist"

ICONSET="$DIST_DIR/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
ICON_SOURCE="$SCRIPT_DIR/Sources/CodeXPetsMac/Resources/AppIcon.png"
sips -z 16 16     "$ICON_SOURCE" --out "$ICONSET/icon_16x16.png" >/dev/null
sips -z 32 32     "$ICON_SOURCE" --out "$ICONSET/icon_16x16@2x.png" >/dev/null
sips -z 32 32     "$ICON_SOURCE" --out "$ICONSET/icon_32x32.png" >/dev/null
sips -z 64 64     "$ICON_SOURCE" --out "$ICONSET/icon_32x32@2x.png" >/dev/null
sips -z 128 128   "$ICON_SOURCE" --out "$ICONSET/icon_128x128.png" >/dev/null
sips -z 256 256   "$ICON_SOURCE" --out "$ICONSET/icon_128x128@2x.png" >/dev/null
sips -z 256 256   "$ICON_SOURCE" --out "$ICONSET/icon_256x256.png" >/dev/null
sips -z 512 512   "$ICON_SOURCE" --out "$ICONSET/icon_256x256@2x.png" >/dev/null
sips -z 512 512   "$ICON_SOURCE" --out "$ICONSET/icon_512x512.png" >/dev/null
cp "$ICON_SOURCE" "$ICONSET/icon_512x512@2x.png"
iconutil -c icns "$ICONSET" -o "$APP_DIR/Contents/Resources/AppIcon.icns"
rm -rf "$ICONSET"

plutil -lint "$APP_DIR/Contents/Info.plist"
lipo "$APP_DIR/Contents/MacOS/CodeXPets" -verify_arch arm64 x86_64
codesign --force --deep --sign - "$APP_DIR"
codesign --verify --deep --strict "$APP_DIR"
"$APP_DIR/Contents/MacOS/CodeXPets" --validate-resources

ditto -c -k --sequesterRsrc --keepParent "$APP_DIR" "$ZIP_PATH"

mkdir -p "$DMG_STAGING_DIR"
ditto "$APP_DIR" "$DMG_STAGING_DIR/CodeXPets.app"
ln -s /Applications "$DMG_STAGING_DIR/Applications"
hdiutil create \
  -volname "CodeXPets $VERSION" \
  -srcfolder "$DMG_STAGING_DIR" \
  -ov \
  -format UDZO \
  -imagekey zlib-level=9 \
  "$DMG_PATH" >/dev/null
hdiutil verify "$DMG_PATH" >/dev/null

DMG_MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/codexpets-dmg.XXXXXX")"
hdiutil attach "$DMG_PATH" -readonly -nobrowse -mountpoint "$DMG_MOUNT_DIR" -quiet
DMG_APP="$DMG_MOUNT_DIR/CodeXPets.app"

if [[ ! -d "$DMG_APP" ]]; then
  echo "DMG is missing CodeXPets.app" >&2
  exit 1
fi
if [[ ! -L "$DMG_MOUNT_DIR/Applications" ]] || \
   [[ "$(readlink "$DMG_MOUNT_DIR/Applications")" != "/Applications" ]]; then
  echo "DMG is missing the Applications shortcut" >&2
  exit 1
fi

DMG_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  "$DMG_APP/Contents/Info.plist")"
if [[ "$DMG_VERSION" != "$VERSION" ]]; then
  echo "DMG app version mismatch: expected $VERSION, got $DMG_VERSION" >&2
  exit 1
fi

plutil -lint "$DMG_APP/Contents/Info.plist"
lipo "$DMG_APP/Contents/MacOS/CodeXPets" -verify_arch arm64 x86_64
codesign --verify --deep --strict "$DMG_APP"
"$DMG_APP/Contents/MacOS/CodeXPets" --validate-resources

hdiutil detach "$DMG_MOUNT_DIR" -quiet
rmdir "$DMG_MOUNT_DIR"
DMG_MOUNT_DIR=""
rm -rf "$DMG_STAGING_DIR"

: > "$CHECKSUM_PATH"
for package in "$DMG_PATH" "$ZIP_PATH"; do
  hash="$(shasum -a 256 "$package" | awk '{print $1}')"
  printf '%s  %s\n' "$hash" "$(basename "$package")" >> "$CHECKSUM_PATH"
done

echo "macOS app: $APP_DIR"
echo "macOS DMG: $DMG_PATH"
echo "macOS ZIP: $ZIP_PATH"
cat "$CHECKSUM_PATH"
