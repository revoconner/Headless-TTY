// Front end for headless-tty, a console program so that attaching can own the terminal.
// htty -s starts a session server in the background, htty -a attaches this terminal to one.

#include "headless_tty/client.hpp"
#include "headless_tty/protocol.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

using namespace headless_tty;

namespace {

constexpr wchar_t SERVER_EXE[] = L"htty-server.exe";

void print_usage() {
    fwprintf(stderr, L"headless-tty - terminal sessions that outlive the terminal they were started from\n\n");
    fwprintf(stderr, L"Usage:\n");
    fwprintf(stderr, L"  htty -s --name <session> [--wait] [--width <cols>] [--height <rows>] -- <command> [args...]\n");
    fwprintf(stderr, L"  htty -a <session>\n\n");
    fwprintf(stderr, L"  -s    Start a session in the background and return\n");
    fwprintf(stderr, L"  -a    Attach this terminal to a session, Ctrl-\\ detaches and leaves it running\n");
    fwprintf(stderr, L"  --wait    After the command exits, keep the session until a client has collected the output\n\n");
    fwprintf(stderr, L"Example:\n");
    fwprintf(stderr, L"  htty -s --name build --wait -- cmd /c build.bat\n");
    fwprintf(stderr, L"  htty -a build\n");
}

bool valid_name(const wchar_t* name) {
    return name && name[0] && !wcschr(name, L'\\');
}

// Only probes the pipe name, it never takes the server's single pipe instance
bool session_exists(const std::wstring& name) {
    std::wstring path = std::wstring(PIPE_PREFIX) + name;
    return WaitNamedPipeW(path.c_str(), 1) || GetLastError() != ERROR_FILE_NOT_FOUND;
}

// The server sits beside this program. A package manager may expose us through a symlink, so the real location is resolved first.
std::wstring server_path() {
    wchar_t module[1024] = {}, real[1024] = {};
    GetModuleFileNameW(NULL, module, 1024);
    std::wstring path = module;

    HANDLE h = CreateFileW(module, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD len = GetFinalPathNameByHandleW(h, real, 1024, FILE_NAME_NORMALIZED);
        if (len > 0 && len < 1024) path = real;
        CloseHandle(h);
    }

    path.resize(path.find_last_of(L'\\') + 1);
    return path + SERVER_EXE;
}

// Skips one token of a raw command line, quotes included, and the blanks after it
const wchar_t* skip_token(const wchar_t* p) {
    bool quoted = false;
    while (*p && (quoted || (*p != L' ' && *p != L'\t'))) {
        if (*p == L'"') quoted = !quoted;
        ++p;
    }
    while (*p == L' ' || *p == L'\t') ++p;
    return p;
}

int start_server(int argc, wchar_t** argv) {
    std::wstring name;
    bool has_command = false;

    for (int i = 2; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--")) {
            has_command = i + 1 < argc;
            break;
        }
        if (!wcscmp(argv[i], L"--name") && i + 1 < argc) name = argv[++i];
    }

    if (!valid_name(name.c_str())) {
        fwprintf(stderr, L"htty -s needs --name <session>, without a backslash in it\n");
        return 1;
    }
    if (!has_command) {
        fwprintf(stderr, L"htty -s needs a command after --\n");
        return 1;
    }
    if (session_exists(name)) {
        fwprintf(stderr, L"session %ls already exists, attach with: htty -a %ls\n", name.c_str(), name.c_str());
        return 1;
    }

    // Everything after -s is handed over exactly as typed, so the quoting of the command survives
    std::wstring server = server_path();
    std::wstring cmdline = L"\"" + server + L"\" " + skip_token(skip_token(GetCommandLineW()));

    // Set by the server once the command is running. The pipe proves nothing here, it exists before the command is spawned.
    std::wstring ready_name = std::wstring(READY_EVENT_PREFIX) + name;
    HANDLE hReady = CreateEventW(NULL, TRUE, FALSE, ready_name.c_str());
    if (!hReady) {
        fwprintf(stderr, L"cannot create the startup event: error %lu\n", GetLastError());
        return 1;
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    // Breakaway keeps the server alive when the launcher runs inside a kill on close job, as an SSH session does. A job that forbids breakaway gets a plain detached launch.
    DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = CreateProcessW(server.c_str(), &cmdline[0], NULL, NULL, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        ok = CreateProcessW(server.c_str(), &cmdline[0], NULL, NULL, FALSE, flags, NULL, NULL, &si, &pi);
    }
    if (!ok) {
        fwprintf(stderr, L"cannot start %ls: error %lu\n", server.c_str(), GetLastError());
        CloseHandle(hReady);
        return 1;
    }

    // While we wait here the server can still borrow this console to print its own startup error
    HANDLE waits[2] = { hReady, pi.hProcess };
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 5000);
    int result = 1;

    if (w == WAIT_OBJECT_0) {
        fwprintf(stderr, L"session %ls started, attach with: htty -a %ls\n", name.c_str(), name.c_str());
        result = 0;
    } else if (w == WAIT_OBJECT_0 + 1) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        fwprintf(stderr, L"session %ls failed to start, the server exited with code %lu\n", name.c_str(), code);
    } else {
        fwprintf(stderr, L"the server is running but session %ls did not report ready\n", name.c_str());
    }

    CloseHandle(hReady);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && !wcscmp(argv[1], L"-a")) {
        if (argc < 3 || !valid_name(argv[2])) {
            fwprintf(stderr, L"htty -a needs a session name\n");
            return 1;
        }
        return run_attach(argv[2]);
    }

    if (argc >= 2 && !wcscmp(argv[1], L"-s")) {
        return start_server(argc, argv);
    }

    print_usage();
    return argc >= 2 && (!wcscmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h")) ? 0 : 1;
}
