#!/bin/bash
# Launcher for the "Clean Metadata.app" bundle.
#
# clean_metadata is an interactive CLI (it lists matches and asks for a
# y/N confirmation on stdin), so rather than reimplement that flow as a
# GUI, this launcher: (1) asks for a folder via a native Finder-style
# picker, then (2) runs the real CLI inside Terminal.app so the existing
# scan/confirm/delete experience works unchanged.
set -euo pipefail

APP_RESOURCES="$(cd "$(dirname "$0")/../Resources" && pwd)"
BIN="$APP_RESOURCES/clean_metadata"

if [ ! -x "$BIN" ]; then
    osascript -e 'display alert "Clean Metadata" message "The clean_metadata binary is missing from this app bundle. Reinstall the app (make install-app)." as critical'
    exit 1
fi

# Ask for the folder to scan. A cancelled dialog makes osascript exit
# non-zero; just quit quietly in that case.
TARGET=$(osascript -e 'POSIX path of (choose folder with prompt "Choose a folder to scan for hidden metadata files:")' 2>/dev/null) || exit 0
[ -z "$TARGET" ] && exit 0
TARGET="${TARGET%/}"

# Build a small one-shot runner script rather than hand-escaping the
# binary/target paths into an AppleScript string — this keeps paths with
# spaces or other special characters safe.
RUNNER="$(mktemp -t clean_metadata_run)"
{
    echo '#!/bin/bash'
    printf '%q %q\n' "$BIN" "$TARGET"
    echo 'echo'
    echo 'echo "Press Enter to close this window."'
    echo 'read -r _'
    printf 'rm -f -- %q\n' "$RUNNER"
} > "$RUNNER"
chmod +x "$RUNNER"

osascript \
    -e 'tell application "Terminal"' \
    -e "  activate" \
    -e "  do script \"$RUNNER\"" \
    -e 'end tell'
