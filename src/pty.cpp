#include "headless_tty/pty.hpp"
#include <sstream>

namespace headless_tty {

ConPTY::ConPTY() {
    ZeroMemory(&m_processInfo, sizeof(m_processInfo));
    ZeroMemory(&m_startupInfo, sizeof(m_startupInfo));
}

ConPTY::~ConPTY() {
    stop();
    cleanup();
}

ConPTY::ConPTY(ConPTY&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.m_mutex);
    m_hPC = other.m_hPC;
    m_hPipeIn = other.m_hPipeIn;
    m_hPipeOut = other.m_hPipeOut;
    m_hPipePTYIn = other.m_hPipePTYIn;
    m_hPipePTYOut = other.m_hPipePTYOut;
    m_hProcess = other.m_hProcess;
    m_hThread = other.m_hThread;
    m_hJob = other.m_hJob;
    m_processInfo = other.m_processInfo;
    m_startupInfo = other.m_startupInfo;
    m_attributeList = std::move(other.m_attributeList);
    m_running.store(other.m_running.load());
    m_stop_requested.store(other.m_stop_requested.load());
    m_read_thread = std::move(other.m_read_thread);
    m_monitor_thread = std::move(other.m_monitor_thread);
    m_output_callback = std::move(other.m_output_callback);
    m_last_error = std::move(other.m_last_error);

    other.m_hPC = nullptr;
    other.m_hPipeIn = nullptr;
    other.m_hPipeOut = nullptr;
    other.m_hPipePTYIn = nullptr;
    other.m_hPipePTYOut = nullptr;
    other.m_hProcess = nullptr;
    other.m_hThread = nullptr;
    other.m_hJob = nullptr;
    other.m_running.store(false);
}

ConPTY& ConPTY::operator=(ConPTY&& other) noexcept {
    if (this != &other) {
        stop();
        cleanup();

        std::lock_guard<std::mutex> lock(other.m_mutex);
        m_hPC = other.m_hPC;
        m_hPipeIn = other.m_hPipeIn;
        m_hPipeOut = other.m_hPipeOut;
        m_hPipePTYIn = other.m_hPipePTYIn;
        m_hPipePTYOut = other.m_hPipePTYOut;
        m_hProcess = other.m_hProcess;
        m_hThread = other.m_hThread;
        m_hJob = other.m_hJob;
        m_processInfo = other.m_processInfo;
        m_startupInfo = other.m_startupInfo;
        m_attributeList = std::move(other.m_attributeList);
        m_running.store(other.m_running.load());
        m_stop_requested.store(other.m_stop_requested.load());
        m_read_thread = std::move(other.m_read_thread);
        m_monitor_thread = std::move(other.m_monitor_thread);
        m_output_callback = std::move(other.m_output_callback);
        m_last_error = std::move(other.m_last_error);

        other.m_hPC = nullptr;
        other.m_hPipeIn = nullptr;
        other.m_hPipeOut = nullptr;
        other.m_hPipePTYIn = nullptr;
        other.m_hPipePTYOut = nullptr;
        other.m_hProcess = nullptr;
        other.m_hThread = nullptr;
        other.m_hJob = nullptr;
        other.m_running.store(false);
    }
    return *this;
}

void ConPTY::set_error(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_error = msg;
}

void ConPTY::set_win_error(const std::string& prefix) {
    DWORD error = GetLastError();
    LPSTR messageBuffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&messageBuffer, 0, NULL);

    std::stringstream ss;
    ss << prefix << ": " << std::string(messageBuffer, size) << " (error " << error << ")";
    LocalFree(messageBuffer);

    set_error(ss.str());
}

std::string ConPTY::get_last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_last_error;
}

bool ConPTY::create_pipes() {
    if (!CreatePipe(&m_hPipePTYIn, &m_hPipeIn, NULL, 0)) {
        set_win_error("Failed to create input pipe");
        return false;
    }

    if (!CreatePipe(&m_hPipeOut, &m_hPipePTYOut, NULL, 0)) {
        set_win_error("Failed to create output pipe");
        CloseHandle(m_hPipePTYIn);
        CloseHandle(m_hPipeIn);
        m_hPipePTYIn = nullptr;
        m_hPipeIn = nullptr;
        return false;
    }

    return true;
}

bool ConPTY::create_pseudo_console(const TerminalSize& size) {
    COORD consoleSize;
    consoleSize.X = static_cast<SHORT>(size.cols);
    consoleSize.Y = static_cast<SHORT>(size.rows);

    HRESULT hr = CreatePseudoConsole(
        consoleSize,
        m_hPipePTYIn,
        m_hPipePTYOut,
        0,
        &m_hPC
    );

    if (FAILED(hr)) {
        std::stringstream ss;
        ss << "CreatePseudoConsole failed with HRESULT 0x" << std::hex << hr;
        set_error(ss.str());
        return false;
    }

    CloseHandle(m_hPipePTYIn);
    CloseHandle(m_hPipePTYOut);
    m_hPipePTYIn = nullptr;
    m_hPipePTYOut = nullptr;

    return true;
}

