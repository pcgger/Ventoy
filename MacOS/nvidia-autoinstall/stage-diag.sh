#!/bin/sh
# From macOS Recovery: sh /Volumes/VTOYPAY/stage-diag.sh  -> installs the boot diagnostics daemon (diag.sh) on the
# Data volume and writes stage-diag-result.txt next to itself.
SRC=$(cd "$(dirname "$0")" && pwd)
exec > "$SRC/stage-diag-result.txt" 2>&1
set -x
DATA=""
for v in /Volumes/*; do [ -d "$v/private/var/db" ] && [ -d "$v/Users" ] && [ -d "$v/Library" ] && DATA="$v"; done
[ -n "$DATA" ] || { echo "STAGE FAILED: no Data volume"; exit 1; }
mkdir -p "$DATA/Library/VtoyMac" || exit 1
cp "$SRC/diag.sh" "$DATA/Library/VtoyMac/diag.sh" || exit 1
cp "$SRC/com.vtoymac.diag.plist" "$DATA/Library/LaunchDaemons/" || exit 1
chown 0:0 "$DATA/Library/VtoyMac/diag.sh" "$DATA/Library/LaunchDaemons/com.vtoymac.diag.plist"
chmod 755 "$DATA/Library/VtoyMac/diag.sh"
chmod 644 "$DATA/Library/LaunchDaemons/com.vtoymac.diag.plist"
ls -la "$DATA/Library/VtoyMac" "$DATA/Library/LaunchDaemons"
sync
echo "STAGE OK"
