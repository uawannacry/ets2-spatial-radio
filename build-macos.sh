#!/usr/bin/env bash
set -euo pipefail

echo "==> Building ETS2 Cabin Radio for macOS..."
mkdir -p .module-cache

SDK_PATH="$(xcrun --show-sdk-path 2>/dev/null || echo "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")"

# 1. Compile SwiftUI launcher (ARM64 native for macOS 13.0+)
swiftc \
  -parse-as-library -swift-version 5 \
  -sdk "${SDK_PATH}" \
  -target arm64-apple-macos13.0 \
  -framework SwiftUI -framework AppKit \
  -o ETS2CabinRadio main.swift

# 2. Compile LiveCabinDSP dylib (x86_64 for ETS2 Rosetta / x86_64 process)
clang -dynamiclib -O2 -Wall -Wextra -arch x86_64 -mmacosx-version-min=10.13 \
  -Wl,-undefined,dynamic_lookup -install_name '@rpath/libETS2CabinDSP.dylib' \
  -o libETS2CabinDSP.dylib LiveCabinDSP.c upnp.c

# 3. Assemble application bundle
APP_DIR="ETS2 Cabin Radio.app"
rm -rf "${APP_DIR}"
mkdir -p "${APP_DIR}/Contents/MacOS"
mkdir -p "${APP_DIR}/Contents/Resources"

cp ETS2CabinRadio "${APP_DIR}/Contents/MacOS/"
cp libETS2CabinDSP.dylib "${APP_DIR}/Contents/Resources/"
cp Info.plist "${APP_DIR}/Contents/"
if [ -f "AppIcon.png" ]; then
  cp AppIcon.png "${APP_DIR}/Contents/Resources/"
fi

# 4. Ad-hoc code sign bundle
codesign --force --deep --sign - "${APP_DIR}"

echo "==> macOS build complete: ${APP_DIR}"
