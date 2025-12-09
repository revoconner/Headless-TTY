/*
headless-tty - A headless terminal that keeps isatty() = true

This CLI tool creates a Windows ConPTY and spawns a process attached to it.
The spawned process will see isatty(stdin) = true and isatty(stdout) = true,
even without a visible console window.

Usage: headless-tty [options] [command] [args...]
 */

#include "headless_tty/pty.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <csignal>
#include <io.h>
#include <fcntl.h>
#include <shellapi.h>

// Tray icon message and menu IDs
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_SHOW_CONSOLE 1001

static std::atomic<bool> g_shutdown_requested{ false };

// Tray mode globals
static HWND g_tray_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static std::atomic<bool> g_console_visible{ false };
static HANDLE g_hConsoleOut = INVALID_HANDLE_VALUE;
static HANDLE g_hConsoleIn = INVALID_HANDLE_VALUE;

void signal_handler(int signum) {
    (void)signum;
    g_shutdown_requested.store(true);
}


void print_usage(const char* program_name) {
    std::cerr << "headless-tty v2.5.0 - A headless terminal that keeps isatty() = true, works with GUI apps, \n and can stay in system tray\n\n";
    std::cerr << "Usage: " << program_name << " [options] [command] [args...]\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --sys-tray         Run with system tray icon (right-click for menu)\n";
    std::cerr << "  --help, -h         Show this help message\n";
    std::cerr << "\n";
    std::cerr << "If no command is specified, notepad.exe opens.\n";
    std::cerr << "\n";
    std::cerr << "Examples:\n";
    std::cerr << "  " << program_name << " app_name\n";
    std::cerr << "  " << program_name << " cmd /c dir\n";
    std::cerr << "  " << program_name << " --sys-tray -- python -u main.py\n";
}

// Convert narrow string to wide string
std::wstring to_wstring(const std::string& str) {
    if (str.empty()) return L"";

    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(),
                                          static_cast<int>(str.length()), NULL, 0);
    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()),
                        &result[0], size_needed);
    return result;
}


struct Args {
    uint16_t width = 120;
    uint16_t height = 40;
    std::wstring command = L"notepad.exe";
    std::wstring args;
    bool help = false;
    bool error = false;
    bool sys_tray = false;
    std::string error_msg;
};

Args parse_args(int argc, char* argv[]) {
    Args args;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            args.help = true;
            return args;
        }
        else if (arg == "--width") {
            if (i + 1 >= argc) {
                args.error = true;
                args.error_msg = "--width requires a value";
                return args;
            }
            args.width = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--height") {
            if (i + 1 >= argc) {
                args.error = true;
                args.error_msg = "--height requires a value";
                return args;
            }
            args.height = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--sys-tray") {
            args.sys_tray = true;
        }
        else if (arg == "--") {
            // Everything after "--" is the command and its arguments, important for other processes to pass its own arguments
            for (int j = i + 1; j < argc; ++j) {
                positional.push_back(argv[j]);
            }
            break;
        }
        else if (arg.substr(0, 2) == "--") {
            args.error = true;
            args.error_msg = "Unknown option: " + arg;
            return args;
        }
        else {
            positional.push_back(arg);
        }
    }

    
    if (!positional.empty()) {
        args.command = to_wstring(positional[0]);

        
        std::string argsStr;
        for (size_t i = 1; i < positional.size(); ++i) {
            if (!argsStr.empty()) argsStr += " ";
            // Quote args with spaces
            if (positional[i].find(' ') != std::string::npos) {
                argsStr += "\"" + positional[i] + "\"";
            } else {
                argsStr += positional[i];
            }
        }
        args.args = to_wstring(argsStr);
    }

    return args;
}


void stdin_forwarder(headless_tty::HeadlessTTY& tty) {
    // Set stdin to binary mode to handle raw bytes
    _setmode(_fileno(stdin), _O_BINARY);

    char buffer[headless_tty::INPUT_BUFFER_SIZE];

    while (!g_shutdown_requested.load() && tty.is_running()) {
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD available = 0;


        INPUT_RECORD inputRecords[128];
        DWORD eventsRead = 0;

        if (GetConsoleMode(hStdin, &available)) {
            if (PeekConsoleInput(hStdin, inputRecords, 128, &eventsRead) && eventsRead > 0) {
                DWORD charsRead = 0;
                if (ReadConsoleA(hStdin, buffer, sizeof(buffer) - 1, &charsRead, NULL)) {
                    if (charsRead > 0) {
                        tty.write(reinterpret_cast<uint8_t*>(buffer), charsRead);
                    }
                }
            } else {
                Sleep(10);
            }
        } else {
            // Pipe mode - use ReadFile
            if (PeekNamedPipe(hStdin, NULL, 0, NULL, &available, NULL) && available > 0) {
                DWORD bytesRead = 0;
                if (ReadFile(hStdin, buffer, sizeof(buffer), &bytesRead, NULL) && bytesRead > 0) {
                    tty.write(reinterpret_cast<uint8_t*>(buffer), bytesRead);
                }
            } else {
                Sleep(10);
            }
        }
    }
}


