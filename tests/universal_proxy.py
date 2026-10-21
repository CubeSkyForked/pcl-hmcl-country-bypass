"""Exercise real Windows APIs through each renamed copy in a fresh process."""
import ctypes as c
from ctypes import wintypes as w
import json
import hashlib
import sys
import threading
import time
from pathlib import Path

path = Path(sys.argv[1]).resolve()
kind = path.stem.lower()
proxy = c.WinDLL(str(path), use_last_error=True)
system = c.WinDLL(str(Path('C:/Windows/System32') / path.name), use_last_error=True)
kernel = c.WinDLL('kernel32', use_last_error=True)
checks = []

def expect(name, passed):
    checks.append({'name': name, 'passed': bool(passed)})
    print(('PASS ' if passed else 'FAIL ') + name, flush=True)

def bind(module, name, result, arguments):
    function = getattr(module, name)
    function.restype, function.argtypes = result, arguments
    return function

def native_ordinal(module, name):
    # System ordinals can differ across Windows builds. Use this loaded image's
    # export table, rather than treating the 26200 name snapshot as the CI OS.
    base = module._handle
    u32 = lambda offset: c.c_uint32.from_address(base + offset).value
    u16 = lambda offset: c.c_uint16.from_address(base + offset).value
    optional = u32(0x3c) + 24
    directory = u32(optional + (112 if u16(optional) == 0x20b else 96))
    ordinal_base, count = u32(directory + 16), u32(directory + 24)
    names, ordinals = u32(directory + 32), u32(directory + 36)
    for index in range(count):
        if c.string_at(base + u32(names + 4 * index)).decode('ascii') == name:
            return ordinal_base + u16(ordinals + 2 * index)
    raise AssertionError('System export not found: ' + name)

wait = bind(proxy, 'HKRegion_WaitForInitialization', w.DWORD, [w.DWORD])
expect('Automatic hook initialization', wait(15000) == 0)
managed_status = bind(proxy, 'HKRegion_GetManagedPatchStatus', w.DWORD, [])
deadline = time.monotonic() + 2
while managed_status() == 997 and time.monotonic() < deadline:
    time.sleep(0.01)
expect('Managed restoration rejects unsupported executable', managed_status() == 50)
c.set_last_error(0x51234567)
managed_status()
expect('Managed status preserves LastError', c.get_last_error() == 0x51234567)
module_handle = bind(kernel, 'GetModuleHandleW', w.HMODULE, [w.LPCWSTR])
expect('Unsupported process leaves Framework CLR unloaded', not module_handle('clr.dll'))
geo = bind(kernel, 'GetUserDefaultGeoName', c.c_int, [w.LPWSTR, c.c_int])
buffer = c.create_unicode_buffer(16)
expect('GeoName HK after automatic initialization', geo(buffer, 16) == 3 and buffer.value == 'HK')
get_proc = bind(kernel, 'GetProcAddress', c.c_void_p, [w.HMODULE, c.c_void_p])
manifest = json.loads((Path(__file__).resolve().parent.parent / 'src/proxy_exports.json').read_text())
entries = manifest['libraries'][kind]['exports']
expect('All native named exports are present', all(hasattr(proxy, entry['name']) for entry in entries if entry['name']))
create_thread = bind(kernel, 'CreateThread', w.HANDLE,
                     [c.c_void_p, c.c_size_t, c.c_void_p, c.c_void_p, w.DWORD, c.c_void_p])
wait_thread = bind(kernel, 'WaitForSingleObject', w.DWORD, [w.HANDLE, w.DWORD])
close_thread = bind(kernel, 'CloseHandle', w.BOOL, [w.HANDLE])
# A native one-argument routine avoids Python callback stack requirements.
# GetModuleHandleW(NULL) is harmless; the thread's return value is unused.
start = c.cast(kernel.GetModuleHandleW, c.c_void_p)
thread = create_thread(None, 65536, start, None, 0x10000, None)
expect('Thread attach works with a 64 KiB reserved stack', thread and wait_thread(thread, 5000) == 0)
if thread:
    close_thread(thread)

if kind == 'version':
    file = 'C:/Windows/System32/kernel32.dll'
    size_args = [w.LPCWSTR, c.POINTER(w.DWORD)]
    real_size = bind(system, 'GetFileVersionInfoSizeW', w.DWORD, size_args)
    proxy_size = bind(proxy, 'GetFileVersionInfoSizeW', w.DWORD, size_args)
    handle = w.DWORD()
    size = real_size(file, c.byref(handle))
    expect('Version resource size is forwarded', size > 0 and proxy_size(file, c.byref(handle)) == size)
    info_args = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p]
    original, forwarded = c.create_string_buffer(size), c.create_string_buffer(size)
    real_info = bind(system, 'GetFileVersionInfoW', w.BOOL, info_args)
    proxy_info = bind(proxy, 'GetFileVersionInfoW', w.BOOL, info_args)
    expect('Version resource bytes match Windows', real_info(file, 0, size, original)
           and proxy_info(file, 0, size, forwarded) and original.raw == forwarded.raw)
    by_ordinal = c.WINFUNCTYPE(w.DWORD, *size_args)(get_proc(proxy._handle, 8))
    expect('Version ordinal 8 dispatches correctly', by_ordinal(file, c.byref(handle)) == size)
    expect('Version names retain original ordinals', all(
        c.cast(getattr(proxy, e['name']), c.c_void_p).value == get_proc(proxy._handle, e['ordinal']) for e in entries))
    # The first lazy resolution must not leak GetProcAddress's LastError.
    language = bind(proxy, 'VerLanguageNameW', w.DWORD, [w.DWORD, w.LPWSTR, w.DWORD])
    native_language = bind(system, 'VerLanguageNameW', w.DWORD, [w.DWORD, w.LPWSTR, w.DWORD])
    a, b = c.create_unicode_buffer(128), c.create_unicode_buffer(128)
    # Warm Windows' NLS cache before comparing unspecified successful-call
    # LastError behavior; its first call can perform additional native loads.
    native_language(0x0409, a, 128)
    c.set_last_error(0x51234567)
    native_result = native_language(0x0409, a, 128)
    native_error = c.get_last_error()
    c.set_last_error(0x51234567)
    proxy_result = language(0x0409, b, 128)
    proxy_error = c.get_last_error()
    print(f'LastError comparison: native={native_error:#x}, proxy={proxy_error:#x}; results={native_result}/{proxy_result}', flush=True)
    expect('Forwarding retains LastError semantics', native_result == proxy_result and a.value == b.value and proxy_error == native_error)
