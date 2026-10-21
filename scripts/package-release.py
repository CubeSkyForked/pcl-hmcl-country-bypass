"""Stage only the two supported delivery names, checking their real PE architecture."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct

root = Path(__file__).resolve().parents[1]
output = root / 'dist'
specs = [('winmm.dll', 'msvc-x64', 0x8664, 'x64'), ('version.dll', 'msvc-Win32', 0x14c, 'x86')]
prepared = []
for name, build, machine, arch in specs:
    source = root / 'build' / build / 'bin' / name
    data = source.read_bytes()
    if len(data) < 64 or data[:2] != b'MZ':
        raise SystemExit(f'{name}: missing DOS/PE header')
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b'PE\0\0':
        raise SystemExit(f'{name}: invalid PE signature')
    actual, = struct.unpack_from('<H', data, pe + 4)
    flags, = struct.unpack_from('<H', data, pe + 22)
    if actual != machine or not flags & 0x2000:
        raise SystemExit(f'{name}: expected {arch} DLL, got machine=0x{actual:04x}, flags=0x{flags:04x}')
    for license_path in (root / 'LICENSE', root / 'third_party/minhook/LICENSE.txt'):
        if license_path.read_bytes() not in data:
            raise SystemExit(f'{name}: embedded license missing: {license_path.name}')
    prepared.append((source, {'name': name, 'architecture': arch, 'machine': f'0x{actual:04x}',
                             'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}))

output.mkdir(exist_ok=True)
unexpected = {p.name for p in output.iterdir()} - {spec[0] for spec in specs}
if unexpected:
    raise SystemExit(f'Refusing to package a directory containing unexpected files: {sorted(unexpected)}')
for source, entry in prepared:
    shutil.copyfile(source, output / entry['name'])
manifest = {'commit': os.environ.get('GITHUB_SHA', ''), 'assets': [entry for _, entry in prepared]}
(root / 'build/release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
notes = ['将两个 DLL 一起放到启动器 EXE 同目录，完全退出启动器后重新启动。', '',
         '- `winmm.dll`：x64，供已验证的 PCL2 / PCL2-CE 加载。',
         '- `version.dll`：x86，供 HMCL 启动壳加载，自动释放 agent 并在目标类加载时分析地区判定。', '',
         'HMCL 按归档结构及 RegionUtils 实际字节码识别，不绑定 EXE/class 哈希；无法明确识别时保留原类并记录原因。',
         '原版 PCL2 的托管恢复仍使用精确 EXE/MVID/IL 校验。',
         '源码许可与 MinHook 许可已嵌入 DLL 的 RCDATA 资源。', '', 'SHA-256：', '', '```text']
notes.extend(entry['sha256'] + '  ' + entry['name'] for _, entry in prepared)
notes.extend(['```', ''])
if manifest['commit']:
    notes.extend(['Source commit: `' + manifest['commit'] + '`', ''])
(root / 'build/release-notes.md').write_text('\n'.join(notes), encoding='utf-8')
print(json.dumps(manifest, indent=2))
