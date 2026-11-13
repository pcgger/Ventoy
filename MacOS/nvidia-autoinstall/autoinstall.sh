#!/bin/bash
# VtoyMac NVIDIA auto-installer. Installed as /Library/VtoyMac/nvidia/autoinstall.sh and started by the LaunchDaemon
# com.vtoymac.nvidia-autoinstall on every boot (also before Setup Assistant) until the NullMoth driver is installed.
#
#  - no NVIDIA GPU on PCI (VMware, other machines): log it and leave everything as it is
#  - otherwise run NullMoth's install.sh with the GSP firmware taken offline from the NVIDIA .run next to it,
#    then put \EFI\VTOY\nvidia-installed on the vdisk's EFI partition (VtoyBoot switches to the <profile>-nvidia
#    OpenCore config: NullMoth boot-args, IONDRVSupport excluded, native resolution) and restart.
#  - at most 3 attempts; every run's log is also copied to \EFI\VTOY\nvidia-autoinstall.log (readable from Windows).
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
BASE=/Library/VtoyMac/nvidia
LOG=/Library/Logs/vtoymac-nvidia-autoinstall.log
exec >>"$LOG" 2>&1
echo "=== $(date '+%F %T') macOS $(sw_vers -productVersion) $(sw_vers -buildVersion)"

ESP=""
REBOOT=0
find_esp() {  # mount the EFI partition that carries \EFI\VTOY\profiles.txt (the vdisk's own ESP)
  for dev in $(diskutil list | awk '$2 == "EFI" { print $NF }'); do
    mp=$(mktemp -d /tmp/vtoyesp.XXXXXX)
    if diskutil mount -mountPoint "$mp" "$dev" >/dev/null 2>&1; then
      if [ -f "$mp/EFI/VTOY/profiles.txt" ]; then ESP=$mp; echo "vdisk ESP: $dev at $mp"; return 0; fi
      diskutil unmount "$mp" >/dev/null 2>&1
    fi
    rmdir "$mp" 2>/dev/null
  done
  echo "vdisk ESP not found"; return 1
}
finish() {  # $1 = exit code
  if [ -n "$ESP" ]; then
    tail -n 400 "$LOG" > "$ESP/EFI/VTOY/nvidia-autoinstall.log" 2>/dev/null
    sync; diskutil unmount "$ESP" >/dev/null 2>&1
  fi
  # restart in the foreground: launchd kills the job's process group when this script exits
  [ "$REBOOT" = 1 ] && { echo "restarting"; sync; /sbin/shutdown -r now; }
  exit "$1"
}

[ -f "$BASE/done" ] && { echo "already installed, nothing to do"; exit 0; }
sleep 20   # let the system settle (diskutil, IOKit matching)
find_esp

if ! ioreg -r -c IOPCIDevice -d 1 | grep -q '"vendor-id" = <de100000>'; then
  echo "no NVIDIA GPU on this machine: nothing to install"
  finish 0
fi
n=$(cat "$BASE/attempts" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" > "$BASE/attempts"
if [ "$n" -gt 3 ]; then echo "gave up after 3 attempts (see earlier runs)"; finish 0; fi
echo "attempt $n: running NullMoth install.sh"

RUN="$BASE/NVIDIA-Linux-x86_64-620.70.06.run"
if NULLMOTH_FW_RUN="$RUN" /bin/bash "$BASE/pkgroot/install.sh"; then
  echo "install.sh succeeded"
  if [ -n "$ESP" ]; then
    date '+%F %T' > "$ESP/EFI/VTOY/nvidia-installed" && echo "set \\EFI\\VTOY\\nvidia-installed"
  else
    echo "WARNING: no vdisk ESP - OpenCore stays on the basic profile"
  fi
  touch "$BASE/done"
  REBOOT=1
  finish 0
fi
echo "install.sh failed (exit $?)"
finish 1
