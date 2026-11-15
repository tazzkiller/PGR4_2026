# The game's executable image as loaded (disc default.xex with Title Update 2 applied), read from
# the running pgr4.exe: the guest's virtual memory starts at the first free 2^n from 2^32
# (rexglue's Memory::Initialize, usually 0x100000000), the image at guest 0x82000000.
#   py dump_image.py <out.bin> [size]
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys

out = sys.argv[1]
size = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0xC00000
pid = int(subprocess.check_output(
    ['powershell', '-NoProfile', '-Command', '(Get-Process pgr4).Id']).split()[0])
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
h = k32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
buf = ctypes.create_string_buffer(size)
for n in range(32, 40):
    base = (1 << n) + 0x82000000
    got = ctypes.c_size_t()
    if k32.ReadProcessMemory(h, ctypes.c_void_p(base), buf, size, ctypes.byref(got)) and got.value:
        data = buf.raw[:got.value]
        if data[:2] == b'MZ' or b'.stream2' in data:
            open(out, 'wb').write(data)
            print(f'guest base 2^{n}: {got.value} bytes')
            break
else:
    print('not found', ctypes.get_last_error())
