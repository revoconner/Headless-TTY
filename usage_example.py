
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

def get_all_descendants(parent_pid):
    """
    Get ALL descendant process PIDs recursively (children, grandchildren, etc.)
    Useful for UWP apps that spawn multiple processes.

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

    # Build a map of parent_pid -> [child processes]
    snapshot = _kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
    if snapshot == INVALID_HANDLE_VALUE or snapshot == 0:
        return []

    try:
        # First pass: build parent->children map
        parent_map = {}
        process_info = {}
        pe32 = PROCESSENTRY32()
        pe32.dwSize = ctypes.sizeof(PROCESSENTRY32)

        if _kernel32.Process32First(snapshot, ctypes.byref(pe32)):
            while True:
                pid = pe32.th32ProcessID
                ppid = pe32.th32ParentProcessID
                name = pe32.szExeFile.decode("utf-8", errors="ignore")

                process_info[pid] = {"pid": pid, "name": name}
                if ppid not in parent_map:
                    parent_map[ppid] = []
                parent_map[ppid].append(pid)

                if not _kernel32.Process32Next(snapshot, ctypes.byref(pe32)):
                    break

        # Recursively collect all descendants
        descendants = []
        def collect_descendants(pid):
            if pid in parent_map:
                for child_pid in parent_map[pid]:
                    if child_pid in process_info:
                        descendants.append(process_info[child_pid])
                        collect_descendants(child_pid)

        collect_descendants(parent_pid)
        return descendants
    finally:
        _kernel32.CloseHandle(snapshot)



headless_tty_exe = "headless-tty.exe" # Use path r"path to headless-tty.exe" such as r"C:\my folder\headless-tty.exe", or if running from the same folder use "headless-tty.exe
cmd_args = r"ipconfig -all >%temp%\ipconfig.txt && notepad %temp%\ipconfig.txt" #writing ipconfig -all data to a txt file in temp and opening that in notepad

cmd = [
    str(headless_tty_exe),
    "--sys-tray",
    "--",
    "cmd",
    "/c",
    str(cmd_args)
]

headless_process = subprocess.Popen(
    cmd
)

# Wait a moment for child process to spawn
time.sleep(0.5)

msg = f"headless-tty PID: {headless_process.pid}\n"

# Get all descendant processes (children, grandchildren, etc.)
children = get_all_descendants(headless_process.pid)
if children:
    for child in children:
        msg += f"Child process PID: {child['pid']} ({child['name']})\n"
else:
    msg += "No child processes found (yet)"


normal_message = "No console shows even though notepad is running\nas a child process of headless-tty using conhost."
warning_message= "Uh Oh! You are running a modern version of notepad,\nUWP spawns multiple child, killing parent, won't work.\nwe must kill child"

if len(children)>3:
    message = str(normal_message + "\n " + warning_message)
else:
    message = str(normal_message)


root = tk.Tk()
root.title("Headless-TTY Test")
root.resizable(False, False)

root.eval('tk::PlaceWindow . left')

header = tk.Label(
    root,
    text=message,
    justify=tk.LEFT,
    padx=20,
    pady=10,
    font=("Segoe UI", 10)
)
header.pack()

label = tk.Label(root, text=msg, justify=tk.LEFT, padx=20, pady=10)
label.pack()

btn_frame = tk.Frame(root)
btn_frame.pack(pady=10)

def on_dismiss():
    root.destroy()

def on_kill_child():
    # Kill the child process (notepad) - this will trigger headless-tty to exit via monitor thread
    if children:
        PROCESS_TERMINATE = 0x0001
        for child in children:
            handle = ctypes.windll.kernel32.OpenProcess(PROCESS_TERMINATE, False, child['pid'])
            if handle:
                ctypes.windll.kernel32.TerminateProcess(handle, 0)
                ctypes.windll.kernel32.CloseHandle(handle)
    root.destroy()

def on_kill_parent():
    # Kill the parent process (headless-tty) - this will kill child via job object (doesn't work on UWP so we go back to killing the last children
    if len(children) > 3:
        print("UWP app detected, killing headless-tty or conhost will not work, we must kill the last child")
        # UWP app - kill conhost (always second child) to trigger clean shutdown
        PROCESS_TERMINATE = 0x0001
        handle = ctypes.windll.kernel32.OpenProcess(PROCESS_TERMINATE, False, children[3]['pid'])
        if handle:
            ctypes.windll.kernel32.TerminateProcess(handle, 0)
            ctypes.windll.kernel32.CloseHandle(handle)
    else:
        # Normal app - terminate headless-tty directly
        headless_process.terminate()
    root.destroy()

dismiss_btn = tk.Button(btn_frame, text="Dismiss", width=15, command=on_dismiss, bg="#377d3b", fg="white", activebackground="#2f5131")
dismiss_btn.pack(side=tk.LEFT, padx=5)

kill_child_btn = tk.Button(btn_frame, text="Kill Child (notepad)", width=20, command=on_kill_child, bg="#1c6392", fg="white", activebackground="#2e5981")
kill_child_btn.pack(side=tk.LEFT, padx=5)

kill_parent_btn = tk.Button(btn_frame, text="Kill Parent (headless-tty)", width=25, command=on_kill_parent, bg="#92231c", fg="white", activebackground="#81332e")
kill_parent_btn.pack(side=tk.LEFT, padx=5)

root.mainloop()
