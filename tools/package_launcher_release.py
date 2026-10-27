"""Create the GitHub Releases assets consumed by SA:COOP Launcher (no credentials)."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import shutil
import zipfile
import ctypes
import os

ROOT = Path(__file__).resolve().parent.parent

def binary_version(path):
    if os.name != 'nt': raise SystemExit('Package native Windows releases on Windows.')
    api = ctypes.WinDLL('version', use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [ctypes.c_wchar_p,ctypes.c_void_p]
    api.GetFileVersionInfoW.argtypes = [ctypes.c_wchar_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_void_p]
    api.VerQueryValueW.argtypes = [ctypes.c_void_p,ctypes.c_wchar_p,ctypes.POINTER(ctypes.c_void_p),ctypes.POINTER(ctypes.c_uint)]
    size=api.GetFileVersionInfoSizeW(str(path),None)
    if not size: raise SystemExit(f'No version resource: {path}')
    data=ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(path),0,size,data): raise SystemExit(f'Cannot read version: {path}')
    value=ctypes.c_void_p(); length=ctypes.c_uint()
    if not api.VerQueryValueW(data,'\\StringFileInfo\\040904b0\\ProductVersion',ctypes.byref(value),ctypes.byref(length)):
        raise SystemExit(f'No product version: {path}')
    return ctypes.wstring_at(value,length.value).rstrip('\0')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tag', help='Optional CI tag; must match the native mod version')
    parser.add_argument('--launcher', type=Path, default=ROOT/'build/launcher/win-x64/SA-COOP Launcher.exe')
    parser.add_argument('--output', type=Path, default=ROOT/'build/github-release')
    args = parser.parse_args()
    version_header = (ROOT/'src/common/config/version/Version.h').read_text(encoding='utf-8')
    version = re.search(r'COOP_VERSION_STR "([^"]+)"', version_header)[1]
    protocol = int(re.search(r'kProtocolVersion = (\d+)', version_header)[1])
    if args.tag and args.tag.removeprefix('v') != version:
        raise SystemExit(f'Tag {args.tag} does not match mod version {version}')
    args.output.mkdir(parents=True, exist_ok=True)
    binaries = {'coop.asi': ROOT/'build/release/bin/coop.dll',
                'coop/coop-server.exe': ROOT/'build/release/bin/coop-server.exe',
                'coop/coop.pdb': ROOT/'build/release/bin/coop.pdb'}
    for path in (binaries['coop.asi'],binaries['coop/coop-server.exe']):
        if binary_version(path)!=version: raise SystemExit(f'{path.name} is stale; rebuild native binaries first.')
    entries = {name: path.read_bytes() for name, path in binaries.items()}
    files = [{'path': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()} for name,data in entries.items()]
    entries['SHA256SUMS.txt'] = ''.join(f'{f["sha256"]}  {f["path"]}\n' for f in files).encode()
    for path in sorted((ROOT/'dist/SA-COOP/coop/licenses').glob('*.txt')):
        entries['coop/licenses/'+path.name] = path.read_bytes()
    entries['docs/TAG-MINIGAME.md'] = (ROOT/'docs/TAG-MINIGAME.md').read_bytes()
    archive = args.output/f'SA-COOP-{version}-update.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED) as z:
        for name,data in entries.items(): z.writestr(name,data)
    notes = ['Догонялки и автоматическая смена ролей', 'Временная максимальная выносливость', 'Передача оружия, патронов и помощь другу']
    manifest = {'schema': 1, 'version': version, 'protocol': protocol, 'minimumLauncherVersion': '1.0.1',
                'package': {'name': archive.name, 'size': archive.stat().st_size,
                            'sha256': hashlib.sha256(archive.read_bytes()).hexdigest()},
                'files': files, 'notes': notes}
    (args.output/'sa-coop-update.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    if args.launcher.is_file():
        with zipfile.ZipFile(args.output/'SA-COOP-Launcher-win-x64.zip','w',zipfile.ZIP_DEFLATED) as z:
            z.write(args.launcher,'SA-COOP Launcher.exe')
            z.write(ROOT/'launcher/README-RU.md','ЛАУНЧЕР-ИНСТРУКЦИЯ.md')
            z.write(ROOT/'launcher/DESIGN.md','DESIGN.md')
            z.write(ROOT/'LICENSE','LICENSE-SA-COOP.txt')
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for file in files:
            assert hashlib.sha256(z.read(file['path'])).hexdigest()==file['sha256']
    print(f'Release {version}, protocol {protocol}: {args.output}')
    for path in sorted(args.output.iterdir()):
        if path.is_file(): print(f'  {path.name}: {path.stat().st_size} bytes')

if __name__=='__main__': main()
