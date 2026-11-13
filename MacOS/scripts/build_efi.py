"""Assemble OpenCore EFI folders for:
  vm  - VMware Workstation guest (install/maintenance), boots from OCBoot disk
  bm  - Dell Precision 5820 bare metal, lives in the ESP inside macOS.vhd.vtoy
Usage: build_efi.py <vm|bm> <outdir> [vtoymac_kext_dir] [acpi_dir]
"""
import copy, os, plistlib, shutil, sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'downloads')  # source/downloads
DEBUG = os.environ.get('OC_DEBUG') == '1'  # OC_DEBUG=1: DEBUG OpenCore + serial/file logging
OC = os.path.join(ROOT, 'OpenCorePkg-DEBUG' if DEBUG else 'OpenCorePkg')
KX = os.path.join(ROOT, 'kexts', 'x')


def find_kext(name):
    for dp, dn, _ in os.walk(KX):
        if name + '.kext' in dn and not os.path.isdir(os.path.join(dp, 'Contents')) and 'PlugIns' not in dp:
            return os.path.join(dp, name + '.kext')
    raise FileNotFoundError(name)


def kext_entry(name, src):
    exe = os.path.join(src, 'Contents', 'MacOS', name)
    return {
        'Arch': 'x86_64', 'BundlePath': name + '.kext', 'Comment': name, 'Enabled': True,
        'ExecutablePath': 'Contents/MacOS/' + name if os.path.exists(exe) else '',
        'MaxKernel': '', 'MinKernel': '', 'PlistPath': 'Contents/Info.plist',
    }