bool ConPTY::initialize_startup_info() {
    ZeroMemory(&m_startupInfo, sizeof(m_startupInfo));
    m_startupInfo.StartupInfo.cb = sizeof(STARTUPINFOEXW);

    SIZE_T attrListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

    m_attributeList = std::make_unique<uint8_t[]>(attrListSize);
    m_startupInfo.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attributeList.get());

    if (!InitializeProcThreadAttributeList(m_startupInfo.lpAttributeList, 1, 0, &attrListSize)) {
        set_win_error("InitializeProcThreadAttributeList failed");
        return false;
    }

    if (!UpdateProcThreadAttribute(
            m_startupInfo.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            m_hPC,
            sizeof(HPCON),
            NULL,
            NULL)) {
        set_win_error("UpdateProcThreadAttribute failed");
        DeleteProcThreadAttributeList(m_startupInfo.lpAttributeList);
        return false;
    }

    return true;
}

bool ConPTY::initialize(const TerminalSize& size) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!create_pipes()) {
        return false;
    }

    if (!create_pseudo_console(size)) {
        cleanup();
        return false;
    }

    if (!initialize_startup_info()) {
        cleanup();
        return false;
    }

    return true;
}

bool ConPTY::spawn(const std::wstring& command,
                   const std::wstring& args,
                   const std::wstring& working_dir) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_hPC) {
        set_error("PTY not initialized. Call initialize() first.");
        return false;
    }

    std::wstring cmdLine = command;
    if (!args.empty()) {
        cmdLine += L" " + args;
    }

    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(0);

    const wchar_t* workDir = working_dir.empty() ? NULL : working_dir.c_str();

    BOOL success = CreateProcessW(
        NULL,                           // Application name (use command line)
        cmdLineBuf.data(),              // Command line
        NULL,                           // Process security attributes
        NULL,                           // Thread security attributes
        FALSE,                          // Inherit handles
        EXTENDED_STARTUPINFO_PRESENT,   // Creation flags
        NULL,                           // Environment (inherit)
        workDir,                        // Working directory
        &m_startupInfo.StartupInfo,     // Startup info
        &m_processInfo                  // Process info output
    );

    if (!success) {
        set_win_error("CreateProcessW failed");
        return false;
    }

    m_hProcess = m_processInfo.hProcess;
    m_hThread = m_processInfo.hThread;

    // Job object ensures child dies when parent is killed (even forcefully)
    m_hJob = CreateJobObjectW(NULL, NULL);
    if (m_hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(m_hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
        AssignProcessToJobObject(m_hJob, m_hProcess);
    }

    m_running.store(true);
    m_stop_requested.store(false);

    return true;
}

void ConPTY::set_output_callback(OutputCallback callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_output_callback = std::move(callback);
}

void ConPTY::read_loop() {
    uint8_t buffer[PTY_BUFFER_SIZE];

    while (!m_stop_requested.load()) {
        DWORD bytesRead = 0;

        if (m_hProcess) {
            DWORD exitCode;
            if (GetExitCodeProcess(m_hProcess, &exitCode) && exitCode != STILL_ACTIVE) {
                DWORD bytesAvailable = 0;
                if (!PeekNamedPipe(m_hPipeOut, NULL, 0, NULL, &bytesAvailable, NULL) || bytesAvailable == 0) {
                    break;
                }
            }
        }

        BOOL success = ReadFile(m_hPipeOut, buffer, sizeof(buffer), &bytesRead, NULL);

        if (!success || bytesRead == 0) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA) {
                break;
            }
            Sleep(10);
            continue;
        }

        OutputCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_output_callback;
        }

        if (callback) {
            callback(buffer, bytesRead);
        }
    }

    m_running.store(false);
}

void ConPTY::monitor_loop() {
    if (!m_hProcess) return;

    // Wait for the child process to exit
    WaitForSingleObject(m_hProcess, INFINITE);

    // Child exited - close the pseudo console to break pipes
    // This will cause read_loop's ReadFile to return, allowing clean exit
    if (m_hPC && !m_stop_requested.load()) {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
    }
}

void ConPTY::start_reading() {
    if (m_read_thread.joinable()) {
        return;
    }
    m_read_thread = std::thread(&ConPTY::read_loop, this);

    // Start monitor thread to detect child process exit
    if (!m_monitor_thread.joinable()) {
        m_monitor_thread = std::thread(&ConPTY::monitor_loop, this);
    }
}

