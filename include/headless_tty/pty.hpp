#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <consoleapi.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>

#include "types.hpp"

namespace headless_tty {


// ConPTY - Windows Pseudo Console wrapper
// Creates a real pseudo-terminal that makes isatty() return true for spawned processes, even without a visible console window.

class ConPTY {
public:
    ConPTY();
    ~ConPTY();

    // Non-copyable, movable
    ConPTY(const ConPTY&) = delete;
    ConPTY& operator=(const ConPTY&) = delete;
    ConPTY(ConPTY&& other) noexcept;
    ConPTY& operator=(ConPTY&& other) noexcept;


    bool initialize(const TerminalSize& size);

    /*
     Spawn a process attached to the PTY
     @param command The command to run (e.g., "cmd.exe", "claude.exe")
     @param args Command line arguments
     @param working_dir Working directory (empty = current)
     @return true if process started successfully
     */

    bool spawn(const std::wstring& command,
               const std::wstring& args = L"",
               const std::wstring& working_dir = L"");
    bool write(const uint8_t* data, size_t length);
    bool write(const std::string& str);
    void set_output_callback(OutputCallback callback); //callback
    void start_reading();
    void stop();
    bool is_running() const;

    /*
     Wait for the process to exit @param timeout_ms Timeout in milliseconds (INFINITE for no timeout)
     @return Exit code of the process, or -1 on error
     */
    int wait(DWORD timeout_ms = INFINITE);
    bool resize(const TerminalSize& size); 
    /*
    Not used in the headless-tty since its
    meant to be headless. 
    #include <headless_tty/pty.hpp>

    headless_tty::ConPTY pty;
    pty.initialize({120, 40});
    pty.spawn(L"notepad.exe");
    pty.start_reading();
    pty.resize({80, 24});
    */

    std::string get_last_error() const;

private:
    void cleanup();
    void read_loop();
    void monitor_loop();
    bool create_pipes();
    bool create_pseudo_console(const TerminalSize& size);
    bool initialize_startup_info();

    HPCON m_hPC = nullptr;
    HANDLE m_hPipeIn = nullptr;   // PTY reads from this (our write end)
    HANDLE m_hPipeOut = nullptr;  // PTY writes to this (our read end)
    HANDLE m_hPipePTYIn = nullptr;  // PTY's read end
    HANDLE m_hPipePTYOut = nullptr; // PTY's write end
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread = nullptr;
    HANDLE m_hJob = nullptr;
    PROCESS_INFORMATION m_processInfo = {};
    STARTUPINFOEXW m_startupInfo = {};
    std::unique_ptr<uint8_t[]> m_attributeList;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stop_requested{ false };
    std::thread m_read_thread;
    std::thread m_monitor_thread;
    mutable std::mutex m_mutex;

    // Callbacks
    OutputCallback m_output_callback;
    mutable std::string m_last_error;
    void set_error(const std::string& msg);
    void set_win_error(const std::string& prefix);
};


class HeadlessTTY {
public:
    HeadlessTTY() = default;
    ~HeadlessTTY();

    /*
     Run a command in the headless PTY
     @param config Configuration including command and size
     @return true if started successfully
     */
    bool start(const Config& config);
    bool write(const std::string& input);
    bool write(const uint8_t* data, size_t length);
    void set_output_callback(OutputCallback callback);
    void stop();
    bool is_running() const;
    int wait(DWORD timeout_ms = INFINITE);
    std::string get_last_error() const;

private:
    std::unique_ptr<ConPTY> m_pty;
    // Config m_config;  // Unused - kept for potential future use
};

} // namespace headless_tty
