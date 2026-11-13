#!/bin/sh
# Diagnostics from an installed system, run from macOS Recovery:  sh /Volumes/VTOYPAY/collect.sh
# Writes /Volumes/VTOYPAY/collect/: collect.txt (NullMoth / VtoyMac state, panics), kernel.txt (kernel log of the
# last boot, from the Data volume's unified log).
O=/Volumes/VTOYPAY/collect
rm -rf "$O"; mkdir -p "$O"
exec > "$O/collect.txt" 2>&1
set -x
D=""
for v in /Volumes/*; do [ -d "$v/private/var/db/diagnostics" ] && [ -d "$v/Users" ] && D="$v"; done
echo "Data volume: $D"
[ -n "$D" ] || exit 1
ls -la "$D/Library/NullMoth" "$D/Library/Extensions" "$D/Library/LaunchDaemons"
for f in "$D"/Library/NullMoth/*.log "$D"/Library/Logs/vtoymac-nvidia-autoinstall.log; do echo "===== $f"; cat "$f"; done
ls -la "$D/Library/Logs/DiagnosticReports" "$D/private/var/db/diagnostics" "$D/private/var/db/diagnostics/Persist" | tail -80
cp "$D"/Library/Logs/DiagnosticReports/*.panic "$D"/Library/Logs/DiagnosticReports/WindowServer*.ips "$O/" 2>/dev/null
ls -la "$D/Library/Apple/System/Library/KernelCollections" "$D/private/var/db/KernelExtensionManagement" 2>/dev/null

# unified log -> logarchive on the Data volume itself (the disk is attached non-persistent: nothing is kept)
A="$D/private/var/tmp/vtoy.logarchive"
rm -rf "$A"; mkdir -p "$A"
cp -R "$D/private/var/db/diagnostics/." "$A/"
cp -R "$D/private/var/db/uuidtext/." "$A/"
du -sh "$A"
log show "$A" --last boot --predicate 'process == "kernel"' --style compact > "$O/kernel.txt" 2> "$O/kernel.err"
log show "$A" --last boot --predicate 'process == "WindowServer" OR process == "launchd" OR eventMessage CONTAINS[c] "nvrm" OR eventMessage CONTAINS[c] "nullmoth"' --style compact > "$O/other.txt" 2>> "$O/kernel.err"
wc -l "$O"/*.txt
sync
echo "COLLECT DONE"
