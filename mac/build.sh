#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
bundle="$PWD/build/Passport Sync.app"
mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources" build/cache
cp Info.plist "$bundle/Contents/Info.plist"
cp RemoteTaskCount.py "$bundle/Contents/Resources/RemoteTaskCount.py"
revision=$(git rev-parse HEAD)
if ! git diff --quiet -- . ../firmware; then revision="$revision-dirty"; fi
/usr/libexec/PlistBuddy -c "Add :PassportSourceCommit string $revision" "$bundle/Contents/Info.plist"
xcrun swiftc -target "$(uname -m)-apple-macosx13.0" -module-cache-path "$PWD/build/cache" -warnings-as-errors -O \
  ManagementApp.swift BackupWindow.swift Brand.swift PassportUI.swift PassportSync.swift QuotaData.swift CodexClient.swift CodexTasks.swift RemoteTasks.swift QuotaTests.swift \
  -o "$bundle/Contents/MacOS/PassportSync"
"$bundle/Contents/MacOS/PassportSync" --self-test
cat Brand.swift tools/MakeIcon.swift > build/MakeIcon.swift
xcrun swift -module-cache-path "$PWD/build/cache" build/MakeIcon.swift "$PWD/build/AppIcon.iconset"
iconutil -c icns build/AppIcon.iconset -o "$bundle/Contents/Resources/AppIcon.icns"
if [[ -n "${PASSPORT_SIGNING_IDENTITY:-}" ]]; then
    codesign --force --options runtime --timestamp --sign "$PASSPORT_SIGNING_IDENTITY" \
        --identifier com.craster.passport-sync "$bundle"
else
    codesign --force --sign - --identifier com.craster.passport-sync "$bundle"
fi
codesign --verify --deep --strict "$bundle"
printf '%s\n' "$bundle"
