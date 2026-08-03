#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="$ROOT/dist"
PROJECT="$ROOT/src/CodeXPets.App/CodeXPets.App.csproj"
INFO_PLIST_TEMPLATE="$ROOT/packaging/macos/Info.plist.in"
ENTITLEMENTS="$ROOT/packaging/macos/CodeXPets.entitlements"
VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
TARGET="${1:-all}"

export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1

[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "Invalid VERSION: $VERSION" >&2
  exit 1
}

case "$TARGET" in
  all) RIDS=(osx-x64 osx-arm64) ;;
  osx-x64|osx-arm64) RIDS=("$TARGET") ;;
  *) echo "Usage: $0 [all|osx-x64|osx-arm64]" >&2; exit 2 ;;
esac

if [[ -x "$ROOT/.dotnet/dotnet" ]]; then
  DOTNET="$ROOT/.dotnet/dotnet"
else
  DOTNET="$(command -v dotnet || true)"
fi
[[ -n "$DOTNET" ]] || { echo ".NET 10 SDK is required." >&2; exit 1; }

mkdir -p "$DIST"

safe_reset_dir() {
  local path="$1"
  case "$path" in
    "$DIST"/*) rm -rf -- "$path"; mkdir -p "$path" ;;
    *) echo "Refusing to reset path outside dist: $path" >&2; exit 3 ;;
  esac
}

safe_remove_file() {
  local path="$1"
  case "$path" in
    "$DIST"/*) rm -f -- "$path" ;;
    *) echo "Refusing to remove path outside dist: $path" >&2; exit 3 ;;
  esac
}

"$DOTNET" restore "$ROOT/CodeXPets.slnx"
"$DOTNET" build "$ROOT/CodeXPets.slnx" -c Release --no-restore
"$DOTNET" test "$ROOT/CodeXPets.slnx" -c Release --no-build --no-restore
"$DOTNET" "$ROOT/src/CodeXPets.App/bin/Release/net10.0/CodeXPets.dll" --validate-resources
"$DOTNET" "$ROOT/src/CodeXPets.App/bin/Release/net10.0/CodeXPets.dll" --smoke-test

make_icns() {
  local source="$ROOT/src/CodeXPets.App/Assets/Icons/AppIcon.png"
  local iconset="$DIST/AppIcon.iconset"
  local output="$DIST/AppIcon.icns"
  safe_reset_dir "$iconset"
  sips -z 16 16 "$source" --out "$iconset/icon_16x16.png" >/dev/null
  sips -z 32 32 "$source" --out "$iconset/icon_16x16@2x.png" >/dev/null
  sips -z 32 32 "$source" --out "$iconset/icon_32x32.png" >/dev/null
  sips -z 64 64 "$source" --out "$iconset/icon_32x32@2x.png" >/dev/null
  sips -z 128 128 "$source" --out "$iconset/icon_128x128.png" >/dev/null
  sips -z 256 256 "$source" --out "$iconset/icon_128x128@2x.png" >/dev/null
  sips -z 256 256 "$source" --out "$iconset/icon_256x256.png" >/dev/null
  sips -z 512 512 "$source" --out "$iconset/icon_256x256@2x.png" >/dev/null
  sips -z 512 512 "$source" --out "$iconset/icon_512x512.png" >/dev/null
  cp "$source" "$iconset/icon_512x512@2x.png"
  iconutil -c icns "$iconset" -o "$output"
  rm -rf -- "$iconset"
}

validate_macho_architecture() {
  local app="$1"
  local expected="$2"
  local executable="$app/Contents/MacOS/CodeXPets"
  local app_archs
  app_archs="$(lipo -archs "$executable")"
  [[ "$app_archs" == "$expected" ]] || {
    echo "Architecture mismatch for $executable: $app_archs (expected $expected)" >&2
    exit 1
  }

  while IFS= read -r -d '' file_path; do
    if file "$file_path" | grep -q 'Mach-O'; then
      local file_archs
      file_archs="$(lipo -archs "$file_path")"
      [[ " $file_archs " == *" $expected "* ]] || {
        echo "Architecture mismatch for $file_path: $file_archs (missing $expected)" >&2
        exit 1
      }
    fi
  done < <(find "$app/Contents/MacOS" -type f -print0)
}

verify_dmg_contents() {
  local dmg_path="$1"
  local rid="$2"
  local mount_point="$DIST/mount-$rid"
  safe_reset_dir "$mount_point"
  hdiutil attach -nobrowse -readonly -mountpoint "$mount_point" "$dmg_path" >/dev/null
  local failed=0
  [[ -d "$mount_point/CodeXPets.app" ]] || { echo "DMG is missing CodeXPets.app" >&2; failed=1; }
  [[ -L "$mount_point/Applications" ]] || { echo "DMG is missing Applications link" >&2; failed=1; }
  hdiutil detach "$mount_point" >/dev/null || failed=1
  safe_reset_dir "$mount_point"
  [[ "$failed" == "0" ]]
}

make_icns
identity="${CODE_SIGN_IDENTITY:--}"
notary_profile="${APPLE_NOTARY_PROFILE:-}"
if [[ "${REQUIRE_NOTARIZATION:-0}" == "1" ]]; then
  [[ "$identity" != "-" ]] || { echo "CODE_SIGN_IDENTITY is required." >&2; exit 1; }
  [[ -n "$notary_profile" ]] || { echo "APPLE_NOTARY_PROFILE is required." >&2; exit 1; }
fi

CREATED=()
for rid in "${RIDS[@]}"; do
  case "$rid" in
    osx-x64)
      package_arch="x64"
      native_arch="x86_64"
      ;;
    osx-arm64)
      package_arch="arm64"
      native_arch="arm64"
      ;;
  esac

  publish="$DIST/publish-$rid"
  bundle_root="$DIST/bundle-$rid"
  app="$bundle_root/CodeXPets.app"
  dmg_stage="$DIST/dmg-$rid"
  zip="$DIST/CodeXPets-v$VERSION-macos-$package_arch.zip"
  dmg="$DIST/CodeXPets-v$VERSION-macos-$package_arch.dmg"
  notary_zip="$DIST/notary-$rid.zip"

  safe_reset_dir "$publish"
  safe_reset_dir "$bundle_root"
  safe_reset_dir "$dmg_stage"
  safe_remove_file "$zip"
  safe_remove_file "$dmg"
  safe_remove_file "$notary_zip"

  "$DOTNET" publish "$PROJECT" -c Release -r "$rid" --self-contained true \
    -o "$publish" -p:DebugType=None -p:DebugSymbols=false
  find "$publish" -type f -name '*.pdb' -delete

  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
  cp -R "$publish"/. "$app/Contents/MacOS/"
  cp "$DIST/AppIcon.icns" "$app/Contents/Resources/AppIcon.icns"
  sed -e "s/__VERSION__/$VERSION/g" -e "s/__BUILD_VERSION__/${VERSION//./}/g" \
    "$INFO_PLIST_TEMPLATE" > "$app/Contents/Info.plist"
  plutil -lint "$app/Contents/Info.plist" >/dev/null
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$app/Contents/Info.plist")" == "$VERSION" ]] || {
    echo "Info.plist version mismatch for $rid" >&2
    exit 1
  }
  [[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")" == "CodeXPets" ]] || {
    echo "Info.plist executable mismatch for $rid" >&2
    exit 1
  }
  chmod +x "$app/Contents/MacOS/CodeXPets"

  validate_macho_architecture "$app" "$native_arch"

  if [[ "$identity" == "-" ]]; then
    codesign --force --deep --sign - "$app"
  else
    codesign --force --deep --options runtime --timestamp \
      --entitlements "$ENTITLEMENTS" --sign "$identity" "$app"
  fi
  codesign --verify --deep --strict --verbose=2 "$app"

  host_arch="$(uname -m)"
  if [[ "$host_arch" == "$native_arch" ]]; then
    "$app/Contents/MacOS/CodeXPets" --smoke-test
  elif [[ "${RUN_CROSS_ARCH_SMOKE:-0}" == "1" && "$host_arch" == "arm64" && "$native_arch" == "x86_64" ]]; then
    arch -x86_64 "$app/Contents/MacOS/CodeXPets" --smoke-test
  fi

  if [[ -n "$notary_profile" && "$identity" != "-" ]]; then
    ditto -c -k --sequesterRsrc --keepParent "$app" "$notary_zip"
    xcrun notarytool submit "$notary_zip" --keychain-profile "$notary_profile" --wait
    xcrun stapler staple "$app"
    xcrun stapler validate "$app"
    safe_remove_file "$notary_zip"
  fi

  ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
  cp -R "$app" "$dmg_stage/CodeXPets.app"
  ln -s /Applications "$dmg_stage/Applications"
  hdiutil create -volname "CodeXPets" -srcfolder "$dmg_stage" -ov -format UDZO "$dmg" >/dev/null

  if [[ -n "$notary_profile" && "$identity" != "-" ]]; then
    codesign --force --timestamp --sign "$identity" "$dmg"
    xcrun notarytool submit "$dmg" --keychain-profile "$notary_profile" --wait
    xcrun stapler staple "$dmg"
    xcrun stapler validate "$dmg"
  fi

  unzip -Z1 "$zip" | grep -qx 'CodeXPets.app/Contents/MacOS/CodeXPets' || {
    echo "ZIP structure validation failed for $rid" >&2
    exit 1
  }
  hdiutil verify "$dmg" >/dev/null
  verify_dmg_contents "$dmg" "$rid"

  CREATED+=("$zip" "$dmg")
  echo "Created $zip"
  echo "Created $dmg"

  for temporary_directory in "$publish" "$bundle_root" "$dmg_stage" "$DIST/mount-$rid"; do
    safe_reset_dir "$temporary_directory"
    rmdir "$temporary_directory"
  done
done

safe_remove_file "$DIST/AppIcon.icns"
checksum="$DIST/SHA256SUMS-macos.txt"
: > "$checksum"
for file_path in "${CREATED[@]}"; do
  shasum -a 256 "$file_path" | sed 's#  .*/#  #' >> "$checksum"
done
cat "$checksum"
