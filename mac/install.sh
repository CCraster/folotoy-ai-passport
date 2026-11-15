#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
label=com.craster.passport-sync
domain="gui/$(id -u)"
legacy_plist="$HOME/Library/LaunchAgents/$label.plist"
legacy_app="$HOME/Library/Application Support/Passport Sync/Passport Sync.app"
app="/Applications/Passport Sync.app"
stop_app() {
    if launchctl print "$domain/$label" >/dev/null 2>&1; then launchctl bootout "$domain/$label"; fi
    rm -f "$legacy_plist"
    local pids
    pids=$(pgrep -f '^/Applications/Passport Sync[.]app/Contents/MacOS/PassportSync( |$)' || true)
    if [[ -n "$pids" ]]; then
        while read -r pid; do kill -TERM "$pid"; done <<< "$pids"
        for _ in {1..30}; do
            if ! pgrep -f '^/Applications/Passport Sync[.]app/Contents/MacOS/PassportSync( |$)' >/dev/null; then return; fi
            sleep 0.1
        done
        printf '%s\n' 'Passport Sync did not stop; installation cancelled.' >&2
        exit 1
    fi
}
if [[ "${1:-}" == --uninstall ]]; then
    if [[ -x "$app/Contents/MacOS/PassportSync" ]]; then "$app/Contents/MacOS/PassportSync" --unregister-login; fi
    stop_app
    rm -rf "$app" "$legacy_app"
    printf '%s\n' 'Passport Sync removed. Local preferences retained.'
    exit
fi
codex_path="${1:-$(command -v codex || true)}"
if [[ "$codex_path" != /* || ! -f "$codex_path" || ! -x "$codex_path" ]]; then
    printf '%s\n' '请先安装 Codex CLI，或将 command -v codex 返回的完整路径传给本脚本。' >&2
    exit 2
fi
bash build.sh
# 迁移首次安装默认开启；后续升级沿用用户已选的登录启动状态。
login_enabled=true
if [[ -x "$app/Contents/MacOS/PassportSync" ]] && /usr/libexec/PlistBuddy -c 'Print :PassportSupportsLoginQuery' "$app/Contents/Info.plist" >/dev/null 2>&1; then
    login_enabled=$("$app/Contents/MacOS/PassportSync" --login-enabled)
fi
stop_app
ditto "build/Passport Sync.app" "$app"
"$app/Contents/MacOS/PassportSync" --configure "$codex_path"
if [[ "$login_enabled" == true ]] && ! "$app/Contents/MacOS/PassportSync" --register-login; then
    printf '%s\n' 'Check login-item permission in the management app.' >&2
fi
rm -rf "$legacy_app"
open "$app"
printf '%s\n' "Installed: $app"
