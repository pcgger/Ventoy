#!/bin/bash
# VtoyMac boot diagnostics (LaunchDaemon com.vtoymac.diag). Runs only while \EFI\VTOY\diag exists on the vdisk's ESP:
# every 30 s for 5 minutes after boot it rewrites \EFI\VTOY\boot-diag-<n>.log (readable from Windows) with the
# NVIDIA driver state, displays, WindowServer and the recent kernel log - also when the screen shows nothing.
PATH=/usr/bin:/bin:/usr/sbin:/sbin
find_esp() {
  for dev in $(diskutil list | awk '$2 == "EFI" { print $NF }'); do
    mp=$(mktemp -d /tmp/vtoydiag.XXXXXX)
    if diskutil mount -mountPoint "$mp" "$dev" >/dev/null 2>&1; then
      [ -f "$mp/EFI/VTOY/profiles.txt" ] && { echo "$mp"; return 0; }
      diskutil unmount "$mp" >/dev/null 2>&1
    fi
    rmdir "$mp" 2>/dev/null
  done
  return 1
}
ESP=$(find_esp) || exit 0
[ -f "$ESP/EFI/VTOY/diag" ] || { diskutil unmount "$ESP" >/dev/null 2>&1; exit 0; }
# keep the last three boots: boot-diag-1.log is the newest
mv -f "$ESP/EFI/VTOY/boot-diag-2.log" "$ESP/EFI/VTOY/boot-diag-3.log" 2>/dev/null
mv -f "$ESP/EFI/VTOY/boot-diag-1.log" "$ESP/EFI/VTOY/boot-diag-2.log" 2>/dev/null
OUT=/var/tmp/vtoy-boot-diag.log
run() { echo "----- $*"; "$@" 2>&1 | head -n "${LIMIT:-80}"; }
for i in 1 2 3 4 5 6 7 8 9 10; do
  {
    echo "=== pass $i  $(date '+%F %T')  up $(sysctl -n kern.boottime | sed 's/.*} //')"
    run sw_vers
    run sysctl -n kern.bootargs
    run csrutil status
    LIMIT=20 run sysctl debug.nvaccelfb debug.nvaccel_iop_flip
    run nvram -p | grep -i -E 'nullmoth|csr-active|boot-args'
    echo "----- nvidia kexts"; kmutil showloaded --list-only 2>/dev/null | grep -i -E 'nullmoth|nvidia|NVPrimary|AMFIPass|Lilu'
    echo "----- AuxKC"; ls -la /Library/KernelCollections /private/var/db/KernelExtensionManagement/AuxKC 2>&1
    echo "----- NVIDIA PCI"; ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | grep -B2 -A40 '"vendor-id" = <de100000>' | grep -E 'IOPCIDevice|assigned-addresses|IOName|class-code|IOPCIResourced|pcidebug|reg"' | head -20
    echo "----- framebuffers / displays"; ioreg -lw0 2>/dev/null | grep -E '\+-o .*(NVRM|NVAccel|NVRMFB|IOFramebuffer|IODisplay|AppleDisplay|IONDRV|AGDC)' | head -40
    LIMIT=30 run pgrep -lx WindowServer
    LIMIT=30 run ls -la /Library/Logs/DiagnosticReports
    echo "----- NullMoth logs"; tail -n 20 /Library/NullMoth/*.log 2>&1
    echo "----- kernel log (nv / gsp / display)"
    log show --last 6m --style compact --predicate 'process == "kernel" AND (eventMessage CONTAINS[c] "nv" OR eventMessage CONTAINS[c] "gsp" OR eventMessage CONTAINS[c] "framebuffer" OR eventMessage CONTAINS[c] "display" OR eventMessage CONTAINS[c] "panic")' 2>&1 | tail -n 250
    echo "----- WindowServer log"
    log show --last 3m --style compact --predicate 'process == "WindowServer"' 2>&1 | tail -n 60
  } > "$OUT" 2>&1
  cp "$OUT" "$ESP/EFI/VTOY/boot-diag-1.log"; sync
  sleep 30
done
diskutil unmount "$ESP" >/dev/null 2>&1
exit 0