def build(kind, out, vtoymac=None, acpi_dir=None):
    efi = os.path.join(out, 'EFI')
    if os.path.exists(efi):
        shutil.rmtree(efi)
    os.makedirs(os.path.join(efi, 'BOOT'))
    shutil.copy(os.path.join(OC, 'X64', 'EFI', 'BOOT', 'BOOTx64.efi'), os.path.join(efi, 'BOOT', 'BOOTx64.efi'))
    # Hide OpenCore's own \EFI\BOOT\BOOTx64.efi from its entry list. Without this it is the first entry
    # ("OCBOOT"), which re-launches OC and hangs; with no picker it would also be the default on an empty disk.
    with open(os.path.join(efi, 'BOOT', '.contentVisibility'), 'w', newline='') as f:
        f.write('Disabled')
    ocdir = os.path.join(efi, 'OC')
    for sub in ('ACPI', 'Drivers', 'Kexts', 'Tools', 'Resources'):
        os.makedirs(os.path.join(ocdir, sub), exist_ok=True)
    shutil.copy(os.path.join(OC, 'X64', 'EFI', 'OC', 'OpenCore.efi'), ocdir)

    with open(os.path.join(OC, 'Docs', 'Sample.plist'), 'rb') as f:
        cfg = plistlib.load(f)

    # ---------------- Drivers ----------------
    drivers = ['OpenRuntime.efi', 'OpenHfsPlus.efi', 'ResetNvramEntry.efi']
    if kind == 'bm':
        drivers.append('ToggleSipEntry.efi')
    tmpl = cfg['UEFI']['Drivers'][0]
    cfg['UEFI']['Drivers'] = []
    for d in drivers:
        shutil.copy(os.path.join(OC, 'X64', 'EFI', 'OC', 'Drivers', d), os.path.join(ocdir, 'Drivers'))
        e = copy.deepcopy(tmpl)
        e.update({'Path': d, 'Enabled': True, 'LoadEarly': False, 'Arguments': '', 'Comment': d})
        cfg['UEFI']['Drivers'].append(e)
    shutil.copy(os.path.join(OC, 'X64', 'EFI', 'OC', 'Tools', 'OpenShell.efi'), os.path.join(ocdir, 'Tools'))
    cfg['Misc']['Tools'] = [t for t in cfg['Misc']['Tools'] if t['Path'] == 'OpenShell.efi']
    for t in cfg['Misc']['Tools']:
        t['Enabled'] = True

    # ---------------- Kexts (order matters: Lilu first) ----------------
    # OC_KEXT_SET=bm: VM build with the bare-metal kext list (VMware reproduction of the bare-metal environment)
    if kind == 'vm' and os.environ.get('OC_KEXT_SET') != 'bm':
        names = ['Lilu', 'VirtualSMC', 'RestrictEvents']
    else:
        names = ['Lilu', 'VirtualSMC', 'SMCProcessor', 'SMCSuperIO', 'AppleALC', 'IntelMausi',
                 'itlwm', 'NVMeFix', 'RestrictEvents', 'CpuTscSync']
    entries = []
    for n in names:
        src = find_kext(n)
        shutil.copytree(src, os.path.join(ocdir, 'Kexts', n + '.kext'))
        entries.append(kext_entry(n, src))
    if vtoymac:
        shutil.copytree(vtoymac, os.path.join(ocdir, 'Kexts', 'VtoyMac.kext'))
        entries.append(kext_entry('VtoyMac', vtoymac))
    cfg['Kernel']['Add'] = entries
    for sect in ('Block', 'Force', 'Patch'):
        cfg['Kernel'][sect] = []

    # ---------------- ACPI ----------------
    cfg['ACPI']['Patch'] = []
    cfg['ACPI']['Delete'] = []
    acpi = []
    if kind == 'bm':
        # No SSDT-PLUG(-ALT): the 5820 DSDT already has real Processor() objects; SSDT-PLUG-ALT added 64 duplicate
        # CPUs and macOS hung at "AppleACPICPU: ProcessorId=". macOS 12.3+ does not need plugin-type at all.
        if acpi_dir and os.path.exists(os.path.join(acpi_dir, 'SSDT-5820.aml')):
            shutil.copy(os.path.join(acpi_dir, 'SSDT-5820.aml'), os.path.join(ocdir, 'ACPI'))
            acpi.append('SSDT-5820.aml')
        else:
            print('WARNING: SSDT-5820.aml missing (compile it in CI) - EC/USBX/RTC not patched')
    cfg['ACPI']['Add'] = [{'Comment': a, 'Enabled': True, 'Path': a} for a in acpi]
    cfg['ACPI']['Quirks']['ResetLogoStatus'] = True

    # ---------------- Booter ----------------
    bq = cfg['Booter']['Quirks']
    cfg['Booter']['Patch'] = []
    cfg['Booter']['MmioWhitelist'] = []
    if kind == 'bm':
        # Keep the firmware's runtime mappings for the BIOS flash (NVRAM store) and the PCH sideband/SPI window.
        # With them devirtualised the kernel never got a working IODTNVRAM ("VtoyMac: nvram not ready" after 10 s)
        # and the APFS container on the Ventoy VDisk was never probed. Regions from the OC log "MMIO devirt".
        for addr, comment in ((0xFF000000, 'BIOS flash (NVRAM store), 16 MB'),
                              (0xFD000000, 'PCH sideband / SPI controller')):
            cfg['Booter']['MmioWhitelist'].append({'Address': addr, 'Comment': comment, 'Enabled': True})
    # bm: SetupVirtualMap off. The Dell 5820 firmware has a MAT ("OCABC: MAT support is 1"); with SetupVirtualMap
    # on it hung right after EXITBS:START (no kernel output, APFS never mounted). Same as HP Z4 / Lenovo P520 configs.
    bq.update({'AvoidRuntimeDefrag': True, 'ProvideCustomSlide': True, 'SetupVirtualMap': kind != 'bm',
               'EnableWriteUnprotector': kind == 'vm', 'RebuildAppleMemoryMap': kind == 'bm',
               'SyncRuntimePermissions': kind == 'bm', 'DevirtualiseMmio': kind == 'bm',
               'ResizeAppleGpuBars': -1})

    # ---------------- Kernel quirks ----------------
    kq = cfg['Kernel']['Quirks']
    kq.update({'DisableLinkeditJettison': True, 'PanicNoKextDump': True, 'PowerTimeoutKernelPanic': True,
               'AppleXcpmCfgLock': kind == 'bm', 'AppleXcpmExtraMsrs': kind == 'bm', 'AppleCpuPmCfgLock': False,
               'DisableIoMapper': kind == 'bm', 'XhciPortLimit': False, 'ProvideCurrentCpuInfo': False})
    cfg['Kernel']['Scheme']['KernelArch'] = 'x86_64'

    # ---------------- Misc ----------------
    m = cfg['Misc']
    # VM: show recovery (auxiliary) entries and auto-boot the default, no keyboard needed
    # VM: the builtin picker hangs while drawing once the macOS disk has APFS volumes (VMware, OC 1.0.8),
    # so the VM skips the picker and boots the default entry (the macOS-blessed one: installer, then macOS).
    # bm: user wants it to look like a real Mac - no picker either; the default entry is macOS (only APFS is
    # scanned). Hot keys still work while booting: Win+V = verbose, Win+R = recovery, Alt = show picker.
    m['Boot'].update({'HideAuxiliary': kind == 'bm', 'PickerMode': 'Builtin', 'ShowPicker': False,
                      'Timeout': 0,
                      'PollAppleHotKeys': True, 'PickerAttributes': 17})
    m['Debug'].update({'AppleDebug': True, 'ApplePanic': True, 'DisableWatchDog': True,
                       # nothing on screen: bm logs to a file on the ESP only (0x41), vm logs nothing
                       'Target': 0x41 if kind == 'bm' else 0})
    # bm: list APFS volumes only (0x101 = FILE_SYSTEM_LOCK | ALLOW_FS_APFS). Without it the picker also shows
    # Ventoy's own EFI partition; choosing it re-enters Ventoy and a second OpenCore launch halts
    # ("Failed to start OpenCore image - Already started"). No device lock: the Ventoy vdisk is a virtual BlockIo.
    m['Security'].update({'AllowSetDefault': True, 'ScanPolicy': 0x101 if kind == 'bm' else 0, 'SecureBootModel': 'Disabled', 'Vault': 'Optional',
                          'ExposeSensitiveData': 6, 'BlacklistAppleUpdate': True, 'DmgLoading': 'Signed'})
    m['Entries'] = []
    if DEBUG:
        # enable + onscreen + serial + file (opencore-*.txt on the ESP); error/warn/info + verbose
        # vm: VMware has a serial port -> serial.log. bm: the workstation may have no UART; log to screen + file only
        m['Debug'].update({'Target': 0x43 if kind == 'bm' else 0x4B, 'DisplayLevel': 0x80400042, 'DisplayDelay': 0})
        m['Serial']['Init'] = kind != 'bm'

    # ---------------- NVRAM ----------------
    add = cfg['NVRAM']['Add']
    boot = add['7C436110-AB2A-4BBB-A880-FE41995C9F82']
    # No -v: show the Apple logo + progress bar instead of scrolling kernel text (DEBUG builds keep -v)
    args = 'keepsyms=1 debug=0x100 revpatch=sbvmm'
    if kind == 'bm':
        # npci=0x2000: keep the firmware's PCI resource layout. Without it the 5820 (C422) hung at
        # "[IOPCIConfigurator::configure()] PCI configuration PC00" after "AppleUSBXHCI: unable to get device memory".
        args += ' alcid=1 npci=0x2000'
    vmap_path = os.path.join(out, '..', 'vtoy-map.bin')
    if kind == 'bm' and os.path.exists(vmap_path):
        # same location as vtoy-map, as boot-args: the kernel can read these before IODTNVRAM exists
        import struct
        with open(vmap_path, 'rb') as f:
            vm_ = f.read()
        sig, = struct.unpack_from('<I', vm_, 8)
        lba, nbytes = struct.unpack_from('<QQ', vm_, 36)
        args += f' vtoysig=0x{sig:08X} vtoylba={lba} vtoysize={nbytes}'
    if DEBUG:
        args = '-v ' + args + (' serial=3' if kind != 'bm' else '')  # verbose text (+ kernel log to COM1 in the VM)
    if os.environ.get('OC_EXTRA_ARGS'):  # e.g. the VMware Ventoy-layout test: vtoysig/vtoylba/vtoysize of the test disk
        args += ' ' + os.environ['OC_EXTRA_ARGS']
    boot['boot-args'] = args
    boot['prev-lang:kbd'] = b'zh-Hans:252'
    boot['csr-active-config'] = b'\x00\x00\x00\x00'
    cfg['NVRAM']['WriteFlash'] = True
    # VtoyMac fallback map (from source/ventoy-fork/tools/vtoy-map.ps1), used when macOS cannot see Ventoy's
    # volatile VentoyOsParam variable. Deleted + re-added on every boot so a moved file gets the new location.
    vmap = os.path.join(out, '..', 'vtoy-map.bin')
    if kind == 'bm' and os.path.exists(vmap):
        guid = '6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21'
        with open(vmap, 'rb') as f:
            add[guid] = {'vtoy-map': f.read()}
        cfg['NVRAM'].setdefault('Delete', {})[guid] = ['vtoy-map']
        print(f'vtoy-map added from {vmap}')

    # ---------------- PlatformInfo ----------------
    pi = cfg['PlatformInfo']
    pi['Generic'].update({'SystemProductName': 'MacPro7,1', 'SpoofVendor': True, 'AdviseFeatures': True})
    pi['UpdateSMBIOSMode'] = 'Custom' if kind == 'bm' else 'Create'
    serial = os.path.join(out, '..', 'smbios.plist')
    if os.path.exists(serial):
        with open(serial, 'rb') as f:
            pi['Generic'].update(plistlib.load(f))

    # ---------------- UEFI ----------------
    u = cfg['UEFI']
    u['ConnectDrivers'] = True
    # VM: no macOS graphics driver -> WindowServer renders on the CPU; fewer pixels = much smoother
    # No GPU acceleration until the NVIDIA driver is installed: WindowServer draws on the CPU, so keep the
    # framebuffer small. bm was 'Max' (4K) - far laggier than the VM. Raise it again once the driver works.
    u['Output'].update({'ProvideConsoleGop': True, 'Resolution': '1920x1080' if kind == 'bm' else '1440x900'})
    u['Quirks'].update({'RequestBootVarRouting': True, 'ReleaseUsbOwnership': kind == 'bm', 'UnblockFsConnect': False})
    u['APFS'].update({'MinDate': -1, 'MinVersion': -1, 'EnableJumpstart': True})

    with open(os.path.join(ocdir, 'config.plist'), 'wb') as f:
        plistlib.dump(cfg, f)
    print(f'{kind} EFI written to {efi}: kexts={[e["BundlePath"] for e in entries]} acpi={acpi}')


if __name__ == '__main__':
    build(sys.argv[1], sys.argv[2], *(sys.argv[3:5]))
