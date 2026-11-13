#!/bin/sh
# Run once from macOS Recovery (Terminal):  sh /Volumes/VTOYPAY/stage.sh
# Copies the NVIDIA auto-installer onto the installed system's Data volume:
#   /Library/VtoyMac/nvidia/{pkgroot, NVIDIA-Linux-x86_64-620.70.06.run, autoinstall.sh}
#   /Library/LaunchDaemons/com.vtoymac.nvidia-autoinstall.plist
# and writes stage-result.txt next to itself.
SRC=$(cd "$(dirname "$0")" && pwd)
OUT="$SRC/stage-result.txt"
exec > "$OUT" 2>&1
set -x

DATA=""
for v in /Volumes/*; do
  [ -d "$v/private/var/db" ] && [ -d "$v/Users" ] && [ -d "$v/Library" ] && DATA="$v"
done
if [ -z "$DATA" ]; then
  for dev in $(diskutil apfs list | awk '/\(Data\)/ { for (i = 1; i <= NF; i++) if ($i ~ /^disk[0-9]+s[0-9]+$/) print $i }'); do
    diskutil mount "$dev"
    mp=$(diskutil info "$dev" | awk -F': *' '/Mount Point/ { print $2 }')
    [ -n "$mp" ] && [ -d "$mp/private/var" ] && DATA="$mp"
  done
fi
[ -n "$DATA" ] || { echo "STAGE FAILED: no Data volume"; exit 1; }
echo "Data volume: $DATA"

B="$DATA/Library/VtoyMac/nvidia"
rm -rf "$B"
mkdir -p "$B" || { echo "STAGE FAILED: mkdir"; exit 1; }
tar -xzf "$SRC/nullmoth-nvidia-2.9.tar.gz" -C "$B" || { echo "STAGE FAILED: untar"; exit 1; }
cp "$SRC/NVIDIA-Linux-x86_64-620.70.06.run" "$B/" || { echo "STAGE FAILED: copy .run"; exit 1; }
cp "$SRC/autoinstall.sh" "$B/autoinstall.sh"
cp "$SRC/com.vtoymac.nvidia-autoinstall.plist" "$DATA/Library/LaunchDaemons/"
chown -R 0:0 "$DATA/Library/VtoyMac" "$DATA/Library/LaunchDaemons/com.vtoymac.nvidia-autoinstall.plist"
chmod 755 "$DATA/Library/VtoyMac" "$B" "$B/autoinstall.sh"
chmod 644 "$DATA/Library/LaunchDaemons/com.vtoymac.nvidia-autoinstall.plist" "$B/NVIDIA-Linux-x86_64-620.70.06.run"
chmod -R go-w "$B/pkgroot"
(cd "$B/pkgroot" && shasum -a 256 -c SHA256SUMS --quiet) && echo "package sums OK"
shasum -a 256 "$B/NVIDIA-Linux-x86_64-620.70.06.run"
ls -la "$B" "$DATA/Library/LaunchDaemons"
sync
echo "STAGE OK"
