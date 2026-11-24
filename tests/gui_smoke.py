from pathlib import Path
import ctypes
from ctypes import wintypes
import subprocess
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / "tests" / ".gui-work"
WORK.mkdir(parents=True, exist_ok=True)
source = WORK / "disguised.mp4"
output = WORK / "disguised_已修复.zip"
for path in (source, output):
    if path.exists():
        path.unlink()
with zipfile.ZipFile(WORK / "payload.zip", "w", zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("目录/测试.txt", "GUI batch conversion")
source.write_bytes(b"\x00\x00\x00\x18ftypisom" + b"VIDEO" * 31 + (WORK / "payload.zip").read_bytes())

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
user32.FindWindowW.restype = wintypes.HWND
user32.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
user32.GetDlgItem.restype = wintypes.HWND
user32.IsWindowEnabled.argtypes = [wintypes.HWND]
user32.IsWindowEnabled.restype = wintypes.BOOL
user32.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.SendMessageW.restype = wintypes.LPARAM
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]

def control_text(window, control_id):
    control = user32.GetDlgItem(window, control_id)
    length = user32.GetWindowTextLengthW(control)
    buffer = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(control, buffer, length + 1)
    return buffer.value

from screen_util import launch as launch_secondary

process, window = launch_secondary(
    ROOT / "dist" / "ZipRepairDesk.exe", [source],
    ready=lambda w: user32.IsWindowEnabled(user32.GetDlgItem(w, 1011)))
try:
    start = user32.GetDlgItem(window, 1011)

    user32.SendMessageW(start, 0x00F5, 0, 0)  # BM_CLICK
    deadline = time.time() + 15
    while time.time() < deadline and not output.exists():
        time.sleep(0.1)
    status = control_text(window, 1009)
    log = control_text(window, 1010)
    (ROOT / ".tmp" / "gui_test_status.log").write_text(status + "\n" + log, encoding="utf-8")
    if not output.exists():
        raise RuntimeError("GUI produced no output: " + status + "\n" + log)
    with zipfile.ZipFile(output) as archive:
        assert archive.read("目录/测试.txt").decode("utf-8") == "GUI batch conversion"
        assert archive.testzip() is None
    print("PASS: GUI argument input / inspect / batch click / output / CRC")
    print("STATUS:", status)
finally:
    if window:
        user32.PostMessageW(window, 0x0010, 0, 0)  # WM_CLOSE
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
