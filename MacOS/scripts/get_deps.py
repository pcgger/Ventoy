"""Download OpenCore + kexts (pinned versions) from GitHub releases into downloads/ and extract them.
Usage: get_deps.py <downloads_dir>"""
import json, os, sys, zipfile
from urllib.request import Request, urlopen

PINS = [  # (repo, tag, asset name substring) -> extracted to kexts/x/<repo>/ or OpenCorePkg/
    ('acidanthera/OpenCorePkg', '1.0.8', 'RELEASE.zip'),
    ('acidanthera/OpenCorePkg', '1.0.8', 'DEBUG.zip'),  # verbose logging build, for troubleshooting
    ('acidanthera/Lilu', '1.7.2', 'RELEASE.zip'),
    ('acidanthera/VirtualSMC', '1.3.8', 'RELEASE.zip'),
    ('acidanthera/WhateverGreen', '1.7.1', 'RELEASE.zip'),
    ('acidanthera/AppleALC', '1.9.8', 'RELEASE.zip'),
    ('acidanthera/RestrictEvents', '1.1.6', 'RELEASE.zip'),
    ('acidanthera/IntelMausi', '1.0.8', 'RELEASE.zip'),
    ('acidanthera/NVMeFix', '1.1.3', 'RELEASE.zip'),
    ('acidanthera/CryptexFixup', '1.0.5', 'RELEASE.zip'),
    ('acidanthera/CpuTscSync', '1.1.2', 'RELEASE.zip'),  # TSC sync across cores (HEDT/Xeon lag)
    ('Mieze/LucyRTL8125Ethernet', 'v1.2.4', 'LucyRTL8125Ethernet-V1.2.4.zip'),  # Realtek 2.5 GbE (ROG etc.)
    ('Mieze/RTL8111_driver_for_OS_X', 'v3.0.0', 'RealtekRTL8111-V3.0.0.zip'),  # Realtek 1 GbE
    ('OpenIntelWireless/itlwm', 'v2.3.0', 'itlwm_v2.3.0_stable.kext.zip'),
    ('USBToolBox/kext', '1.2.0', 'RELEASE.zip'),
]
H = {'User-Agent': 'deps-fetch', 'Accept': 'application/vnd.github+json'}


def main(root):
    os.makedirs(os.path.join(root, 'kexts', 'x'), exist_ok=True)
    for repo, tag, sub in PINS:
        rel = json.load(urlopen(Request(f'https://api.github.com/repos/{repo}/releases/tags/{tag}', headers=H)))
        asset = next(a for a in rel['assets'] if sub in a['name'])
        name = repo.split('/')[1]
        is_oc = name == 'OpenCorePkg'
        zp = os.path.join(root, asset['name'] if is_oc else os.path.join('kexts', asset['name']))
        if not os.path.exists(zp):
            with urlopen(Request(asset['browser_download_url'], headers={'User-Agent': 'deps-fetch'})) as r, open(zp, 'wb') as f:
                f.write(r.read())
        oc_dir = 'OpenCorePkg-DEBUG' if 'DEBUG' in sub else 'OpenCorePkg'
        dst = os.path.join(root, oc_dir) if is_oc else os.path.join(root, 'kexts', 'x', name)
        with zipfile.ZipFile(zp) as z:
            for m in z.namelist():  # refuse path traversal
                if m.startswith(('/', '\\')) or '..' in m.replace('\\', '/').split('/'):
                    raise SystemExit(f'bad path in {zp}: {m}')
            z.extractall(dst)
        print(f'{repo} {tag}: {asset["name"]} ({asset["size"]} bytes)')


if __name__ == '__main__':
    main(sys.argv[1])
