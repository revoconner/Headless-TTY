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
#include <string>
#include <thread>
#include <vector>

using namespace headless_tty;

namespace {

constexpr uint8_t DETACH_KEY = 0x1C;  // Ctrl-backslash

std::atomic<bool> g_quit{ false };

bool write_all(HANDLE h, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        DWORD written = 0;
        if (!WriteFile(h, data + sent, static_cast<DWORD>(len - sent), &written, NULL) || written == 0) {
            return false;
        }
        sent += written;
    }
    return true;
}

bool read_all(HANDLE h, uint8_t* data, size_t len) {
    size_t got = 0;
    while (got < len) {
        DWORD r = 0;
        if (!ReadFile(h, data + got, static_cast<DWORD>(len - got), &r, NULL) || r == 0) {
            return false;
        }
        got += r;
    }
    return true;
}

void send_frame(HANDLE h, FrameType type, const uint8_t* payload, size_t len) {
    uint8_t header[FRAME_HEADER_SIZE];
    header[0] = static_cast<uint8_t>(type);
    uint32_t n = static_cast<uint32_t>(len);
    memcpy(header + 1, &n, 4);

    if (!write_all(h, header, sizeof(header))) {
        g_quit.store(true);
        return;
    }
    if (len && !write_all(h, payload, len)) {
        g_quit.store(true);
    }
}

void pump_output(HANDLE hPipe, HANDLE hOut) {
    for (;;) {
        uint8_t header[FRAME_HEADER_SIZE];
        if (!read_all(hPipe, header, sizeof(header))) break;

        uint32_t len = 0;
        memcpy(&len, header + 1, 4);
        if (len > MAX_FRAME_PAYLOAD) break;

        std::vector<uint8_t> payload(len);
        if (len && !read_all(hPipe, payload.data(), len)) break;

        if (static_cast<FrameType>(header[0]) == FrameType::Data && len) {
            DWORD written = 0;
            WriteFile(hOut, payload.data(), len, &written, NULL);
        }
    }
    g_quit.store(true);
}

void watch_resize(HANDLE hPipe, HANDLE hOut) {
    uint16_t last_cols = 0, last_rows = 0;

    while (!g_quit.load()) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            uint16_t cols = static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
            uint16_t rows = static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);

            if (cols != last_cols || rows != last_rows) {
                uint8_t payload[4];
                memcpy(payload, &cols, 2);
                memcpy(payload + 2, &rows, 2);
                send_frame(hPipe, FrameType::Resize, payload, sizeof(payload));
                last_cols = cols;
                last_rows = rows;
            }
        }
        Sleep(500);
    }
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: htty-client <name>\n");
        return 1;
    }

    std::wstring pipe_name = L"\\\\.\\pipe\\htty-";
    pipe_name += argv[1];

    HANDLE hPipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"cannot attach to %s: %lu\n", argv[1], GetLastError());
        return 1;
    }

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    DWORD in_mode = 0, out_mode = 0;
    GetConsoleMode(hIn, &in_mode);
    GetConsoleMode(hOut, &out_mode);

    // Raw input, no line buffering, no echo, no local Ctrl-C handling
    SetConsoleMode(hIn, ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(hOut, out_mode | ENABLE_PROCESSED_OUTPUT
                                  | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                                  | DISABLE_NEWLINE_AUTO_RETURN);

    std::thread out_thread(pump_output, hPipe, hOut);
    std::thread size_thread(watch_resize, hPipe, hOut);

    uint8_t buf[4096];
    while (!g_quit.load()) {
        DWORD got = 0;
        if (!ReadFile(hIn, buf, sizeof(buf), &got, NULL) || got == 0) break;

        DWORD start = 0;
        bool detach = false;

        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] == DETACH_KEY) {
                if (i > start) send_frame(hPipe, FrameType::Data, buf + start, i - start);
                detach = true;
                break;
            }
        }

        if (!detach && got > start) {
            send_frame(hPipe, FrameType::Data, buf + start, got - start);
        }
        if (detach) break;
    }

    g_quit.store(true);
    CancelIoEx(hPipe, NULL);
    CloseHandle(hPipe);

    if (out_thread.joinable()) out_thread.join();
    if (size_thread.joinable()) size_thread.join();

    SetConsoleMode(hIn, in_mode);
    SetConsoleMode(hOut, out_mode);

    fwprintf(stderr, L"\ndetached\n");
    return 0;
}
