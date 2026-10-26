"""启动某个 exe，量它的内存 / 句柄 / 线程，然后关掉。

用法：python measure.py <exe 路径> [等待秒数]
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

psapi = ctypes.WinDLL("psapi")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32")
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.CloseHandle.argtypes = [wt.HANDLE]
u32.GetGuiResources.argtypes = [wt.HANDLE, wt.DWORD]
u32.GetGuiResources.restype = wt.DWORD


class PMC(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
                ("PrivateUsage", ctypes.c_size_t),
                ("_pad", ctypes.c_size_t)]


psapi.GetProcessMemoryInfo.argtypes = [wt.HANDLE, ctypes.POINTER(PMC), wt.DWORD]
psapi.GetProcessMemoryInfo.restype = wt.BOOL
MB = 1024.0 * 1024.0


def sample(pid):
    hp = k32.OpenProcess(0x1010, False, pid)
    if not hp:
        return None
    try:
        pm = PMC()
        pm.cb = ctypes.sizeof(pm)
        psapi.GetProcessMemoryInfo(hp, ctypes.byref(pm), pm.cb)
        return {
            "private": pm.PrivateUsage,
            "ws": pm.WorkingSetSize,
            "peak": pm.PeakWorkingSetSize,
            "gdi": u32.GetGuiResources(hp, 0),
            "user": u32.GetGuiResources(hp, 1),
        }
    finally:
        k32.CloseHandle(hp)


def threads(pid):
    class TE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
                    ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                    ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                    ("dwFlags", wt.DWORD)]

    snap = k32.CreateToolhelp32Snapshot(4, 0)
    n, te = 0, TE()
    te.dwSize = ctypes.sizeof(te)
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            n += 1
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return n


def main():
    exe = sys.argv[1]
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 3.0
    size = os.path.getsize(exe)
    p = subprocess.Popen([exe])
    try:
        time.sleep(wait)
        s = sample(p.pid)
        if not s:
            print("拿不到进程信息（可能已退出）")
            return 1
        print(f"exe      : {os.path.basename(exe)}  {size/1024:.1f} KB")
        print(f"私有提交 : {s['private']/MB:.2f} MB")
        print(f"工作集   : {s['ws']/MB:.2f} MB   (峰值 {s['peak']/MB:.2f} MB)")
        print(f"GDI/USER : {s['gdi']} / {s['user']}")
        print(f"线程数   : {threads(p.pid)}")
        return 0
    finally:
        p.kill()
        p.wait()


if __name__ == "__main__":
    sys.exit(main())
