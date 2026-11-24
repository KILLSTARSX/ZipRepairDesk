"""在非主显示器上隐藏启动 GUI，再移到副屏显示，避免占用主屏。"""
from ctypes import wintypes
import ctypes
import subprocess
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, ctypes.c_void_p, wintypes.LPARAM]
user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.c_void_p]


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD)]


def _monitors():
    result = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                        ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
    def callback(monitor, _dc, _rect, _param):
        info = MONITORINFO()
        info.cbSize = ctypes.sizeof(MONITORINFO)
        if user32.GetMonitorInfoW(monitor, ctypes.byref(info)):
            result.append(info)
        return True

    user32.EnumDisplayMonitors(None, None, callback, 0)
    return result


def secondary():
    """返回未标记为主屏的工作区 (left, top, right, bottom)；只有一块屏时退回主屏。"""
    monitors = _monitors()
    for info in monitors:
        if not info.dwFlags & 1:
            return (info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom)
    info = monitors[0]
    return (info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom)


def launch(exe, args=(), ready=None, timeout=10.0):
    """隐藏启动，移到副屏后不激活显示；ready(window) 为 True 即返回。"""
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0  # SW_HIDE
    process = subprocess.Popen([str(exe), *map(str, args)], startupinfo=startup)
    window = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        window = user32.FindWindowW("ZipRepairDeskWindow", None)
        if window:
            break
        time.sleep(0.1)
    if not window:
        process.kill()
        raise RuntimeError("GUI window did not appear")
    left, top, right, bottom = secondary()
    rect = wintypes.RECT()
    user32.GetWindowRect(window, ctypes.byref(rect))
    width = min(rect.right - rect.left, (right - left) - 40)
    height = min(rect.bottom - rect.top, (bottom - top) - 40)
    x = left + ((right - left) - width) // 2
    y = top + ((bottom - top) - height) // 2
    user32.SetWindowPos(window, None, x, y, width, height,
                        0x0004 | 0x0010)  # SWP_NOZORDER | SWP_NOACTIVATE
    user32.ShowWindow(window, 4)  # SW_SHOWNOACTIVATE
    if ready:
        while time.time() < deadline:
            if ready(window):
                break
            time.sleep(0.1)
        else:
            process.kill()
            raise RuntimeError("GUI did not become ready")
    return process, window