bool ConPTY::write(const uint8_t* data, size_t length) {
    if (!m_hPipeIn) {
        set_error("Write pipe not available");
        return false;
    }

    DWORD bytesWritten = 0;
    BOOL success = WriteFile(m_hPipeIn, data, static_cast<DWORD>(length), &bytesWritten, NULL);

    if (!success) {
        set_win_error("WriteFile failed");
        return false;
    }

    return bytesWritten == length;
}

bool ConPTY::write(const std::string& str) {
    return write(reinterpret_cast<const uint8_t*>(str.c_str()), str.length());
}

void ConPTY::stop() {
    m_stop_requested.store(true);

    if (m_hProcess) {
        DWORD exitCode;
        if (GetExitCodeProcess(m_hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            TerminateProcess(m_hProcess, 0);
        }
    }

    if (m_read_thread.joinable()) {
        m_read_thread.join();
    }

    if (m_monitor_thread.joinable()) {
        m_monitor_thread.join();
    }

    m_running.store(false);
}

bool ConPTY::is_running() const {
    return m_running.load();
}

int ConPTY::wait(DWORD timeout_ms) {
    if (!m_hProcess) {
        return -1;
    }

    DWORD result = WaitForSingleObject(m_hProcess, timeout_ms);

    if (result == WAIT_OBJECT_0) {
        DWORD exitCode;
        if (GetExitCodeProcess(m_hProcess, &exitCode)) {
            return static_cast<int>(exitCode);
        }
    }

    return -1;
}

bool ConPTY::resize(const TerminalSize& size) {
    if (!m_hPC) {
        set_error("PTY not initialized");
        return false;
    }

    COORD newSize;
    newSize.X = static_cast<SHORT>(size.cols);
    newSize.Y = static_cast<SHORT>(size.rows);

    HRESULT hr = ResizePseudoConsole(m_hPC, newSize);

    if (FAILED(hr)) {
        std::stringstream ss;
        ss << "ResizePseudoConsole failed with HRESULT 0x" << std::hex << hr;
        set_error(ss.str());
        return false;
    }

    return true;
}

void ConPTY::cleanup() {
    if (m_startupInfo.lpAttributeList) {
        DeleteProcThreadAttributeList(m_startupInfo.lpAttributeList);
        m_startupInfo.lpAttributeList = nullptr;
    }
    m_attributeList.reset();

    if (m_hThread) {
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    if (m_hProcess) {
        CloseHandle(m_hProcess);
        m_hProcess = nullptr;
    }
    if (m_hJob) {
        CloseHandle(m_hJob);
        m_hJob = nullptr;
    }

    if (m_hPC) {
        ClosePseudoConsole(m_hPC);
        m_hPC = nullptr;
    }

    if (m_hPipeIn) {
        CloseHandle(m_hPipeIn);
        m_hPipeIn = nullptr;
    }
    if (m_hPipeOut) {
        CloseHandle(m_hPipeOut);
        m_hPipeOut = nullptr;
    }
    if (m_hPipePTYIn) {
        CloseHandle(m_hPipePTYIn);
        m_hPipePTYIn = nullptr;
    }
    if (m_hPipePTYOut) {
        CloseHandle(m_hPipePTYOut);
        m_hPipePTYOut = nullptr;
    }

    ZeroMemory(&m_processInfo, sizeof(m_processInfo));
    ZeroMemory(&m_startupInfo, sizeof(m_startupInfo));
}

HeadlessTTY::~HeadlessTTY() {
    stop();
}

bool HeadlessTTY::start(const Config& config) {
    // m_config = config;  // Unused
    m_pty = std::make_unique<ConPTY>();

    if (!m_pty->initialize(config.size)) {
        return false;
    }

    if (!m_pty->spawn(config.command, config.args, config.working_dir)) {
        return false;
    }

    m_pty->start_reading();
    return true;
}

bool HeadlessTTY::write(const std::string& input) {
    if (!m_pty) return false;
    return m_pty->write(input);
}

bool HeadlessTTY::write(const uint8_t* data, size_t length) {
    if (!m_pty) return false;
    return m_pty->write(data, length);
}

void HeadlessTTY::set_output_callback(OutputCallback callback) {
    if (m_pty) {
        m_pty->set_output_callback(std::move(callback));
    }
}

void HeadlessTTY::stop() {
    if (m_pty) {
        m_pty->stop();
    }
}

bool HeadlessTTY::is_running() const {
    return m_pty && m_pty->is_running();
}

int HeadlessTTY::wait(DWORD timeout_ms) {
    if (!m_pty) return -1;
    return m_pty->wait(timeout_ms);
}

std::string HeadlessTTY::get_last_error() const {
    if (!m_pty) return "PTY not initialized";
    return m_pty->get_last_error();
}

} // namespace headless_tty
