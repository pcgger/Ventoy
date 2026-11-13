"""Download pinned extras that are not GitHub-release kexts, verifying each against a pinned hash.
Usage: get_extras.py <downloads_dir> [name ...]   (default: all)

  amfipass   AMFIPass 1.4.1 (OpenCore Legacy Patcher payload; NVIDIA driver needs it with AMFI off)
  nvrun      NVIDIA-Linux-x86_64-620.70.06.run (the NVIDIA driver's GSP firmware source, pinned in fw_fetch.sh)
  ventoy     Ventoy Windows release (VTOYEFI image + boot code for the VMware Ventoy test disk)
"""
import hashlib, json, os, sys, zipfile
from urllib.request import Request, urlopen

UA = {'User-Agent': 'extras-fetch'}
ITEMS = {
    'amfipass': dict(url='https://raw.githubusercontent.com/dortania/OpenCore-Legacy-Patcher/main/payloads/Kexts/Acidanthera/AMFIPass-v1.4.1-RELEASE.zip',
                     file='AMFIPass-v1.4.1-RELEASE.zip', git_blob='b42fe05c39c4da22d239efedfb1111c51f226eee', unzip='kexts/x/AMFIPass'),
    'amdvanilla': dict(url='https://raw.githubusercontent.com/AMD-OSX/AMD_Vanilla/eaf52ef292abf4ebec899df6d48626569ba50cc6/patches.plist',
                       file='amd/AMD_Vanilla-eaf52ef-patches.plist'),
    'nvrun': dict(url='https://download.nvidia.com/XFree86/Linux-x86_64/620.70.06/NVIDIA-Linux-x86_64-620.70.06.run',
                  file='nvidia-v2.9/NVIDIA-Linux-x86_64-620.70.06.run',
                  sha256='29118e3abd08c07559bcca3d9c76238d039490af0362a2e5bcfcbe7204afd4b1'),
}


def fetch(url, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.part'
    with urlopen(Request(url, headers=UA)) as r, open(tmp, 'wb') as f:
        while chunk := r.read(1 << 22):
            f.write(chunk)
    os.replace(tmp, path)


def check(item, path):
    data_hash = hashlib.sha256()
    blob = hashlib.sha1(b'blob %d\0' % os.path.getsize(path))
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 22):
            data_hash.update(chunk); blob.update(chunk)
    if 'sha256' in item and data_hash.hexdigest() != item['sha256']:
        raise SystemExit(f'{path}: sha256 mismatch {data_hash.hexdigest()}')
    if 'git_blob' in item and blob.hexdigest() != item['git_blob']:
        raise SystemExit(f'{path}: git blob mismatch {blob.hexdigest()}')
    return data_hash.hexdigest()


def ventoy(root):
    rel = json.load(urlopen(Request('https://api.github.com/repos/ventoy/Ventoy/releases/latest', headers=UA)))
    a = next(x for x in rel['assets'] if x['name'].endswith('-windows.zip'))
    path = os.path.join(root, 'ventoy', a['name'])
    if not os.path.exists(path):
        fetch(a['browser_download_url'], path)
    got = check({}, path)
    want = (a.get('digest') or '').removeprefix('sha256:')
    if want and got != want:
        raise SystemExit(f'{path}: sha256 {got} != release digest {want}')
    with zipfile.ZipFile(path) as z:
        for m in z.namelist():
            if m.startswith(('/', '\\')) or '..' in m.replace('\\', '/').split('/'):
                raise SystemExit(f'bad path {m}')
        z.extractall(os.path.join(root, 'ventoy', 'x'))
    print(f'ventoy {rel["tag_name"]}: {a["name"]} sha256 {got}')


def main(root, names):
    for name in names or ['amfipass', 'nvrun', 'ventoy']:
        if name == 'ventoy':
            ventoy(root); continue
        item = ITEMS[name]
        path = os.path.join(root, item['file'])
        if not os.path.exists(path):
            fetch(item['url'], path)
        h = check(item, path)
        if 'unzip' in item:
            with zipfile.ZipFile(path) as z:
                z.extractall(os.path.join(root, item['unzip']))
        print(f'{name}: {path} sha256 {h} ok')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
