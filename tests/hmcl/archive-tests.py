"""Exercise the native archive recognizer with real ZIP directories and malformed inputs."""
import io
from pathlib import Path
import subprocess
import sys
import uuid
import zipfile

probe = Path(sys.argv[1]).resolve()
out = Path(sys.argv[2]) / uuid.uuid4().hex
out.mkdir(parents=True)
launcher = 'org/jackhuang/hmcl/Launcher.class'
region = 'org/jackhuang/hmcl/util/RegionUtils.class'

def archive(names, comment=b''):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', compression=zipfile.ZIP_DEFLATED) as jar:
        for name in names:
            jar.writestr(name, b'fixture content; bytecode is checked separately by the JVM tests')
        jar.comment = comment
    return buffer.getvalue()

valid = archive([launcher, region])
cases = [('jar', valid, True), ('appended-exe', b'MZ' + bytes(4094) + valid, True),
         ('zip-comment', archive([launcher, region], b'build metadata'), True),
         ('missing-region', archive([launcher]), False), ('missing-launcher', archive([region]), False),
         ('lookalike-entry', archive([launcher, region + '.txt']), False),
         ('truncated-directory', valid[:-12], False), ('not-zip', b'MZ' + bytes(500), False),
         ('marker-text-only', (launcher + region).encode(), False)]
for name, data, expected in cases:
    file = out / (name + '.exe')
    file.write_bytes(data)
    result = subprocess.run([str(probe), str(file), '1' if expected else '0'], timeout=10)
    assert result.returncode == 0, name
    print('PASS archive-' + name)
