/*
htty-server - the background half of headless-tty

Owns a Windows ConPTY with the command running inside it and serves it over a named pipe, so that htty -a can attach, detach and attach again while the command keeps running.

Normally started through htty -s, which launches it detached from the terminal.

Usage: htty-server --name <session> [options] -- <command> [args...]
 */

#include "headless_tty/pty.hpp"
#include "headless_tty/session.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

void print_usage(const char* program_name) {
    std::cerr << "htty-server - session server of headless-tty, normally started through: htty -s\n\n";
    std::cerr << "Usage: " << program_name << " --name <session> [options] -- <command> [args...]\n\n";
    std::cerr << "Options:\n";
    std::cerr << "  --name <session>   Session name, attach with: htty -a <session>\n";
    std::cerr << "  --wait             After the command exits, stay until a client has collected the output\n";
    std::cerr << "  --width <cols>     Terminal width until a client attaches, default 120\n";
    std::cerr << "  --height <rows>    Terminal height until a client attaches, default 40\n";
    std::cerr << "  --help, -h         Show this help message\n";
    std::cerr << "\n";
    std::cerr << "Example:\n";
    std::cerr << "  " << program_name << " --name build --wait -- cmd /c build.bat\n";
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
    std::wstring command;
    std::wstring args;
    std::wstring session_name;
    bool wait = false;
    bool help = false;
    bool error = false;
    std::string error_msg;
};

Args fail(Args args, const std::string& msg) {
    args.error = true;
    args.error_msg = msg;
    return args;
}

Args parse_args(int argc, char* argv[]) {
    Args args;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        bool has_value = i + 1 < argc;

        if (arg == "--help" || arg == "-h") {
            args.help = true;
            return args;
        }
        else if (arg == "--width" || arg == "--height") {
            if (!has_value) return fail(args, arg + " requires a value");
            long value = strtol(argv[++i], nullptr, 10);
            if (value < 1 || value > 32767) return fail(args, arg + " must be between 1 and 32767");
            (arg == "--width" ? args.width : args.height) = static_cast<uint16_t>(value);
        }
        else if (arg == "--name") {
            if (!has_value) return fail(args, "--name requires a value");
            args.session_name = to_wstring(argv[++i]);
        }
        else if (arg == "--wait") {
            args.wait = true;
        }
        else if (arg == "--") {
            // Everything after "--" is the command and its arguments, important for other processes to pass its own arguments
            for (int j = i + 1; j < argc; ++j) {
                positional.push_back(argv[j]);
            }
            break;
        }
        else if (arg.substr(0, 2) == "--") {
            return fail(args, "Unknown option: " + arg);
        }
        else {
            positional.push_back(arg);
        }
    }

    // The name becomes part of a pipe path, which cannot hold a backslash
    if (args.session_name.empty()) return fail(args, "--name <session> is required");
    if (args.session_name.find(L'\\') != std::wstring::npos) return fail(args, "--name cannot contain a backslash");
    if (positional.empty()) return fail(args, "a command is required");

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

    return args;
}

// This is a GUI subsystem binary with no console of its own. Text is only visible if the console of the launcher is borrowed, which is htty -s while it waits for the ready signal.
void borrow_parent_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* dummy;
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
}

void report_error(const std::string& msg) {
    borrow_parent_console();
    std::cerr << "Error: " << msg << std::endl;
}


int main(int argc, char* argv[]) {
    Args args = parse_args(argc, argv);

    if (args.help) {
        borrow_parent_console();
        print_usage(argv[0]);
        return 0;
    }
    if (args.error) {
        report_error(args.error_msg);
        print_usage(argv[0]);
        return 1;
    }

    headless_tty::HeadlessTTY tty;

    headless_tty::Config config;
    config.size.cols = args.width;
    config.size.rows = args.height;
    config.command = args.command;
    config.args = args.args;

    // Opened before anything is spawned, so a name clash never starts and then kills a command
    headless_tty::Session session(tty, args.session_name, config.size, args.wait);
    if (!session.open()) {
        report_error(session.get_last_error());
        return 1;
    }

    // Inherited by the child, lets htty -a refuse to attach to the session it runs in
    SetEnvironmentVariableW(headless_tty::SESSION_ENV_VAR, args.session_name.c_str());

    tty.set_output_callback([&session](const uint8_t* data, size_t length) {
        session.on_output(data, length);
    });

    if (!tty.start(config)) {
        report_error("Failed to start the command: " + tty.get_last_error());
        return 1;
    }
    session.serve();

    // Tells a waiting htty -s that the command is really running. The event is absent when the server was started by hand.
    std::wstring ready_name = std::wstring(headless_tty::READY_EVENT_PREFIX) + args.session_name;
    HANDLE hReady = OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str());
    if (hReady) {
        SetEvent(hReady);
        CloseHandle(hReady);
    }

    while (tty.is_running()) {
        Sleep(100);
    }

    // Joins the PTY read thread first, so every byte of output is in the session before the exit code goes out
    tty.stop();

    int exitCode = tty.wait(0);
    if (exitCode < 0) exitCode = 0;
    session.finish(exitCode);
    return exitCode;
}