elif kind == 'winhttp':
    open_args = [w.LPCWSTR, w.DWORD, w.LPCWSTR, w.LPCWSTR, w.DWORD]
    http_open = bind(proxy, 'WinHttpOpen', c.c_void_p, open_args)
    close = bind(proxy, 'WinHttpCloseHandle', w.BOOL, [c.c_void_p])
    session = http_open('PCL universal proxy test', 1, None, None, 0)
    expect('WinHttpOpen forwards five arguments', session)
    connect = bind(proxy, 'WinHttpConnect', c.c_void_p, [c.c_void_p, w.LPCWSTR, w.WORD, w.DWORD])
    connection = connect(session, 'localhost', 80, 0)
    expect('WinHttpConnect returns a native handle', connection)
    request_open = bind(proxy, 'WinHttpOpenRequest', c.c_void_p,
                        [c.c_void_p, w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD])
    request = request_open(connection, 'GET', '/proxy-test', None, None, None, 0)
    expect('WinHttpOpenRequest forwards seven arguments', request)
    timeout = bind(proxy, 'WinHttpSetTimeouts', w.BOOL, [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_int])
    expect('WinHttpSetTimeouts forwards stack arguments', timeout(session, 1000, 1000, 1000, 1000))
    set_option = bind(proxy, 'WinHttpSetOption', w.BOOL, [c.c_void_p, w.DWORD, c.c_void_p, w.DWORD])
    query_option = bind(proxy, 'WinHttpQueryOption', w.BOOL, [c.c_void_p, w.DWORD, c.c_void_p, c.POINTER(w.DWORD)])
    context, observed = c.c_size_t(0x123456789ABCDEF if c.sizeof(c.c_void_p) == 8 else 0x12345678), c.c_size_t()
    length = w.DWORD(c.sizeof(context))
    expect('Pointer-sized WinHTTP context round-trips', set_option(request, 45, c.byref(context), length.value)
           and query_option(request, 45, c.byref(observed), c.byref(length)) and context.value == observed.value)
    ordinal = native_ordinal(system, 'WinHttpOpen')
    ordinal_open = c.WINFUNCTYPE(c.c_void_p, *open_args)(get_proc(proxy._handle, ordinal))
    ordinal_session = ordinal_open('Ordinal test', 1, None, None, 0)
    expect('WinHTTP native ordinal selects WinHTTP, not version', ordinal_session)
    for handle in [ordinal_session, request, connection, session]:
        expect('Native WinHTTP handle closes', bool(handle) and close(handle))
    real_close = bind(system, 'WinHttpCloseHandle', w.BOOL, [c.c_void_p])
    c.set_last_error(0x51234567)
    real_result = real_close(None)
    real_error = c.get_last_error()
    c.set_last_error(0x51234567)
    proxy_result = close(None)
    expect('WinHTTP error matches native API', real_result == proxy_result and c.get_last_error() == real_error)
elif kind == 'winmm':
    get_time = bind(proxy, 'timeGetTime', w.DWORD, [])
    real_time = bind(system, 'timeGetTime', w.DWORD, [])
    expect('WinMM clock forwards', abs(get_time() - real_time()) < 100)
    callback_type = c.WINFUNCTYPE(None, w.UINT, w.UINT, c.c_size_t, c.c_size_t, c.c_size_t)
    completed = threading.Event()
    seen = []
    def tick(timer, message, user, first, second):
        seen.append(user)
        completed.set()
    callback = callback_type(tick)
    timer = bind(proxy, 'timeSetEvent', w.UINT, [w.UINT, w.UINT, callback_type, c.c_size_t, w.UINT])
    timer_id = timer(30, 0, callback, 0x12345678, 0)
    expect('WinMM timer callback and stack arguments', timer_id and completed.wait(2) and seen == [0x12345678])
    ordinal = native_ordinal(system, 'timeGetTime')
    ordinal_time = c.WINFUNCTYPE(w.DWORD)(get_proc(proxy._handle, ordinal))
    expect('WinMM native ordinal dispatches correctly', abs(ordinal_time() - real_time()) < 100)
else:
    raise ValueError(kind)

report = {'kind': kind, 'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
          'checks': checks, 'passed': all(i['passed'] for i in checks)}
if len(sys.argv) > 2:
    Path(sys.argv[2]).write_text(json.dumps(report, indent=2) + '\n')
sys.exit(0 if report['passed'] else 1)
