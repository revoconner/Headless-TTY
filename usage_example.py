
#####################################
## Run with pythonw usage_example.py ######
## -------------------------------------------------#
## Running with pythonw, instead of python ###
## Runs python without attached console #####
## so you can truly appreciate the example   ###
#####################################

import subprocess
import ctypes
from ctypes import wintypes
import time
import tkinter as tk
from tkinter import messagebox

# Windows API constants
TH32CS_SNAPPROCESS = 0x00000002
_kernel32 = ctypes.windll.kernel32

class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", ctypes.c_char * 260),
    ]

def get_child_processes(parent_pid):
    """
    Get child process PIDs using Windows API.

    Args:
        parent_pid: Parent process ID

    Returns:
        list: List of dicts with 'pid' and 'name' keys
    """
    # Configure return types for 64-bit handle support
    _kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    _kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    _kernel32.Process32First.restype = wintypes.BOOL
    _kernel32.Process32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32)]
    _kernel32.Process32Next.restype = wintypes.BOOL
    _kernel32.Process32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32)]

    snapshot = _kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)

    # Check for INVALID_HANDLE_VALUE (-1)
    INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
    if snapshot == INVALID_HANDLE_VALUE or snapshot == 0:
        return []

    try:
        children = []
        pe32 = PROCESSENTRY32()
        pe32.dwSize = ctypes.sizeof(PROCESSENTRY32)

        if _kernel32.Process32First(snapshot, ctypes.byref(pe32)):
            while True:
                if pe32.th32ParentProcessID == parent_pid:
                    children.append({
                        "pid": pe32.th32ProcessID,
                        "name": pe32.szExeFile.decode("utf-8", errors="ignore")
                    })
                if not _kernel32.Process32Next(snapshot, ctypes.byref(pe32)):
                    break

        return children
    finally:
        _kernel32.CloseHandle(snapshot)



headless_tty_exe = "headless-tty.exe" # Use path r"path to headless-tty.exe" such as r"C:\my folder\headless-tty.exe"
cmd = [
    str(headless_tty_exe),
    "--",
    "notepad"
]

headless_process = subprocess.Popen(
    cmd
)

# Wait a moment for child process to spawn
time.sleep(0.5)

msg = f"headless-tty PID: {headless_process.pid}\n"

# Get child processes
children = get_child_processes(headless_process.pid)
if children:
    for child in children:
        msg += f"Child process PID: {child['pid']} ({child['name']})\n"
else:
    msg += "No child processes found (yet)"

root = tk.Tk()
root.title("Headless-TTY Test")
root.resizable(False, False)

root.eval('tk::PlaceWindow . center')

header = tk.Label(
    root,
    text="No console shows even though notepad is running\nas a child process of headless-tty using conhost",
    justify=tk.CENTER,
    padx=20,
    pady=10,
    font=("Segoe UI", 10)
)
header.pack()

# PID info label
label = tk.Label(root, text=msg, justify=tk.LEFT, padx=20, pady=10)
label.pack()

btn_frame = tk.Frame(root)
btn_frame.pack(pady=10)

def on_ok():
    root.destroy()

def on_kill():
    headless_process.terminate()
    root.destroy()

ok_btn = tk.Button(btn_frame, text="Dismiss this message", width=30, command=on_ok, bg="#377d3b", fg="white", activebackground="#2f5131")
ok_btn.pack(side=tk.LEFT, padx=5)

kill_btn = tk.Button(btn_frame, text="Kill Headless TTY to kill all", width=30, command=on_kill, bg="#92231c", fg="white", activebackground="#81332e")
kill_btn.pack(side=tk.LEFT, padx=5)

root.mainloop()
