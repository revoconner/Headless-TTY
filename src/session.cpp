#include "headless_tty/session.hpp"

#include <sddl.h>

#include <algorithm>
#include <cstring>

namespace headless_tty {

namespace {

// ConPTY asks the terminal for win32 input mode at startup. A client terminal that honoured it would send every key as a long escape sequence and the detach key would never be seen, so it is removed from everything a client receives.
constexpr char WIN32_INPUT_MODE[] = "\x1b[?9001h";

// Terminal queries. Live they go to the attached client and its terminal answers. In a replay they are stale, and with nobody attached they are answered here.
constexpr char QUERY_DA1[] = "\x1b[c";
constexpr char QUERY_CPR[] = "\x1b[6n";
constexpr char REPLY_DA1[] = "\x1b[?1;0c";
constexpr char REPLY_CPR[] = "\x1b[1;1R";

// Removes every occurrence of seq and returns how many there were
size_t strip(std::vector<uint8_t>& buf, const char* seq) {
    size_t n = strlen(seq), count = 0;
    auto it = buf.begin();
    while ((it = std::search(it, buf.end(), seq, seq + n)) != buf.end()) {
        it = buf.erase(it, it + n);
        ++count;
    }
    return count;
}

// Length of the longest tail of buf that is a proper prefix of seq, so a sequence split across two reads is still caught
size_t partial_tail(const std::vector<uint8_t>& buf, const char* seq) {
    size_t n = strlen(seq);
    // Parenthesized so the min macro from windows.h does not expand here
    for (size_t k = (std::min)(n - 1, buf.size()); k > 0; --k) {
        if (memcmp(buf.data() + buf.size() - k, seq, k) == 0) return k;
    }
    return 0;
}

bool current_user(std::wstring& sid, bool& elevated) {
    HANDLE hToken = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;

    alignas(TOKEN_USER) uint8_t user[256];
    TOKEN_ELEVATION elevation = {};
    DWORD len = 0;
    bool ok = GetTokenInformation(hToken, TokenUser, user, sizeof(user), &len)
           && GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &len);
    CloseHandle(hToken);
    if (!ok) return false;

    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, &text)) return false;
    sid = text;
    LocalFree(text);
    elevated = elevation.TokenIsElevated != 0;
    return true;
}

}

Session::Session(HeadlessTTY& tty, const std::wstring& name, TerminalSize size, bool wait_for_client)
    : m_tty(tty), m_name(name), m_size(size), m_wait(wait_for_client) {}

Session::~Session() {
    stop();
    if (m_hPipe != INVALID_HANDLE_VALUE) CloseHandle(m_hPipe);
    for (HANDLE h : { m_hStop, m_hReadEvent, m_hWriteEvent, m_hQueueEvent }) {
        if (h) CloseHandle(h);
    }
}

bool Session::open() {
    std::wstring sid;
    bool elevated = false;
    if (!current_user(sid, elevated)) {
        m_last_error = "cannot read the current user from the process token";
        return false;
    }

    // Only this user may connect. An elevated server also refuses writers below high integrity, otherwise an ordinary process could type into an admin shell.
    std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    if (elevated) sddl += L"S:(ML;;NW;;;HI)";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sa.lpSecurityDescriptor, NULL)) {
        m_last_error = "cannot build the pipe security descriptor, error " + std::to_string(GetLastError());
        return false;
    }

    // First instance only, so a second server or a squatter on the same name fails here. A single instance makes a second client see ERROR_PIPE_BUSY.
    std::wstring path = std::wstring(PIPE_PREFIX) + m_name;
    m_hPipe = CreateNamedPipeW(path.c_str(),
                               PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                               PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                               1, 65536, 65536, 0, &sa);
    DWORD err = GetLastError();
    LocalFree(sa.lpSecurityDescriptor);

    if (m_hPipe == INVALID_HANDLE_VALUE) {
        m_last_error = err == ERROR_ACCESS_DENIED ? "session name is already in use"
                                                  : "cannot create the session pipe, error " + std::to_string(err);
        return false;
    }

    m_hStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_hReadEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_hWriteEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_hQueueEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    return true;
}

void Session::serve() {
    if (!m_thread.joinable()) m_thread = std::thread(&Session::serve_loop, this);
}

void Session::stop() {
    if (m_hStop) SetEvent(m_hStop);
    if (m_thread.joinable()) m_thread.join();
}

