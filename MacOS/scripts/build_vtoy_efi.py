"""Build the universal EFI for a macOS Ventoy vdisk: VtoyBoot + OpenCore + one config template per platform.

Usage: build_vtoy_efi.py <outdir> <VtoyMac.kext> <dir with *.aml> <VtoyBoot.efi> [--debug]

  EFI/BOOT/BOOTx64.efi          VtoyBoot (picks the profile, hands Ventoy's location to VtoyMac, starts OpenCore)
  EFI/OC/...                    OpenCore with the union of all kexts / SSDTs / drivers
  EFI/VTOY/profiles.txt         first matching rule wins (SMBIOS / CPUID)
  EFI/VTOY/profiles/<p>.plist   OpenCore config templates (@VTOYARGS@ / @CORES:..@ placeholders)
  EFI/VTOY/profiles/<p>-nvidia.plist   used once \\EFI\\VTOY\\nvidia-installed exists

Profiles: dell-5820 (validated on hardware), intel, intel-hybrid (12th-14th gen), amd, amd-sv, vm (VMware).
Every profile is "NVIDIA ready": SIP 0x0A43, AMFI boot-args, AMFIPass. The -nvidia variants add the NullMoth
boot-args, exclude IONDRVSupport and use the native resolution.
"""
import base64, copy, os, plistlib, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
if '--debug' in sys.argv:
    os.environ['OC_DEBUG'] = '1'
    sys.argv.remove('--debug')
sys.path.insert(0, HERE)
import build_efi  # noqa: E402  (reads OC_DEBUG at import)

DL = build_efi.ROOT
AMFI_ARGS = 'amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0'
NV_ARGS = 'nvfb=1 nvaccel=1 nvfbheads=4 -nvkmsnosmooth'
CSR_NV = b'\x43\x0A\x00\x00'
AMD_PATCHES = os.path.join(DL, 'amd', 'AMD_Vanilla-eaf52ef-patches.plist')

RULES = """# VtoyBoot profile rules - first match wins.
# keys: sys-vendor sys-product board-vendor board-product cpu-vendor cpu-model (FF-MM hex), or * (always)
vm            sys-vendor=VMware
dell-5820     sys-product=Precision 5820
intel-hybrid  cpu-model=06-97
intel-hybrid  cpu-model=06-9A
intel-hybrid  cpu-model=06-B7
intel-hybrid  cpu-model=06-BA
intel-hybrid  cpu-model=06-BF
amd-sv        board-product=X570
amd-sv        board-product=B450
amd-sv        board-product=X470
amd-sv        board-product=B350
amd-sv        board-product=X370
amd           cpu-vendor=AuthenticAMD
intel         cpu-vendor=GenuineIntel
intel         *
"""

KEXTS = {
    'dell-5820': ['Lilu', 'AMFIPass', 'VirtualSMC', 'SMCProcessor', 'SMCSuperIO', 'AppleALC', 'IntelMausi', 'itlwm',
                  'NVMeFix', 'RestrictEvents', 'CpuTscSync', 'VtoyMac'],
    'intel': ['Lilu', 'AMFIPass', 'VirtualSMC', 'SMCProcessor', 'SMCSuperIO', 'AppleALC', 'IntelMausi',
              'LucyRTL8125Ethernet', 'RealtekRTL8111', 'itlwm', 'NVMeFix', 'RestrictEvents', 'VtoyMac'],
    'amd': ['Lilu', 'AMFIPass', 'VirtualSMC', 'AppleALC', 'IntelMausi', 'LucyRTL8125Ethernet', 'RealtekRTL8111',
            'itlwm', 'NVMeFix', 'RestrictEvents', 'VtoyMac'],
    'vm': ['Lilu', 'AMFIPass', 'VirtualSMC', 'RestrictEvents', 'VtoyMac'],
}
KEXTS['intel-hybrid'] = KEXTS['intel']
KEXTS['amd-sv'] = KEXTS['amd']


def kext_src(name, vtoymac):
    return vtoymac if name == 'VtoyMac' else build_efi.find_kext(name)


def set_args(cfg, args):
    cfg['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']['boot-args'] = args


def get_args(cfg):
    return cfg['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']['boot-args']


def strip_legacy_vtoy(args):
    return ' '.join(a for a in args.split() if not a.startswith(('vtoysig=', 'vtoylba=', 'vtoysize=')))


