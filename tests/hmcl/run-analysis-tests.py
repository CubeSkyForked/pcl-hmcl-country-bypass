"""Offline JVM tests for semantic selection, changed layouts and late retransformation."""
import argparse
import base64
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import uuid
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--java-home', default='')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
fixtures = root / 'tests/hmcl/analysis'
out = root / 'build' / ('analysis-tests-' + uuid.uuid4().hex)
out.mkdir(parents=True)
java = str(Path(args.java_home) / 'bin/java.exe') if args.java_home else 'java'
javac = str(Path(args.java_home) / 'bin/javac.exe') if args.java_home else 'javac'
agent = root / 'build/hmcl-agent/HKRegion.HMCL.Agent.jar'
creation = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0

def run(command):
    result = subprocess.run([str(value) for value in command], text=True, encoding='utf-8', errors='replace',
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, creationflags=creation)
    if result.returncode:
        raise AssertionError(result.stdout)
    return result.stdout

def option(log):
    return 'log64:' + base64.b64encode(str(log).encode('utf-8')).decode('ascii')

results = []
for layout in ('classic', 'renamed', 'ambiguous', 'unsupported'):
    case = out / layout
    classes = case / 'classes'
    classes.mkdir(parents=True)
    sources = list((fixtures / 'common').rglob('*.java'))
    if layout == 'unsupported':
        sources += list((root / 'tests/hmcl/unsupported').rglob('*.java'))
        sources += [root / 'tests/hmcl/UnsupportedMain.java']
        main = 'UnsupportedMain'
    elif layout == 'ambiguous':
        sources += list((fixtures / layout).rglob('*.java')) + [root / 'tests/hmcl/UnsupportedMain.java']
        main = 'UnsupportedMain'
    else:
        sources += list((fixtures / layout).rglob('*.java')) + [fixtures / 'AnalysisMain.java']
        main = 'AnalysisMain'
    run([javac, '--release', '17', '-encoding', 'UTF-8', '-g', '-d', classes, *sources])
    jar = case / 'fixture.jar'
    with zipfile.ZipFile(jar, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr('META-INF/MANIFEST.MF', 'Manifest-Version: 1.0\r\nMain-Class: ' + main + '\r\n\r\n')
        for file in sorted(classes.rglob('*.class')):
            archive.write(file, file.relative_to(classes).as_posix())
    log = case / 'startup.log'
    command = [java, '-Xverify:all', '-Duser.timezone=Asia/Shanghai', '-Duser.country=CN',
               '-javaagent:' + str(agent) + '=' + option(log), '-jar', jar]
    output = run(command)
    log_text = log.read_text(encoding='utf-8')
    expected = 'TRANSFORMED:' if layout in ('classic', 'renamed') else 'REJECTED:'
    assert expected in log_text, log_text
    results.append({'case': layout + '-startup', 'passed': True, 'output': output.strip()})
    if layout not in ('classic', 'renamed'):
        continue
    run([javac, '--add-modules', 'jdk.attach', '-d', classes, fixtures / 'Attach.java'])
    process = subprocess.Popen([java, '-Xverify:all', '-XX:+EnableDynamicAgentLoading', '-Duser.timezone=Asia/Shanghai',
                                '-Duser.country=CN', '-cp', str(jar), main, 'late'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding='utf-8', errors='replace', creationflags=creation)
    lines = queue.Queue()
    def wait_ready():
        lines.put(process.stdout.readline())
    threading.Thread(target=wait_ready, daemon=True).start()
    try:
        ready = lines.get(timeout=15)
        if ready.strip() != 'READY':
            raise AssertionError('Late baseline failed: ' + ready)
        late_log = case / 'late.log'
        run([java, '--add-modules', 'jdk.attach', '-cp', classes, 'Attach', process.pid, agent, option(late_log)])
        output, _ = process.communicate('\n', timeout=15)
        assert process.returncode == 0, output
        assert 'retransform=true' in late_log.read_text(encoding='utf-8')
        results.append({'case': layout + '-late', 'passed': True, 'output': output.strip()})
    finally:
        if process.poll() is None:
            process.terminate()
            process.communicate(timeout=10)

(root / 'build/analysis-tests.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
for result in results:
    print('PASS ' + result['case'])