bool Session::stopping() const {
    return WaitForSingleObject(m_hStop, 0) == WAIT_OBJECT_0;
}

void Session::on_output(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<uint8_t> buf = std::move(m_held);
    m_held.clear();
    buf.insert(buf.end(), data, data + length);

    strip(buf, WIN32_INPUT_MODE);
    size_t hold = partial_tail(buf, WIN32_INPUT_MODE);
    m_held.assign(buf.end() - hold, buf.end());
    buf.resize(buf.size() - hold);

    if (!m_attached.load()) {
        // Newer conhost builds stall for seconds waiting on a DA1 answer, and so do some apps
        if (strip(buf, QUERY_DA1)) m_tty.write(REPLY_DA1);
        if (strip(buf, QUERY_CPR)) m_tty.write(REPLY_CPR);
    }
    if (buf.empty()) return;

    m_ring.push(buf.data(), buf.size());
    if (m_attached.load()) enqueue(FrameType::Data, buf.data(), buf.size());
}

void Session::finish(int exit_code) {
    bool drain;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_finished = true;
        m_exit_code = exit_code;
        if (m_attached.load()) enqueue(FrameType::Exit, &m_exit_code, sizeof(m_exit_code));
        drain = m_attached.load() || m_wait;
    }
    if (!drain) SetEvent(m_hStop);
    if (m_thread.joinable()) m_thread.join();
}

// Caller holds m_mutex
void Session::enqueue(FrameType type, const void* payload, size_t len) {
    if (m_queued_bytes + len > MAX_QUEUED_BYTES) {
        // The client is not keeping up. Drop it, the session carries on and the ring still has the output.
        m_attached.store(false);
        m_dropped = true;
        m_queue.clear();
        m_queued_bytes = 0;
        SetEvent(m_hQueueEvent);
        return;
    }

    std::vector<uint8_t> frame(FRAME_HEADER_SIZE + len);
    frame[0] = static_cast<uint8_t>(type);
    uint32_t n = static_cast<uint32_t>(len);
    memcpy(frame.data() + 1, &n, 4);
    if (len) memcpy(frame.data() + FRAME_HEADER_SIZE, payload, len);

    m_queued_bytes += frame.size();
    m_queue.push_back(std::move(frame));
    SetEvent(m_hQueueEvent);
}

void Session::serve_loop() {
    while (!stopping()) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_finished && (!m_wait || m_exit_delivered)) break;
        }
        if (wait_for_client()) serve_client();
        else if (!stopping()) Sleep(100);
        DisconnectNamedPipe(m_hPipe);
    }
}

bool Session::wait_for_client() {
    OVERLAPPED ov = {};
    ov.hEvent = m_hReadEvent;
    if (ConnectNamedPipe(m_hPipe, &ov)) return true;

    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return true;
    if (err != ERROR_IO_PENDING) return false;

    HANDLE waits[2] = { m_hReadEvent, m_hStop };
    if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) CancelIoEx(m_hPipe, &ov);
    DWORD n = 0;
    return GetOverlappedResult(m_hPipe, &ov, &n, TRUE) != 0;
}