// System Tray Mode Functions

// Console control handler - called when user closes console window
BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        g_shutdown_requested.store(true);
        if (g_tray_hwnd) {
            PostMessage(g_tray_hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }
    return FALSE;
}

// Show console window (allocate if needed)
void show_console() {
    if (g_console_visible.load()) return;

    if (!AllocConsole()) return;

    // Get standard handles after AllocConsole
    g_hConsoleOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hConsoleIn = GetStdHandle(STD_INPUT_HANDLE);

    if (g_hConsoleOut != INVALID_HANDLE_VALUE) {
        // Enable VT processing for proper terminal rendering
        DWORD outMode = 0;
        if (GetConsoleMode(g_hConsoleOut, &outMode)) {
            SetConsoleMode(g_hConsoleOut, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    if (g_hConsoleIn != INVALID_HANDLE_VALUE) {
        // Enable line input with echo
        SetConsoleMode(g_hConsoleIn, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    }

    SetConsoleTitleW(L"headless-tty");
    g_console_visible.store(true);

    // Register handler so closing console window exits app
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
}

// Show tray context menu
void show_tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (hMenu) {
        if (g_console_visible.load()) {
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"Hide Console");
        } else {
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_CONSOLE, L"Show Console");
        }

        // Required for menu to work properly
        SetForegroundWindow(hwnd);
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
        PostMessage(hwnd, WM_NULL, 0, 0);

        DestroyMenu(hMenu);
    }
}

// Hide console window
void hide_console() {
    if (!g_console_visible.load()) return;

    g_console_visible.store(false);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);

    // Invalidate handles (don't close - they're from GetStdHandle)
    g_hConsoleOut = INVALID_HANDLE_VALUE;
    g_hConsoleIn = INVALID_HANDLE_VALUE;

    FreeConsole();
}

// Tray window procedure
LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP) {
                show_tray_menu(hwnd);
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_SHOW_CONSOLE) {
                if (g_console_visible.load()) {
                    hide_console();
                } else {
                    show_console();
                }
            }
            break;

        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Setup system tray icon
bool setup_tray(HINSTANCE hInstance) {
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"HeadlessTTYTrayClass";

    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // Create hidden message-only window
    g_tray_hwnd = CreateWindowExW(
        0, L"HeadlessTTYTrayClass", L"HeadlessTTY",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, hInstance, NULL
    );

    if (!g_tray_hwnd) {
        return false;
    }

    // Setup tray icon
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_tray_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"headless-tty");

    return Shell_NotifyIconW(NIM_ADD, &g_nid) == TRUE;
}

// Remove tray icon
void remove_tray() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_tray_hwnd) {
        DestroyWindow(g_tray_hwnd);
        g_tray_hwnd = nullptr;
    }
}

// Console input forwarder for tray mode using raw input events
void tray_console_input_forwarder(headless_tty::HeadlessTTY& tty) {
    std::string lineBuffer;

    while (true) {
        if (g_shutdown_requested.load() || !tty.is_running()) {
            break;
        }

        HANDLE hIn = g_hConsoleIn;
        HANDLE hOut = g_hConsoleOut;
        if (!g_console_visible.load() || hIn == INVALID_HANDLE_VALUE) {
            Sleep(50);
            continue;
        }

        DWORD waitResult = WaitForSingleObject(hIn, 50);

        if (g_shutdown_requested.load() || !tty.is_running()) {
            break;
        }

        if (waitResult != WAIT_OBJECT_0 || !g_console_visible.load()) {
            continue;
        }

        // Read input events
        INPUT_RECORD record;
        DWORD eventsRead = 0;
        if (!ReadConsoleInputA(hIn, &record, 1, &eventsRead) || eventsRead == 0) {
            continue;
        }

        // Only process key down events
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
            continue;
        }

        char ch = record.Event.KeyEvent.uChar.AsciiChar;
        WORD vk = record.Event.KeyEvent.wVirtualKeyCode;

        if (vk == VK_RETURN) {
            // Echo newline and send line to PTY
            if (hOut != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteConsoleA(hOut, "\r\n", 2, &written, NULL);
            }
            lineBuffer += "\r\n";
            if (tty.is_running()) {
                tty.write(reinterpret_cast<const uint8_t*>(lineBuffer.c_str()), lineBuffer.size());
            }
            lineBuffer.clear();
        } else if (vk == VK_BACK) {
            // Handle backspace
            if (!lineBuffer.empty()) {
                lineBuffer.pop_back();
                if (hOut != INVALID_HANDLE_VALUE) {
                    DWORD written;
                    WriteConsoleA(hOut, "\b \b", 3, &written, NULL);
                }
            }
        } else if (ch >= 32 && ch < 127) {
            // Printable ASCII - echo and buffer
            lineBuffer += ch;
            if (hOut != INVALID_HANDLE_VALUE) {
                DWORD written;
                WriteConsoleA(hOut, &ch, 1, &written, NULL);
            }
        }
    }
}

