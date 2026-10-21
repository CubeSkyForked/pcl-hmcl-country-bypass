"""Stage the two proxy DLLs and their matching standalone Java agent."""
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import zipfile

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

agent = root / 'build/hmcl-agent/HKRegion.HMCL.Agent.jar'
agent_data = agent.read_bytes()
with zipfile.ZipFile(io.BytesIO(agent_data)) as archive:
    if archive.testzip() is not None:
        raise SystemExit('Java agent: corrupt ZIP entry')
    attributes = dict(line.split(': ', 1) for line in archive.read('META-INF/MANIFEST.MF').decode('utf-8').splitlines() if ': ' in line)
    expected = {'Premain-Class': 'hkregion.hmcl.HmclRegionAgent',
                'Agent-Class': 'hkregion.hmcl.HmclRegionAgent', 'Can-Retransform-Classes': 'true'}
    if any(attributes.get(key) != value for key, value in expected.items()):
        raise SystemExit('Java agent: invalid instrumentation manifest')
    for name in ('hkregion/hmcl/HmclRegionAgent.class', 'hkregion/hmcl/HmclClassAnalyzer.class'):
        data = archive.read(name)
        if len(data) < 8 or data[:4] != b'\xca\xfe\xba\xbe' or struct.unpack_from('>H', data, 6)[0] > 61:
            raise SystemExit(f'Java agent: invalid or newer-than-Java-17 class: {name}')
    if archive.read('META-INF/LICENSE') != (root / 'LICENSE').read_bytes():
        raise SystemExit('Java agent: project license missing or stale')
for source, entry in prepared:
    if agent_data not in source.read_bytes():
        raise SystemExit(f"{entry['name']}: embedded agent differs from standalone JAR; rebuild both DLLs")
prepared.append((agent, {'name': agent.name, 'architecture': 'any', 'javaRelease': 17,
                         'bytes': len(agent_data), 'sha256': hashlib.sha256(agent_data).hexdigest()}))

output.mkdir(exist_ok=True)
unexpected = {p.name for p in output.iterdir()} - {entry['name'] for _, entry in prepared}
if unexpected:
    raise SystemExit(f'Refusing to package a directory containing unexpected files: {sorted(unexpected)}')
for source, entry in prepared:
    shutil.copyfile(source, output / entry['name'])
manifest = {'commit': os.environ.get('GITHUB_SHA', ''), 'assets': [entry for _, entry in prepared]}
(root / 'build/release-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
notes = ['将两个 DLL 一起放到启动器 EXE 同目录，完全退出启动器后重新启动。', '',
         '- `winmm.dll`：x64，供已验证的 PCL2 / PCL2-CE 加载。',
         '- `version.dll`：x86，供 HMCL 启动壳加载，自动释放 agent 并在目标类加载时分析地区判定。',
         '- `HKRegion.HMCL.Agent.jar`：可选的独立 Java agent，供高级用户直接加载；Java 17+，与 JVM 位数无关。', '',
         '独立使用 agent 时无需 DLL，将 `-javaagent` 放在 HMCL 的 `-jar` 参数之前：', '',
         '```powershell', 'java "-javaagent:.\\HKRegion.HMCL.Agent.jar" -jar ".\\HMCL.jar"', '```', '',
         'HMCL 按归档结构及 RegionUtils 实际字节码识别，不绑定 EXE/class 哈希；无法明确识别时保留原类并记录原因。',
         '原版 PCL2 的托管恢复仍使用精确 EXE/MVID/IL 校验。',
         '源码许可与 MinHook 许可已嵌入 DLL 的 RCDATA 资源；独立 JAR 包含项目 MIT 许可证。', '', 'SHA-256：', '', '```text']
notes.extend(entry['sha256'] + '  ' + entry['name'] for _, entry in prepared)
notes.extend(['```', ''])
if manifest['commit']:
    notes.extend(['Source commit: `' + manifest['commit'] + '`', ''])
(root / 'build/release-notes.md').write_text('\n'.join(notes), encoding='utf-8')
print(json.dumps(manifest, indent=2))
