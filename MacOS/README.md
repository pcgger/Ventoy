# VtoyMac — boot macOS from a Ventoy vdisk

This directory adds macOS support to Ventoy's **vDisk boot** (the `*.vtoy` file mode
that already works for Linux through [vtoyboot](https://github.com/ventoy/vtoyboot)).

## How it works

| Stage | Who | What happens |
|---|---|---|
| UEFI | Ventoy (unchanged) | `macOS.vhd.vtoy` is selected in the menu; `ventoy_x64.efi sector512` exposes the file as a block device and chain-loads `\EFI\BOOT\BOOTx64.efi` from the vdisk's ESP. It also sets the EFI variable `VentoyOsParam` and leaves the image-location table in runtime memory. |
| UEFI | **VtoyBoot.efi** (`\EFI\BOOT\BOOTx64.efi` of the vdisk) | Identifies the machine (SMBIOS system/board, CPUID vendor + model), picks a profile from `\EFI\VTOY\profiles.txt` (dell-5820, intel, intel-hybrid, amd, amd-sv, vm), writes `\EFI\OC\config.plist` from it (the `-nvidia` variant once `\EFI\VTOY\nvidia-installed` exists), copies `VentoyOsParam` + the location table to ACPI NVS memory, passes `vtoyparam=<phys>` and starts OpenCore. No menu, no text. |
| UEFI | OpenCore (inside the vdisk ESP) | Normal OpenCore boot: picks the APFS container on the vdisk, injects kexts incl. **VtoyMac.kext**. |
| XNU | **VtoyMac.kext** | An `IOPartitionScheme` that attaches to the physical disk carrying the Ventoy partition, reads `VentoyOsParam` + the location table, validates the image (disk GUID/signature, GPT header, fixed-VHD footer) and publishes the image sectors as a whole-disk `IOMedia` ("Ventoy VDisk"). |
| XNU | Apple GUID scheme + APFS | Match on the new media as if it were a real disk; root volume mounts. |

The same `macOS.vhd.vtoy` also runs in VMware Workstation through a flat VMDK descriptor
(`MacOS/vm/macOS.vmdk`), so the system can be installed, maintained and backed up from Windows.

## Requirements / rules

* UEFI boot, Ventoy partition on NTFS or exFAT, 512-byte sectors.
* **Fixed-size** VHD (or raw `.img.vtoy`), **contiguous** on disk (`tools/vtoy-contig.ps1` checks).
* Backup = copy the file. Restore = copy it back (Ventoy re-reads the location on every boot).
* No VMware snapshots on the vdisk (they write to a separate delta file Ventoy cannot see).
* While macOS runs from the vdisk the physical Ventoy disk is claimed by VtoyMac, so its
  other partitions are not visible in macOS. Boot argument `-vtoymacoff` disables the kext.

## NVIDIA (Ampere/Ada, NullMoth driver)

`nvidia-autoinstall/` holds a LaunchDaemon that runs at every boot (also before Setup Assistant)
until the driver is installed: without an NVIDIA GPU it only logs; with one it runs NullMoth's
`install.sh` (GSP firmware from the NVIDIA `.run` next to it), sets `\EFI\VTOY\nvidia-installed`
and restarts. Stage it once from macOS Recovery with `stage.sh` from a FAT volume that carries the
package and the `.run`. Its log is copied to `\EFI\VTOY\nvidia-autoinstall.log`.

## Build

GitHub Actions (`.github/workflows/vtoymac.yml`) builds `VtoyMac.kext` on a macOS runner and
`VtoyBoot.efi` + `SSDT-*.aml` on Ubuntu (clang/lld-link, acpica).
Locally on a Mac: `make -C MacOS/VtoyMac`.
`scripts/build_vtoy_efi.py <out> <VtoyMac.kext> <aml dir> <VtoyBoot.efi> [--debug]` assembles the universal ESP.

## Fallback location source

If a firmware does not pass `VentoyOsParam` to macOS, OpenCore can provide the location instead:
`NVRAM → Add → 6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21 → vtoy-map` (binary, see `VtoyMac.hpp`),
generated on Windows by `tools/vtoy-map.ps1`.
