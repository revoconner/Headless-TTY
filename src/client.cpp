// Attach to a running headless-tty session.
// Ctrl-\ detaches and leaves the session alive.
// usage: htty-client <name>

#include "headless_tty/protocol.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

using namespace headless_tty;

namespace {

constexpr wchar_t DETACH_KEY = 0x1C;  // Ctrl-backslash

// Soft reset, clear and home. Drawn before a replay so stale local state does not mix with it, without wiping the local scrollback like a hard reset would.
constexpr char RESET_SCREEN[] = "\x1b[!p\x1b[2J\x1b[H";

// Undo whatever modes the session left on: alt screen, colors, hidden cursor, mouse tracking, focus events, bracketed paste
constexpr char RESTORE_TERMINAL[] = "\x1b[?1049l\x1b[0m\x1b[?25h\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1004l\x1b[?2004l\x1b[!p";

// Manual reset event, set once by whichever thread decides the client is done
HANDLE g_hQuit = NULL;

std::atomic<bool> g_session_ended{ false };
std::atomic<int> g_exit_code{ 0 };

// Overlapped transfer of exactly len bytes, abandoned when g_hQuit is set. The pipe is opened overlapped so a pending read never blocks a write on the same handle. Each thread passes its own event.
bool pipe_transfer(HANDLE hPipe, HANDLE hEvent, bool is_write, uint8_t* data, size_t len) {
    size_t done = 0;
    while (done < len) {
        OVERLAPPED ov = {};
        ov.hEvent = hEvent;
        DWORD chunk = static_cast<DWORD>(len - done);
        DWORD n = 0;

        BOOL ok = is_write ? WriteFile(hPipe, data + done, chunk, NULL, &ov)
                           : ReadFile(hPipe, data + done, chunk, NULL, &ov);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE waits[2] = { hEvent, g_hQuit };
            if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(hPipe, &ov);
            }
        }
        // Also waits out a cancelled request, so ov is never freed while the kernel still owns it
        if (!GetOverlappedResult(hPipe, &ov, &n, TRUE) || n == 0) return false;
        done += n;
    }
    return true;
}

// Header and payload go out as one write so a frame can never be split by another frame
bool send_frame(HANDLE hPipe, HANDLE hEvent, FrameType type, const void* payload, size_t len) {
    std::vector<uint8_t> frame(FRAME_HEADER_SIZE + len);
    frame[0] = static_cast<uint8_t>(type);
    uint32_t n = static_cast<uint32_t>(len);
    memcpy(frame.data() + 1, &n, 4);
    if (len) memcpy(frame.data() + FRAME_HEADER_SIZE, payload, len);
    return pipe_transfer(hPipe, hEvent, true, frame.data(), frame.size());
}

void write_console(HANDLE hOut, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    while (len) {
        DWORD written = 0;
        if (!WriteFile(hOut, p, static_cast<DWORD>(len), &written, NULL) || written == 0) return;
        p += written;
        len -= written;
    }
}

void pump_output(HANDLE hPipe, HANDLE hOut) {
    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    std::vector<uint8_t> payload;

    for (;;) {
        uint8_t header[FRAME_HEADER_SIZE];
        if (!pipe_transfer(hPipe, hEvent, false, header, sizeof(header))) break;

        uint32_t len = 0;
        memcpy(&len, header + 1, 4);
        if (len > MAX_FRAME_PAYLOAD) break;

        payload.resize(len);
        if (len && !pipe_transfer(hPipe, hEvent, false, payload.data(), len)) break;

        FrameType type = static_cast<FrameType>(header[0]);
        if (type == FrameType::Replay) {
            write_console(hOut, RESET_SCREEN, sizeof(RESET_SCREEN) - 1);
        }
        if (type == FrameType::Data || type == FrameType::Replay) {
            write_console(hOut, payload.data(), len);
        } else if (type == FrameType::Exit) {
            int32_t code = 0;
            if (len >= 4) memcpy(&code, payload.data(), 4);
            g_exit_code.store(code);
            g_session_ended.store(true);
            break;
        }
    }

    CloseHandle(hEvent);
    SetEvent(g_hQuit);
}

bool get_window_size(HANDLE hOut, uint16_t& cols, uint16_t& rows) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return false;
    cols = static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
    rows = static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
    return true;
}

// The pipe is briefly absent while the server starts or swaps clients, so a missing pipe is retried for about 2 seconds
HANDLE connect_pipe(const std::wstring& path, DWORD& err) {
    for (int i = 0; i < 20; ++i) {
        // SECURITY_IDENTIFICATION stops a rogue process that squats the pipe name from impersonating us
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) break;
        Sleep(100);
    }
    return INVALID_HANDLE_VALUE;
}

