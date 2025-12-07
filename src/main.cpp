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


static std::atomic<bool> g_shutdown_requested{ false };

void signal_handler(int signum) {
    (void)signum;
    g_shutdown_requested.store(true);
}


void print_usage(const char* program_name) {
    std::cerr << "headless-tty v2.0.0 - A headless terminal that keeps isatty() = true\n\n";
    std::cerr << "Usage: " << program_name << " [options] [command] [args...]\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --width <cols>     Terminal width (default: 120)\n";
    std::cerr << "  --height <rows>    Terminal height (default: 40)\n";
    std::cerr << "  --help, -h         Show this help message\n";
    std::cerr << "\n";
    std::cerr << "If no command is specified, cmd.exe is used.\n";
    std::cerr << "\n";
    std::cerr << "Examples:\n";
    std::cerr << "  " << program_name << " app_name\n";
    std::cerr << "  " << program_name << " --width 80 --height 24 pythonw\n";
    std::cerr << "  " << program_name << " cmd /c dir\n";
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
    uint16_t width = 10;
    uint16_t height = 10;
    std::wstring command = L"cmd.exe";
    std::wstring args;
    bool help = false;
    bool error = false;
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

int main(int argc, char* argv[]) {

    Args args = parse_args(argc, argv);

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
        return 0;
    }

    if (args.error) {
        std::cerr << "Error: " << args.error_msg << "\n\n";
        print_usage(argv[0]);
        return 1;
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
