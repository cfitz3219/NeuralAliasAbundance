"""Exercise Windows DLL process-detach with an active diagnostic worker."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

if len(sys.argv) == 3 and sys.argv[1] == "--child":
    ctypes.windll.kernel32.SetErrorMode(3)
    dll = ctypes.CDLL(sys.argv[2])
    status = (ctypes.c_uint32 * 6)()
    status[0] = ctypes.sizeof(status)
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if dll.AbundanceQueryProxyStatus(status) and status[5] & 8:
            # Windows terminates other threads before invoking DLL teardown.
            ctypes.windll.kernel32.ExitProcess(0)
        time.sleep(0.05)
    raise SystemExit("diagnostic adapter did not start")

with tempfile.TemporaryDirectory(prefix="na-dll-exit-") as directory:
    root = Path(directory)
    dll_path = root / "NeuralAliasDXGI.dll"
    shutil.copyfile(sys.argv[1], dll_path)
    (root / "NeuralAliasHost.ini").write_text(
        "[host]\nenabled=1\n[route_a]\nenabled=0\n[fsr]\nenabled=0\n"
        "[xess]\nenabled=0\n[adapter]\nenabled=1\n[observe]\nproducers=1\n"
        f"[discovery]\nenabled=1\nread_only=1\nlog={root / 'capture.jsonl'}\n",
        encoding="ascii",
    )
    result = subprocess.run([sys.executable, __file__, "--child", str(dll_path)], timeout=15)
    assert result.returncode == 0, f"DLL process teardown failed: {result.returncode:#x}"
    print("active diagnostic DLL exited without fail-fast")