// Run in system tray mode
int run_tray_mode(const Args& args) {
    HINSTANCE hInstance = GetModuleHandle(NULL);

    if (!setup_tray(hInstance)) {
        return 1;
    }

    // Create and start HeadlessTTY
    headless_tty::HeadlessTTY tty;

    headless_tty::Config config;
    config.size.cols = args.width;
    config.size.rows = args.height;
    config.command = args.command;
    config.args = args.args;

    if (!tty.start(config)) {
        remove_tray();
        return 1;
    }

    // Set output callback AFTER start() - m_pty must exist first
    tty.set_output_callback([](const uint8_t* data, size_t length) {
        if (g_console_visible.load() && g_hConsoleOut != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(g_hConsoleOut, data, static_cast<DWORD>(length), &written, NULL);
        }
    });

    // Start console input forwarder thread
    std::thread input_thread(tray_console_input_forwarder, std::ref(tty));

    // Message loop with periodic check for process exit
    MSG msg;
    while (!g_shutdown_requested.load()) {
        // Wait for messages or timeout (100ms) to check process status
        DWORD result = MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);

        if (result == WAIT_OBJECT_0) {
            // Process all pending messages
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    g_shutdown_requested.store(true);
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        // Check if child process exited
        if (!tty.is_running()) {
            g_shutdown_requested.store(true);
        }
    }

    // Cleanup
    g_shutdown_requested.store(true);
    tty.stop();

    // Input thread will exit on next loop iteration (100ms max)
    if (input_thread.joinable()) {
        input_thread.join();
    }

    if (g_console_visible.load()) {
        hide_console();
    }

    remove_tray();

    int exitCode = tty.wait(0);
    return exitCode >= 0 ? exitCode : 0;
}


int main(int argc, char* argv[]) {
    Args args = parse_args(argc, argv);

    // Attach to parent console only for help/error output
    if (args.help || args.error) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* dummy;
            freopen_s(&dummy, "CONOUT$", "w", stderr);
        }
    }

    /* 
    Detect if we have a console attached (for GUI subsystem support)
    If a process is not a console app, do not open console.
    If a console app is detected keep input/output true

    This allows the headless-TTY to open any GUI app such as notepad 
    without showing a console.

    As well as open console apps with isatty()=True
    without showing a console. 
    */
    
    bool has_console = false;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD console_mode;
    if (hStdin != INVALID_HANDLE_VALUE && hStdin != NULL) {
        // Check if it's a console or a pipe (both are valid for I/O)
        if (GetConsoleMode(hStdin, &console_mode) || GetFileType(hStdin) == FILE_TYPE_PIPE) {
            has_console = true;
        }
    }

    if (args.help) {
        print_usage(argv[0]);
        std::cerr.flush();
        FreeConsole();
        return 0;
    }

    if (args.error) {
        std::cerr << "Error: " << args.error_msg << "\n\n";
        print_usage(argv[0]);
        std::cerr.flush();
        FreeConsole();
        return 1;
    }

    // System tray mode - separate execution path
    if (args.sys_tray) {
        return run_tray_mode(args);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Set stdout to binary mode for raw output (only if we have valid handles)
    if (has_console) {
        _setmode(_fileno(stdout), _O_BINARY);
        _setmode(_fileno(stderr), _O_BINARY);
    }

    // Creation Happens here XD
    headless_tty::HeadlessTTY tty;


    headless_tty::Config config;
    config.size.cols = args.width;
    config.size.rows = args.height;
    config.command = args.command;
    config.args = args.args;

    // Only set output callback if we have somewhere to write
    if (has_console) {
        tty.set_output_callback([](const uint8_t* data, size_t length) {
            // Write directly to stdout
            DWORD bytesWritten;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data, static_cast<DWORD>(length), &bytesWritten, NULL);
        });
    }


    if (!tty.start(config)) {
        if (has_console) {
            std::cerr << "Failed to start headless TTY: " << tty.get_last_error() << std::endl;
        }
        return 1;
    }

    // Only start stdin forwarding if we have a console
    std::thread stdin_thread;
    if (has_console) {
        stdin_thread = std::thread(stdin_forwarder, std::ref(tty));
    }


    while (tty.is_running() && !g_shutdown_requested.load()) {
        Sleep(100);
    }

    g_shutdown_requested.store(true);
    tty.stop();

    if (stdin_thread.joinable()) {
        stdin_thread.join();
    }


    int exitCode = tty.wait(0);

    return exitCode >= 0 ? exitCode : 0;
}