def profile(base, vmbase, name, vtoymac):
    cfg = copy.deepcopy(vmbase if name == 'vm' else base)
    kq, bq = cfg['Kernel']['Quirks'], cfg['Booter']['Quirks']
    cfg['Kernel']['Add'] = [build_efi.kext_entry(k, kext_src(k, vtoymac)) for k in KEXTS[name]]
    args = get_args(cfg)
    if name != 'dell-5820':
        # the hardcoded vdisk location only belongs to the Dell's disk; everyone else gets it from VtoyBoot
        args = strip_legacy_vtoy(args).replace(' npci=0x2000', '')
        cfg['NVRAM']['Add'].pop('6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21', None)
        cfg['NVRAM'].get('Delete', {}).pop('6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21', None)
    if name == 'dell-5820':
        acpi = ['SSDT-5820.aml']
        # FADT reset register is an SMI (I/O 0xB2 <- 0x73): macOS restart stops after "launchd quiesce complete"
        # (seen 2026-11-13). Use the PCH reset control register instead (I/O 0xCF9 <- 0x06, full reset).
        cfg['ACPI']['Patch'] = [{
            'Base': '', 'BaseSkip': 0, 'Comment': 'FADT reset: SMI 0xB2/0x73 -> 0xCF9/0x06', 'Count': 1,
            'Enabled': True, 'Find': bytes.fromhex('01080000B2000000000000' + '0073'),
            'Limit': 0, 'Mask': b'', 'OemTableId': bytes(8),
            'Replace': bytes.fromhex('01080000F90C0000000000' + '0006'),
            'ReplaceMask': b'', 'Skip': 0, 'TableLength': 0, 'TableSignature': b'FACP'}]
    elif name == 'vm':
        acpi = []
    else:
        acpi = ['SSDT-VTOY-GENERIC.aml']
        cfg['Booter']['MmioWhitelist'] = []
        bq.update({'SetupVirtualMap': True, 'DevirtualiseMmio': True, 'RebuildAppleMemoryMap': True,
                   'SyncRuntimePermissions': True, 'ProtectUefiServices': True, 'EnableWriteUnprotector': False})
        kq.update({'AppleXcpmCfgLock': True, 'AppleXcpmExtraMsrs': False, 'DisableIoMapper': True,
                   'ProvideCurrentCpuInfo': True})
    cfg['ACPI']['Add'] = [{'Comment': a, 'Enabled': True, 'Path': a} for a in acpi]
    # APFS only, every profile: otherwise OpenCore lists Ventoy's own VTOYEFI partition and, with no macOS entry,
    # boots it -> Ventoy -> this vdisk again -> "OC: Found previous image, aborting" (seen in VMware)
    cfg['Misc']['Security']['ScanPolicy'] = 0x101
    cfg['Misc']['Boot']['HideAuxiliary'] = True
    if name == 'intel-hybrid':
        # Alder/Raptor Lake: report a Comet Lake CPUID (Dortania), real topology from ProvideCurrentCpuInfo
        cfg['Kernel']['Emulate'].update({'Cpuid1Data': b'\x55\x06\x0A\x00' + bytes(12),
                                         'Cpuid1Mask': b'\xFF\xFF\xFF\xFF' + bytes(12)})
    if name in ('amd', 'amd-sv'):
        bq.update({'SetupVirtualMap': name == 'amd-sv', 'DevirtualiseMmio': False, 'ProtectUefiServices': False})
        kq.update({'AppleXcpmCfgLock': False, 'AppleXcpmExtraMsrs': False, 'DisableIoMapper': False,
                   'ProvideCurrentCpuInfo': True})
        with open(AMD_PATCHES, 'rb') as f:
            cfg['Kernel']['Patch'] = plistlib.load(f)['Kernel']['Patch']
        cfg['Kernel']['Block'] = [{'Arch': 'x86_64', 'Comment': 'AppleMCEReporter panics on AMD with MacPro7,1',
                                   'Enabled': True, 'Identifier': 'com.apple.driver.AppleMCEReporter',
                                   'MaxKernel': '', 'MinKernel': '', 'Strategy': 'Exclude'}]
    # NVIDIA ready (all profiles): SIP bits the driver needs, AMFI off, AMFIPass loaded
    # Delete first, or OpenCore keeps whatever an older config left in flash ("ignored, exists": seen on the Dell,
    # SIP stayed on). The old hardcoded vtoy-map must go too: after the image is copied it points at stale sectors.
    apple = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
    cfg['NVRAM']['Add'][apple]['csr-active-config'] = CSR_NV
    cfg['NVRAM']['Add'].pop('6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21', None)
    dl = cfg['NVRAM'].setdefault('Delete', {})
    dl.setdefault(apple, [])
    if 'csr-active-config' not in dl[apple]:
        dl[apple].append('csr-active-config')
    dl['6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21'] = ['vtoy-map']
    args = f'{args} {AMFI_ARGS} @VTOYARGS@'
    set_args(cfg, args)
    return cfg


