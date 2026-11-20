"""Exercise Windows DLL process-detach with an active diagnostic worker."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid


def trigger_deferred_start(dll, status):
    def check_idle():
        assert dll.AbundanceQueryProxyStatus(status) and status[5] == 1, status[5]

    factory = dll.CreateDXGIFactory1
    factory.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    factory.restype = ctypes.c_long
    result = ctypes.c_void_p()
    time.sleep(0.25)
    check_idle()
    invalid_iid = (ctypes.c_byte * 16)()
    assert factory(ctypes.byref(invalid_iid), ctypes.byref(result)) < 0 and not result.value
    check_idle()
    iid = (ctypes.c_byte * 16).from_buffer_copy(uuid.UUID("770aae78-f26f-4dba-a829-253c83d1b387").bytes_le)
    assert factory(ctypes.byref(iid), ctypes.byref(result)) >= 0 and result.value
    vtable = ctypes.cast(result, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[2])
    release(result)


if sys.argv[1] == "--child":
    ctypes.windll.kernel32.SetErrorMode(3)
    dll = ctypes.CDLL(sys.argv[2])
    status = (ctypes.c_uint32 * 6)()
    status[0] = ctypes.sizeof(status)
    if "--deferred" in sys.argv:
        trigger_deferred_start(dll, status)
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if dll.AbundanceQueryProxyStatus(status) and status[5] & 8:
            # Windows terminates other threads before invoking DLL teardown.
            ctypes.windll.kernel32.ExitProcess(0)
        time.sleep(0.05)
    raise SystemExit("diagnostic adapter did not start")

with tempfile.TemporaryDirectory(prefix="na-dll-exit-") as directory:
    root = Path(directory)
    dll_path = root / "dxgi.dll"
    shutil.copyfile(sys.argv[1], dll_path)
    for deferred in (False, True):
        (root / "NeuralAliasHost.ini").write_text(
            f"[host]\nenabled=1\nstart_after_factory={int(deferred)}\n"
            "[route_a]\nenabled=0\n[fsr]\nenabled=0\n"
            "[xess]\nenabled=0\n[adapter]\nenabled=1\n[observe]\nproducers=1\n"
            f"[discovery]\nenabled=1\nread_only=1\nlog={root / 'capture.jsonl'}\n",
            encoding="ascii",
        )
        cmd = [sys.executable, __file__, "--child", str(dll_path)]
        if deferred:
            cmd.append("--deferred")
        result = subprocess.run(cmd, timeout=11)
        mode = "deferred" if deferred else "eager"
        assert result.returncode == 0, f"DLL {mode} startup or teardown failed: {result.returncode:#x}"
        print(f"active diagnostic DLL ({mode} start) exited without fail-fast")
