"""Validate the payload extraction ABI, Unicode paths, nonce and corruption handling."""
import ctypes
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import sys
import uuid
import zipfile

proxy_path = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
directory = output / ('agent-' + uuid.uuid4().hex + '-中文 空格')
directory.mkdir(parents=True)
proxy = ctypes.WinDLL(str(proxy_path), use_last_error=True)
extract = proxy.HKRegion_ExtractHmclAgentW
extract.argtypes = [w.HWND, w.HINSTANCE, w.LPWSTR, ctypes.c_int]
extract.restype = None

def call(request):
    command = ctypes.create_unicode_buffer(f'"{directory}" {request}')
    extract(None, None, command, 0)
    return ctypes.get_last_error()

checks = []
def expect(name, result):
    checks.append({'name': name, 'passed': bool(result)})
    print(('PASS ' if result else 'FAIL ') + name)

request = str(uuid.uuid4())
expect('Extractor succeeds with Unicode and spaces', call(request) == 0)
manifest_path = directory / 'hmcl-agent.json'
manifest = json.loads(manifest_path.read_text())
expect('Fresh request nonce is returned', manifest['requestId'] == request)
jar = directory / manifest['file']
expect('Extracted JAR matches its SHA256', hashlib.sha256(jar.read_bytes()).hexdigest() == manifest['sha256'])
with zipfile.ZipFile(jar) as archive:
    data = archive.read('META-INF/MANIFEST.MF')
    expect('JAR declares startup, Attach and retransformation entries', all(part in data for part in
        [b'Premain-Class: hkregion.hmcl.HmclRegionAgent', b'Agent-Class: hkregion.hmcl.HmclRegionAgent',
         b'Can-Retransform-Classes: true']))
request2 = str(uuid.uuid4())
expect('Existing verified JAR is reused', call(request2) == 0 and json.loads(manifest_path.read_text())['requestId'] == request2)
before = manifest_path.read_bytes()
expect('Invalid nonce is rejected', call('invalid/request') == 87 and manifest_path.read_bytes() == before)
jar.write_bytes(b'corrupt existing artifact')
expect('Corrupt JAR does not publish a fresh manifest', call(str(uuid.uuid4())) != 0 and manifest_path.read_bytes() == before)
report = {'profile': proxy_path.stem, 'proxy_sha256': hashlib.sha256(proxy_path.read_bytes()).hexdigest(),
          'checks': checks, 'passed': all(c['passed'] for c in checks)}
output.mkdir(parents=True, exist_ok=True)
(output / f'{proxy_path.stem}-extraction.json').write_text(json.dumps(report, indent=2) + '\n')
sys.exit(0 if report['passed'] else 1)
