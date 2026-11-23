"""Release check for the recorder DLL, independent of the build folder."""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
ASSETS = {
    110: "design/poise/fonts/Poise-Light.ttf",
    111: "assets/fonts/poise/Poise-Regular.ttf",
    112: "assets/fonts/poise/Poise-Medium.ttf",
    113: "assets/fonts/poise/OFL.txt",
}


def check_poise(dll):
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    def bind(name, args, result):
        proc = getattr(api, name)
        proc.argtypes, proc.restype = args, result
        return proc
    pointer = ctypes.c_void_p
    load = bind("LoadLibraryExW", [wintypes.LPCWSTR, pointer, wintypes.DWORD], pointer)
    release = bind("FreeLibrary", [pointer], wintypes.BOOL)
    find = bind("FindResourceW", [pointer, pointer, pointer], pointer)
    load_resource = bind("LoadResource", [pointer, pointer], pointer)
    lock = bind("LockResource", [pointer], pointer)
    size = bind("SizeofResource", [pointer, pointer], wintypes.DWORD)
    module = load(str(dll), None, 2)  # data only, no DLL initialization
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for resource_id, path in ASSETS.items():
            entry = find(module, resource_id, 10)  # RT_RCDATA
            data = lock(load_resource(module, entry)) if entry else None
            if not data or ctypes.string_at(data, size(module, entry)) != (ROOT / path).read_bytes():
                raise ValueError(f"Missing or incorrect embedded {Path(path).name} in {dll}")
    finally:
        release(module)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dxgi", type=Path)
    args = parser.parse_args()
    dll = args.dxgi.resolve(strict=True)
    subprocess.run([sys.executable, str(ROOT / "tests/cpu/test_diagnostic_dll_exit.py"), str(dll)], check=True)
    check_poise(dll)
    print("Packaged DLL started scene discovery and contains all three Poise faces and their license.")
