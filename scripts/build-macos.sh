#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/dist"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
RID="${1:-osx-$(uname -m)}"
LIMIT_BYTES=$((10 * 1024 * 1024))
mkdir -p "$DIST"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Invalid VERSION: $VERSION" >&2
  exit 1
}

case "$RID" in
  osx-arm64|arm64)
    PACKAGE_ARCH="arm64"
    NATIVE_ARCH="arm64"
    ;;
  osx-x64|x64|x86_64)
    PACKAGE_ARCH="x64"
    NATIVE_ARCH="x86_64"
    ;;
  *)
    echo "Unsupported macOS target: $RID" >&2
    exit 2
    ;;
esac

BUILD="$ROOT/build/macos-$PACKAGE_ARCH"
CMAKE_ARGS=(
  -S "$ROOT"
  -B "$BUILD"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_OSX_ARCHITECTURES="$NATIVE_ARCH"
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
  -DCODEXPETS_BUILD_APP=ON
  -DCODEXPETS_BUILD_TESTS=ON
)
if command -v ninja >/dev/null 2>&1; then
  CMAKE_ARGS+=( -G Ninja )
fi
cmake "${CMAKE_ARGS[@]}"
cmake --build "$BUILD" --config Release --parallel

HOST_ARCH="$(uname -m)"
if [[ "$HOST_ARCH" == "$NATIVE_ARCH" ]]; then
  ctest --test-dir "$BUILD" -C Release --output-on-failure
else
  echo "Skipping runtime tests for $NATIVE_ARCH on $HOST_ARCH."
fi

APP="$BUILD/CodeXPets.app"
EXECUTABLE="$APP/Contents/MacOS/CodeXPets"
[[ -x "$EXECUTABLE" ]] || { echo "Missing bundle executable: $EXECUTABLE" >&2; exit 1; }
plutil -lint "$APP/Contents/Info.plist" >/dev/null
[[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")" == "$VERSION" ]] || {
  echo "Info.plist version mismatch" >&2
  exit 1
}
lipo -archs "$EXECUTABLE" | tr ' ' '\n' | grep -qx "$NATIVE_ARCH" || {
  echo "Mach-O architecture mismatch: $(lipo -archs "$EXECUTABLE")" >&2
  exit 1
}

if [[ "$HOST_ARCH" == "$NATIVE_ARCH" ]]; then
  "$EXECUTABLE" --validate-resources
  "$EXECUTABLE" --smoke-test
fi

IDENTITY="${CODE_SIGN_IDENTITY:--}"
NOTARY_PROFILE="${APPLE_NOTARY_PROFILE:-}"
if [[ "${REQUIRE_NOTARIZATION:-0}" == "1" ]]; then
  [[ "$IDENTITY" != "-" ]] || { echo "CODE_SIGN_IDENTITY is required." >&2; exit 1; }
  [[ -n "$NOTARY_PROFILE" ]] || { echo "APPLE_NOTARY_PROFILE is required." >&2; exit 1; }
fi

if [[ "$IDENTITY" == "-" ]]; then
  codesign --force --sign - "$APP"
else
  codesign --force --options runtime --timestamp \
    --entitlements "$ROOT/packaging/macos/CodeXPets.entitlements" \
    --sign "$IDENTITY" "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP"

TEMP_ROOT="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/codexpets-package.XXXXXX")"
if [[ -n "$NOTARY_PROFILE" && "$IDENTITY" != "-" ]]; then
  NOTARY_ZIP="$TEMP_ROOT/CodeXPets-notary.zip"
  ditto -c -k --sequesterRsrc --keepParent "$APP" "$NOTARY_ZIP"
  xcrun notarytool submit "$NOTARY_ZIP" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$APP"
  xcrun stapler validate "$APP"
fi

ZIP="$DIST/CodeXPets-v$VERSION-macos-$PACKAGE_ARCH.zip"
ZIP_TEMP="$TEMP_ROOT/CodeXPets.zip"
ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP_TEMP"
mv -f "$ZIP_TEMP" "$ZIP"
unzip -Z1 "$ZIP" | grep -qx 'CodeXPets.app/Contents/MacOS/CodeXPets' || {
  echo "ZIP structure validation failed" >&2
  exit 1
}

DMG_STAGE="$TEMP_ROOT/dmg"
mkdir -p "$DMG_STAGE"
ditto "$APP" "$DMG_STAGE/CodeXPets.app"
ln -s /Applications "$DMG_STAGE/Applications"
DMG="$DIST/CodeXPets-v$VERSION-macos-$PACKAGE_ARCH.dmg"
hdiutil create -volname CodeXPets -srcfolder "$DMG_STAGE" -ov -format UDZO "$DMG" >/dev/null
hdiutil verify "$DMG" >/dev/null

if [[ -n "$NOTARY_PROFILE" && "$IDENTITY" != "-" ]]; then
  codesign --force --timestamp --sign "$IDENTITY" "$DMG"
  xcrun notarytool submit "$DMG" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$DMG"
  xcrun stapler validate "$DMG"
fi

APP_BYTES=$(( $(du -sk "$APP" | awk '{print $1}') * 1024 ))
ZIP_BYTES="$(stat -f%z "$ZIP")"
DMG_BYTES="$(stat -f%z "$DMG")"
for pair in "app:$APP_BYTES" "zip:$ZIP_BYTES" "dmg:$DMG_BYTES"; do
  name="${pair%%:*}"
  bytes="${pair#*:}"
  if (( bytes > LIMIT_BYTES )); then
    echo "$name exceeds 10 MiB: $bytes bytes" >&2
    exit 1
  fi
done

CHECKSUM="$DIST/SHA256SUMS-macos.txt"
{
  shasum -a 256 "$ZIP" | sed 's#  .*/#  #'
  shasum -a 256 "$DMG" | sed 's#  .*/#  #'
} > "$CHECKSUM"
cat "$CHECKSUM"
printf 'Created %s (%.2f MiB)\n' "$ZIP" "$(awk "BEGIN {print $ZIP_BYTES/1048576}")"
printf 'Created %s (%.2f MiB)\n' "$DMG" "$(awk "BEGIN {print $DMG_BYTES/1048576}")"