// One read stays pending the whole time while queued output is written, which the overlapped pipe allows
void Session::serve_client() {
    OVERLAPPED ov = {};
    ov.hEvent = m_hReadEvent;
    uint8_t chunk[4096];
    std::vector<uint8_t> inbuf;
    bool pending = false, done = false;

    while (!done) {
        if (!pending) {
            if (!ReadFile(m_hPipe, chunk, sizeof(chunk), NULL, &ov) && GetLastError() != ERROR_IO_PENDING) break;
            pending = true;
        }

        HANDLE waits[3] = { m_hStop, m_hReadEvent, m_hQueueEvent };
        DWORD w = WaitForMultipleObjects(3, waits, FALSE, INFINITE);

        if (w == WAIT_OBJECT_0 + 1) {
            DWORD n = 0;
            pending = false;
            if (!GetOverlappedResult(m_hPipe, &ov, &n, FALSE) || n == 0) break;
            inbuf.insert(inbuf.end(), chunk, chunk + n);
            done = !handle_frames(inbuf);
        } else if (w == WAIT_OBJECT_0 + 2) {
            done = !flush_queue();
        } else {
            break;
        }
    }

    if (pending) {
        DWORD n = 0;
        CancelIoEx(m_hPipe, &ov);
        GetOverlappedResult(m_hPipe, &ov, &n, TRUE);
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_attached.store(false);
    m_dropped = false;
    m_queue.clear();
    m_queued_bytes = 0;
}

bool Session::handle_frames(std::vector<uint8_t>& buf) {
    size_t pos = 0;
    bool ok = true;

    while (ok && buf.size() - pos >= FRAME_HEADER_SIZE) {
        uint32_t len = 0;
        memcpy(&len, buf.data() + pos + 1, 4);
        if (len > MAX_FRAME_PAYLOAD) return false;
        if (buf.size() - pos < FRAME_HEADER_SIZE + len) break;

        ok = handle_frame(static_cast<FrameType>(buf[pos]), buf.data() + pos + FRAME_HEADER_SIZE, len);
        pos += FRAME_HEADER_SIZE + len;
    }

    buf.erase(buf.begin(), buf.begin() + pos);
    return ok;
}

// Returns false when the connection should end
bool Session::handle_frame(FrameType type, const uint8_t* payload, size_t len) {
    TerminalSize size;

    switch (type) {
        case FrameType::Hello:
            if (len < 5 || payload[0] != PROTOCOL_VERSION || m_attached.load()) return false;
            memcpy(&size.cols, payload + 1, 2);
            memcpy(&size.rows, payload + 3, 2);
            attach(size);
            return true;

        case FrameType::Data:
            // Input for a child that has exited is discarded, nothing would ever read it
            if (m_attached.load() && len && m_tty.is_running()) m_tty.write(payload, len);
            return true;

        case FrameType::Resize:
            if (m_attached.load() && len >= 4) {
                memcpy(&size.cols, payload, 2);
                memcpy(&size.rows, payload + 2, 2);
                apply_size(size);
            }
            return true;

        case FrameType::Detach:
            return false;

        default:
            // Unknown frames are skipped so a newer client still works
            return true;
    }
}

void Session::attach(TerminalSize size) {
    bool same_size = size.cols == m_size.cols && size.rows == m_size.rows;
    bool nudge = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::vector<uint8_t> replay = m_ring.snapshot();
        if (replay.size() == REPLAY_CAPACITY) {
            // The ring has wrapped, so its first bytes may be half an escape sequence. Start at a line boundary.
            auto nl = std::find(replay.begin(), replay.end(), '\n');
            replay.erase(replay.begin(), nl == replay.end() ? nl : nl + 1);
        }
        strip(replay, QUERY_DA1);
        strip(replay, QUERY_CPR);

        if (!replay.empty()) enqueue(FrameType::Replay, replay.data(), replay.size());
        m_attached.store(true);

        if (m_finished) enqueue(FrameType::Exit, &m_exit_code, sizeof(m_exit_code));
        else nudge = !replay.empty() && same_size;
    }

    apply_size(size);

    if (nudge) {
        // A replay of raw bytes cannot rebuild a full screen app. A size change makes the app repaint, so an unchanged size is bumped by a column and put back. Growing first never truncates a line.
        m_tty.resize({ static_cast<uint16_t>(size.cols + 1), size.rows });
        WaitForSingleObject(m_hStop, 150);
        m_tty.resize(size);
    }
}

void Session::apply_size(TerminalSize size) {
    if (!size.cols || !size.rows) return;
    m_size = size;
    m_tty.resize(size);
}

// Returns false when the connection should end
bool Session::flush_queue() {
    for (;;) {
        std::vector<uint8_t> frame;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_dropped) return false;
            if (m_queue.empty()) return true;
            frame = std::move(m_queue.front());
            m_queue.pop_front();
            m_queued_bytes -= frame.size();
        }

        if (!write_frame(frame)) return false;

        if (static_cast<FrameType>(frame[0]) == FrameType::Exit) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_exit_delivered = true;
            return false;
        }
    }
}

bool Session::write_frame(const std::vector<uint8_t>& frame) {
    OVERLAPPED ov = {};
    ov.hEvent = m_hWriteEvent;

    if (!WriteFile(m_hPipe, frame.data(), static_cast<DWORD>(frame.size()), NULL, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;

        // A client that stops reading is dropped, it must never hold up the session
        HANDLE waits[2] = { m_hWriteEvent, m_hStop };
        if (WaitForMultipleObjects(2, waits, FALSE, CLIENT_WRITE_TIMEOUT_MS) != WAIT_OBJECT_0) CancelIoEx(m_hPipe, &ov);
    }

    DWORD n = 0;
    return GetOverlappedResult(m_hPipe, &ov, &n, TRUE) && n == frame.size();
}

}
