"""Package the installed mods, never the base GTA, private profiles or development files."""
import argparse
import hashlib
import json
from pathlib import Path
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = {
    'bass.dll', 'CLEO.asi', 'CrashInfo.ini', 'CrashInfo.SA.asi', 'dinput8.dll',
    'FramerateVigilante.ini', 'FramerateVigilante.SA.asi', 'HeliFixSA.asi',
    'IndieVehSmoke.asi', 'libcurl.dll', 'modloader.asi', 'SilentPatchSA.asi',
    'SilentPatchSA.ini', 'spreadfix.asi', 'stream.ini', 'vorbis.dll',
    'vorbisFile.dll', 'vorbisHooked.dll', 'zlib1.dll', '_noDEP.asi',
    'Essentials (Readme - EN).txt', 'Essentials (Leiame - PT).txt',
}
DIRECTORIES = ('modloader', 'CLEO', 'scripts', 'cleo_readme', '(ESSENTIALS README)', 'EffectLoader', 'source')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def excluded(relative):
    parts = relative.as_posix().lower().split('/')
    return (relative.suffix.lower() in {'.log', '.sav', '.dmp', '.b'} or
            any(p in {'cleo_saves', '.profiles', 'cache', '.cache', 'logs', 'backups'} for p in parts) or
            relative.name.lower() in {'log.txt', 'crashlist.txt', 'crash.log'})


def collect(game):
    entries = {}
    for name in sorted(ROOT_FILES):
        path = game / name
        if path.is_file():
            entries[name] = path
    for name in DIRECTORIES:
        directory = game / name
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob('*')):
            if path.is_symlink() or path.is_junction():
                raise SystemExit(f'Linked paths are not supported: {path}')
            if path.is_file():
                relative = path.relative_to(game)
                if not excluded(relative):
                    entries[relative.as_posix()] = path
    entries['coop/cutscenes.txt'] = ROOT / 'package/coop/cutscenes.txt'
    entries['coop/licenses/SA-COOP.txt'] = ROOT / 'LICENSE'
    entries['coop/licenses/ENet.txt'] = ROOT / 'third_party/enet/LICENSE'
    entries['coop/licenses/MinHook.txt'] = ROOT / 'third_party/minhook/LICENSE.txt'
    for name in ('FramerateVigilante.txt', 'SilentPatch.txt'):
        path = game / 'coop/patches/60fps-friend-pack/Licenses' / name
        if path.is_file():
            entries['coop/licenses/' + name] = path
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/installation-release')
    parser.add_argument('--version', default='1.0.0')
    args = parser.parse_args()
    game = args.game.resolve(strict=True)
    if not (game / 'gta_sa.exe').is_file():
        raise SystemExit('Select the installed game directory.')
    args.output.mkdir(parents=True, exist_ok=True)
    entries = collect(game)
    files = []
    archive_path = args.output / f'SA-COOP-Modpack-{args.version}.zip'
    if archive_path.exists():
        raise SystemExit('Package already exists; use a new output directory/version.')
    print(f'Packaging {len(entries)} mod files; base GTA and all private coop data excluded.', flush=True)
    with zipfile.ZipFile(archive_path, 'x', zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
        for index, (name, path) in enumerate(sorted(entries.items())):
            if path.is_symlink() or path.is_junction():
                raise SystemExit(f'Linked path: {path}')
            before = (path.stat().st_size, path.stat().st_mtime_ns)
            sha256 = digest(path)
            archive.write(path, name)
            if before != (path.stat().st_size, path.stat().st_mtime_ns):
                raise SystemExit(f'File changed while packaging: {name}; close the game and retry.')
            files.append({'path': name, 'size': before[0], 'sha256': sha256})
            if index % 150 == 0:
                print(f'  {index + 1}/{len(entries)}: {name}', flush=True)
    if archive_path.stat().st_size >= 2 * 1024**3:
        raise SystemExit('Package exceeds the per-asset limit. Split this pack before publishing.')
    manifest = {'schema': 1, 'version': args.version, 'minimumLauncherVersion': '1.1.0',
                'package': {'name': archive_path.name, 'size': archive_path.stat().st_size, 'sha256': digest(archive_path)},
                'files': files,
                'mods': sorted(p.name for p in (game / 'modloader').iterdir() if p.is_dir() and not p.name.startswith('.'))}
    (args.output / 'sa-coop-install.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    notes = ('Пакет модов из текущей сборки MortysCode. Базовая GTA San Andreas не включена.\n\n'
             'Лаунчер 1.1.0 копирует собственную GTA SA 1.0 US пользователя в новую папку, '
             'устанавливает этот пакет и скачивает последнюю стабильную SA:COOP.\n'
             'Сохранения, профили, адреса подключения и пароли владельца сборки исключены.\n'
             'Файлы авторов и лицензии включены в архив.\n\n' +
             '\n'.join('- ' + mod for mod in manifest['mods']) + '\n')
    (args.output / 'RELEASE-NOTES.md').write_text(notes, encoding='utf-8')
    print(f'PACKAGE OK: {archive_path.stat().st_size} compressed, {sum(f["size"] for f in files)} installed bytes.', flush=True)


if __name__ == '__main__':
    main()
