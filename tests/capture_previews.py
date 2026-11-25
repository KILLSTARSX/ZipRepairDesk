"""Capture clean Chinese and English client-area previews on the secondary display."""
from __future__ import annotations

import ctypes
from ctypes import wintypes
from pathlib import Path
import struct
import time
import winreg
import zipfile

from PIL import ImageGrab

from screen_util import launch

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "dist" / "ZipRepairDesk.exe"
WORK = ROOT / "tests" / ".preview-work"
USER32 = ctypes.WinDLL("user32", use_last_error=True)
USER32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
IDC_LIST = 1007
IDC_THEME = 1014
IDC_LANGUAGE = 1015
LVM_GETITEMCOUNT = 0x1004
BM_CLICK = 0x00F5
REG_PATH = r"Software\KILL-STAR SX\ZipRepairDesk"


def text(window: int) -> str:
    length = USER32.GetWindowTextLengthW(window)
    buffer = ctypes.create_unicode_buffer(length + 1)
    USER32.GetWindowTextW(window, buffer, len(buffer))
    return buffer.value


def click(window: int, control_id: int) -> None:
    control = USER32.GetDlgItem(window, control_id)
    USER32.SendMessageW(control, BM_CLICK, 0, 0)
    time.sleep(0.35)


def select_mode(window: int, english: bool) -> None:
    language = USER32.GetDlgItem(window, IDC_LANGUAGE)
    currently_english = text(language) == "中文"
    if currently_english != english:
        click(window, IDC_LANGUAGE)
    theme = USER32.GetDlgItem(window, IDC_THEME)
    currently_dark = text(theme) in {"Dark", "深色"}
    if not currently_dark:
        click(window, IDC_THEME)
    USER32.InvalidateRect(window, None, True)
    USER32.UpdateWindow(window)
    time.sleep(0.45)


def mp4_prefix() -> bytes:
    def box(kind: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload) + 8) + kind + payload
    return box(b"ftyp", b"isom\0\0\2\0isomiso2") + box(b"mdat", b"\0" * 48)


def samples(english: bool) -> list[Path]:
    WORK.mkdir(parents=True, exist_ok=True)
    names = (("disguised-sample.mp4", "plain-archive.zip", "plain-video.mp4", "sample.txt")
             if english else ("伪装样本.mp4", "普通压缩包.zip", "纯视频.mp4", "示例文本.txt"))
    disguised, plain, video = (WORK / name for name in names[:3])
    entry = names[3]
    with zipfile.ZipFile(plain, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(entry, "Zip Repair Desk preview\n" * 20)
    disguised.write_bytes(mp4_prefix() + plain.read_bytes())
    video.write_bytes(mp4_prefix())
    return [disguised, plain, video]


def capture(window: int, output: Path, inset: int = 4) -> tuple[int, int]:
    # Only the client area is captured: no desktop, shadow, title bar or resize border.
    USER32.SetWindowPos(window, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010)
    USER32.SetCursorPos(0, 0)
    USER32.UpdateWindow(window)
    time.sleep(0.5)
    rect = wintypes.RECT()
    if not USER32.GetClientRect(window, ctypes.byref(rect)):
        raise ctypes.WinError(ctypes.get_last_error())
    point = wintypes.POINT(0, 0)
    if not USER32.ClientToScreen(window, ctypes.byref(point)):
        raise ctypes.WinError(ctypes.get_last_error())
    box = (point.x + inset, point.y + inset,
           point.x + rect.right - inset, point.y + rect.bottom - inset)
    image = ImageGrab.grab(bbox=box, all_screens=True)
    image.save(output)
    return image.size


def saved_preferences():
    try:
        key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_READ)
    except FileNotFoundError:
        return None
    values = {}
    with key:
        for name in ("Theme", "Language"):
            try:
                values[name] = winreg.QueryValueEx(key, name)[0]
            except FileNotFoundError:
                values[name] = None
    return values


def restore_preferences(values) -> None:
    if values is None:
        return
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0, winreg.KEY_SET_VALUE) as key:
        for name, value in values.items():
            if value is None:
                try:
                    winreg.DeleteValue(key, name)
                except FileNotFoundError:
                    pass
            else:
                winreg.SetValueEx(key, name, 0, winreg.REG_DWORD, value)


def main() -> None:
    preferences = saved_preferences()
    process = None
    try:
        for english, filename in ((False, "preview.png"), (True, "preview-en.png")):
            paths = samples(english)
            process, window = launch(EXE, paths,
                ready=lambda w: USER32.SendMessageW(USER32.GetDlgItem(w, IDC_LIST), LVM_GETITEMCOUNT, 0, 0) == 3)
            try:
                select_mode(window, english)
                size = capture(window, ROOT / filename)
                print(f"{filename}: {size[0]}x{size[1]}, client area inset 4 px")
            finally:
                process.terminate()
                process.wait(timeout=5)
                process = None
    finally:
        if process is not None:
            process.kill()
        restore_preferences(preferences)


if __name__ == "__main__":
    main()