std::string to_utf8(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), NULL, 0, NULL, NULL);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, NULL, NULL);
    return out;
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || !argv[1][0] || wcschr(argv[1], L'\\')) {
        fwprintf(stderr, L"usage: htty-client <name>\n");
        return 1;
    }
    std::wstring name = argv[1];

    wchar_t current[256];
    DWORD current_len = GetEnvironmentVariableW(SESSION_ENV_VAR, current, 256);
    if (current_len > 0 && current_len < 256 && name == current) {
        fwprintf(stderr, L"already inside session %ls\n", name.c_str());
        return 1;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD in_mode = 0, out_mode = 0;
    if (!GetConsoleMode(hIn, &in_mode) || !GetConsoleMode(hOut, &out_mode)) {
        fwprintf(stderr, L"htty-client needs a console on stdin and stdout\n");
        return 1;
    }

    DWORD err = 0;
    HANDLE hPipe = connect_pipe(std::wstring(PIPE_PREFIX) + name, err);
    if (hPipe == INVALID_HANDLE_VALUE) {
        if (err == ERROR_FILE_NOT_FOUND) fwprintf(stderr, L"no session named %ls\n", name.c_str());
        else if (err == ERROR_PIPE_BUSY) fwprintf(stderr, L"another client is already attached to %ls\n", name.c_str());
        else fwprintf(stderr, L"cannot attach to %ls: error %lu\n", name.c_str(), err);
        return 1;
    }

    g_hQuit = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    // Raw VT input with no line buffering, echo or local Ctrl-C handling. Extended flags without quick edit, because a mouse selection would freeze output. ConPTY output is UTF-8.
    UINT out_cp = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleMode(hIn, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    SetConsoleMode(hOut, out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);

    uint16_t cols = 120, rows = 40;
    get_window_size(hOut, cols, rows);
    uint8_t hello[5] = { PROTOCOL_VERSION };
    memcpy(hello + 1, &cols, 2);
    memcpy(hello + 3, &rows, 2);
    bool alive = send_frame(hPipe, hEvent, FrameType::Hello, hello, sizeof(hello));

    std::thread out_thread(pump_output, hPipe, hOut);

    // Input is read as records, not bytes, so text arrives as UTF-16 whatever the console input code page is. With VT input on, special keys already arrive as escape sequences, one character per record.
    HANDLE waits[2] = { hIn, g_hQuit };
    std::wstring pending;
    bool detach = false;

    while (alive && !detach) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 500);
        if (w != WAIT_OBJECT_0 && w != WAIT_TIMEOUT) break;

        if (w == WAIT_OBJECT_0) {
            INPUT_RECORD recs[128];
            DWORD count = 0;
            if (!ReadConsoleInputW(hIn, recs, 128, &count)) break;

            std::wstring text = pending;
            pending.clear();

            for (DWORD i = 0; i < count && !detach; ++i) {
                if (recs[i].EventType != KEY_EVENT) continue;
                const KEY_EVENT_RECORD& key = recs[i].Event.KeyEvent;
                wchar_t ch = key.uChar.UnicodeChar;

                // Alt+numpad delivers its character on the key up of Alt, everything else on key down
                if (!ch || (!key.bKeyDown && key.wVirtualKeyCode != VK_MENU)) continue;

                if (ch == DETACH_KEY) detach = true;
                else text.append(key.bKeyDown && key.wRepeatCount > 1 ? key.wRepeatCount : 1, ch);
            }

            // Half of a surrogate pair waits for the record that completes it
            if (!text.empty() && IS_HIGH_SURROGATE(text.back())) {
                pending = text.back();
                text.pop_back();
            }
            if (!text.empty()) {
                std::string bytes = to_utf8(text);
                alive = send_frame(hPipe, hEvent, FrameType::Data, bytes.data(), bytes.size());
            }
        }

        // Checked on every wake, since a classic console window can change size without raising a buffer size event
        uint16_t new_cols = cols, new_rows = rows;
        if (alive && get_window_size(hOut, new_cols, new_rows) && (new_cols != cols || new_rows != rows)) {
            cols = new_cols;
            rows = new_rows;
            uint8_t size[4];
            memcpy(size, &cols, 2);
            memcpy(size + 2, &rows, 2);
            alive = send_frame(hPipe, hEvent, FrameType::Resize, size, sizeof(size));
        }
    }

    if (detach) send_frame(hPipe, hEvent, FrameType::Detach, NULL, 0);

    SetEvent(g_hQuit);
    out_thread.join();
    CloseHandle(hPipe);
    CloseHandle(hEvent);
    CloseHandle(g_hQuit);

    write_console(hOut, RESTORE_TERMINAL, sizeof(RESTORE_TERMINAL) - 1);
    SetConsoleMode(hIn, in_mode);
    SetConsoleMode(hOut, out_mode);
    SetConsoleOutputCP(out_cp);

    if (g_session_ended.load()) {
        fwprintf(stderr, L"\n[session %ls ended, exit code %d]\n", name.c_str(), g_exit_code.load());
        return g_exit_code.load();
    }
    if (detach) {
        fwprintf(stderr, L"\n[detached from %ls]\n", name.c_str());
        return 0;
    }
    fwprintf(stderr, L"\n[connection to %ls lost]\n", name.c_str());
    return 1;
}