def nvidia(cfg):
    c = copy.deepcopy(cfg)
    set_args(c, get_args(c).replace('@VTOYARGS@', f'{NV_ARGS} @VTOYARGS@'))
    c['Kernel']['Block'] = c['Kernel'].get('Block', []) + [{
        'Arch': 'x86_64', 'Comment': 'IONDRVSupport: NullMoth NVRMFB must own display 0', 'Enabled': True,
        'Identifier': 'com.apple.iokit.IONDRVSupport', 'MaxKernel': '', 'MinKernel': '', 'Strategy': 'Exclude'}]
    # Native resolution only without -v: a verbose console on a 4K firmware framebuffer scrolls by moving ~33 MB of
    # VRAM per line over PCIe, which made the Dell's debug boots look frozen for many minutes (2026-11-13).
    if '-v' not in get_args(c).split():
        c['UEFI']['Output']['Resolution'] = 'Max'
    return c


def dump_template(cfg, path):
    """plistlib cannot write placeholders inside <data>: write sentinels, then swap in the @CORES:..@ tokens."""
    cfg = copy.deepcopy(cfg)
    tokens = {}
    for i, p in enumerate(cfg['Kernel'].get('Patch', [])):
        if 'cpuid_cores_per_package' in p.get('Comment', ''):
            r = p['Replace']
            sentinel = b'VTOYCORE%02d' % i
            tokens[base64.b64encode(sentinel).decode()] = f'@CORES:{r[:1].hex()}:{r[2:].hex()}@'
            p['Replace'] = sentinel
    text = plistlib.dumps(cfg).decode('utf-8')
    for s, t in tokens.items():
        assert s in text, s
        text = text.replace(s, t)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


def main(out, vtoymac, aml_dir, vtoyboot):
    tmp = os.path.join(out, '_base')
    for d in (os.path.join(out, 'EFI'), tmp):
        if os.path.exists(d):
            shutil.rmtree(d)
    build_efi.build('bm', tmp, vtoymac, aml_dir)
    with open(os.path.join(tmp, 'EFI', 'OC', 'config.plist'), 'rb') as f:
        base = plistlib.load(f)
    vmtmp = os.path.join(out, '_vm')
    build_efi.build('vm', vmtmp, vtoymac)
    with open(os.path.join(vmtmp, 'EFI', 'OC', 'config.plist'), 'rb') as f:
        vmbase = plistlib.load(f)
    shutil.copytree(os.path.join(tmp, 'EFI'), os.path.join(out, 'EFI'))
    shutil.rmtree(tmp); shutil.rmtree(vmtmp)
    oc = os.path.join(out, 'EFI', 'OC')
    # union of kexts and SSDTs
    for k in sorted({k for v in KEXTS.values() for k in v}):
        dst = os.path.join(oc, 'Kexts', k + '.kext')
        if not os.path.exists(dst):
            shutil.copytree(kext_src(k, vtoymac), dst)
    for a in ('SSDT-5820.aml', 'SSDT-VTOY-GENERIC.aml'):
        shutil.copy(os.path.join(aml_dir, a), os.path.join(oc, 'ACPI', a))
    shutil.copy(vtoyboot, os.path.join(out, 'EFI', 'BOOT', 'BOOTx64.efi'))
    vt = os.path.join(out, 'EFI', 'VTOY', 'profiles')
    os.makedirs(vt)
    with open(os.path.join(out, 'EFI', 'VTOY', 'profiles.txt'), 'w', newline='\n') as f:
        f.write(RULES)
    names = ['dell-5820', 'intel', 'intel-hybrid', 'amd', 'amd-sv', 'vm']
    for n in names:
        cfg = profile(base, vmbase, n, vtoymac)
        for k in cfg['Kernel']['Add']:
            assert os.path.isdir(os.path.join(oc, 'Kexts', k['BundlePath'])), k['BundlePath']
        for a in cfg['ACPI']['Add']:
            assert os.path.isfile(os.path.join(oc, 'ACPI', a['Path'])), a['Path']
        dump_template(cfg, os.path.join(vt, n + '.plist'))
        if n != 'vm':
            dump_template(nvidia(cfg), os.path.join(vt, n + '-nvidia.plist'))
    # initial config.plist (VtoyBoot rewrites it on every boot): the Dell profile with no Ventoy args
    with open(os.path.join(vt, 'dell-5820.plist'), encoding='utf-8') as f:
        initial = f.read().replace('@VTOYARGS@', '')
    with open(os.path.join(oc, 'config.plist'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(initial)
    print('profiles:', ', '.join(sorted(os.listdir(vt))))
    print('kexts:', ', '.join(sorted(os.listdir(os.path.join(oc, 'Kexts')))))


if __name__ == '__main__':
    main(*sys.argv[1:5])
