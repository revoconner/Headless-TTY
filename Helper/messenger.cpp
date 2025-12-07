/* 
Messenger v4.0 - Authenticated Input Injection
 *
This tiny executable file must be spawned with UAC elevation. 
Mostly needed if you want an "ink framework" app such as gemini cli or claude code, 
    you will need their pid and this to send message.
*
Such apps, while will get the message from emulator,
    but won't process return key to send. .
*
SECURITY:::
Send text and special keys to Node.js/Ink apps (Claude CLI, Gemini, etc.)
with HMAC-SHA256 authentication to prevent unauthorized use.
 *
Authentication Flow:
1. Server creates named pipe with session secret [Server implementation not included.]
2. This exe connects to pipe, receives secret + target binding
3. Verifies HMAC signature from command line args
4. Verifies target PID matches registered target
5. Only then proceeds with injection
 *
Security Implementation:
1. HMAC SHA256 - Using windows bcrypt
2. 10 second replay protection
3. Target binding with PID+process name verification
4. Const time compare to prevent timing attac
5. Secret from pipe not hardcoded. I recommend implementing per session refresh.
 *
Build: g++ -o messenger.exe messenger.cpp -static -s -mwindows -lbcrypt
 *
Usage:
  messenger.exe <PID> <command> <timestamp> <sig>  (text injection)
 :: Sends enter automatically with a combo approach that works. 
 :: Just sending VK_RETURN, or \n or anything else doesn't. 
 */

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <ctime>
#include <cstdio>

#pragma comment(lib, "bcrypt.lib")

// Return Codes
const int SUCCESS = 0;
const int ERR_USAGE = 1;
const int ERR_AUTH_FAILED = 401;
const int ERR_TARGET_MISMATCH = 403;
const int ERR_TIMESTAMP_EXPIRED = 408;
const int ERR_PIPE_NOT_FOUND = 503;
const int ERR_ATTACH_FAILED = 2;

// Constants, use random uuid at runtime with another thing like server PID
const int TIMESTAMP_WINDOW_SECONDS = 10;
const char* DEFAULT_PIPE_NAME = "\\\\.\\pipe\\InjectorAuth";

// Auth Payload Structure
struct AuthPayload {
    std::string secret;
    DWORD target_pid;
    std::string target_name;
    bool valid;
};

// Helper: Process escape sequences (e.g. \t, \n)
std::string ProcessEscapeSequences(const std::string& input) {
    std::string result;
    for (size_t i = 0; i < input.length(); i++) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            switch (input[i + 1]) {
                case 'n':  result += '\n'; i++; break;
                case 'r':  result += '\r'; i++; break;
                case 't':  result += '\t'; i++; break;
                case '\\': result += '\\'; i++; break;
                default:   result += input[i]; break;
            }
        } else {
            result += input[i];
        }
    }
    return result;
}

// Auth: Get pipe name from file
std::string GetPipeName() {
    char localAppData[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        return DEFAULT_PIPE_NAME;
    }

    std::string pipePath = std::string(localAppData) + "\\ClaudeInjector\\pipe_name.txt";

    std::ifstream file(pipePath);
    if (!file.is_open()) {
        return DEFAULT_PIPE_NAME;
    }

    std::string pipeName;
    std::getline(file, pipeName);
    file.close();

    return pipeName.empty() ? DEFAULT_PIPE_NAME : pipeName;
}

// Auth: Read auth payload from named pipe
AuthPayload ReadAuthFromPipe(int* error_code) {
    AuthPayload result = {"", 0, "", false};
    *error_code = ERR_AUTH_FAILED;  // Default to auth failure
    
    std::string pipeName = GetPipeName();
    
    HANDLE pipe = CreateFileA(
        pipeName.c_str(),
        GENERIC_READ,
        0,
        NULL,
        OPEN_EXISTING,
        0,
        NULL
    );

    if (pipe == INVALID_HANDLE_VALUE) {
        *error_code = ERR_PIPE_NOT_FOUND;  // 503
        return result;
    }

    char buffer[512] = {0};
    DWORD bytesRead;
    BOOL readResult = ReadFile(pipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL);
    CloseHandle(pipe);

    if (!readResult || bytesRead == 0) {
        return result;  
    }

    std::string payload(buffer, bytesRead);

    if (payload == "REJECTED" || payload == "NO_TARGET") {
        return result;  
    }

    size_t pos1 = payload.find('\n');
    size_t pos2 = payload.find('\n', pos1 + 1);

    if (pos1 == std::string::npos || pos2 == std::string::npos) {
        return result;  // default 401
    }

    result.secret = payload.substr(0, pos1);
    result.target_pid = std::stoul(payload.substr(pos1 + 1, pos2 - pos1 - 1));
    result.target_name = payload.substr(pos2 + 1);
    result.valid = true;
    
    *error_code = 0;  // Success

    return result;
}

// Auth: Get process name by PID
std::string GetProcessName(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return "";
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(pe32);

    std::string result = "";
    if (Process32First(snapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == pid) {
                result = pe32.szExeFile;
                break;
            }
        } while (Process32Next(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return result;
}

// Auth: Verify target PID and process name
bool VerifyTarget(DWORD requestedPid, const AuthPayload& auth) {
    // Check 1: Requested PID matches registered target
    if (requestedPid != auth.target_pid) {
        return false;
    }

    // Check 2: Process at that PID has expected name
    std::string actualName = GetProcessName(requestedPid);

    // Case-insensitive comparison
    if (_stricmp(actualName.c_str(), auth.target_name.c_str()) != 0) {
        return false;
    }

    return true;
}

// Auth: Verify timestamp is within acceptable window
bool VerifyTimestamp(const std::string& timestamp) {
    int64_t ts = std::stoll(timestamp);
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t age = now - ts;

    return age >= 0 && age <= TIMESTAMP_WINDOW_SECONDS;
}

// Auth: Decode hex string to raw bytes
std::vector<uint8_t> HexDecode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;  // Invalid hex length

    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        unsigned int byte;
        if (sscanf(hex.c_str() + i, "%02x", &byte) != 1) {
            return std::vector<uint8_t>();  // Parse error
        }
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    return bytes;
}

// Auth: Compute HMAC-SHA256
std::string ComputeHMAC(const std::vector<uint8_t>& key, const std::string& message) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    UCHAR hash[32];
    //std::string result;

    // Open algorithm provider with HMAC flag
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlg,
        BCRYPT_SHA256_ALGORITHM,
        NULL,
        BCRYPT_ALG_HANDLE_HMAC_FLAG
    );

    if (!BCRYPT_SUCCESS(status)) {
        return "";
    }

    // Create hash with secret key
    status = BCryptCreateHash(
        hAlg,
        &hHash,
        NULL,
        0,
        (PUCHAR)key.data(),
        (ULONG)key.size(),
        0
    );

    if (!BCRYPT_SUCCESS(status)) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    // Hash the message
    status = BCryptHashData(
        hHash,
        (PUCHAR)message.data(),
        (ULONG)message.size(),
        0
    );

    if (!BCRYPT_SUCCESS(status)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }

    // Finish hash
    status = BCryptFinishHash(hHash, hash, 32, 0);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (!BCRYPT_SUCCESS(status)) {
        return "";
    }

    // Convert to hex string
    std::stringstream ss;
    for (int i = 0; i < 32; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    }

    return ss.str();
}

// Auth: Constant-time string comparison (prevents timing attacks)
bool ConstantTimeCompare(const std::string& a, const std::string& b) {
    volatile int result = a.size() ^ b.size();  // XOR lengths first to prevent leak although
    // it'll always be 64 char anyways
    
    size_t len = (a.size() < b.size()) ? a.size() : b.size();
    for (size_t i = 0; i < len; i++) {
        result |= a[i] ^ b[i];
    }
    return result == 0;
}

// Input Injection: Send Return/Enter Key
void SendRawModeEnter(HANDLE hConsoleInput) {
    INPUT_RECORD records[2] = {0};

    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    records[0].Event.KeyEvent.wVirtualScanCode = 0x1C;
    records[0].Event.KeyEvent.uChar.AsciiChar = '\r';
    records[0].Event.KeyEvent.dwControlKeyState = 0;

    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    WriteConsoleInput(hConsoleInput, records, 2, &written);
}

// Input Injection: Send Tab Key
void SendRawModeTab(HANDLE hConsoleInput) {
    INPUT_RECORD records[2] = {0};

    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_TAB;
    records[0].Event.KeyEvent.wVirtualScanCode = 0x0F;
    records[0].Event.KeyEvent.uChar.AsciiChar = '\t';
    records[0].Event.KeyEvent.dwControlKeyState = 0;

    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    WriteConsoleInput(hConsoleInput, records, 2, &written);
}

// Input Injection: Send Escape Key
void SendRawModeEscape(HANDLE hConsoleInput) {
    INPUT_RECORD records[2] = {0};

    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_ESCAPE;
    records[0].Event.KeyEvent.wVirtualScanCode = 0x01;
    records[0].Event.KeyEvent.uChar.AsciiChar = 0x1B;
    records[0].Event.KeyEvent.dwControlKeyState = 0;

    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    WriteConsoleInput(hConsoleInput, records, 2, &written);
}

// Input Injection: Send Shift Key
void SendRawModeShift(HANDLE hConsoleInput, bool keyDown) {
    INPUT_RECORD record = {0};

    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = keyDown ? TRUE : FALSE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = VK_LSHIFT;
    record.Event.KeyEvent.wVirtualScanCode = 0x2A;
    record.Event.KeyEvent.uChar.AsciiChar = 0;
    record.Event.KeyEvent.dwControlKeyState = keyDown ? SHIFT_PRESSED : 0;

    DWORD written;
    WriteConsoleInput(hConsoleInput, &record, 1, &written);
}

// Input Injection: Send Standard Text
void SendText(HANDLE hConsoleInput, const std::string& text) {
    std::vector<INPUT_RECORD> buffer;

    for (char c : text) {
        INPUT_RECORD irDown = {0};
        irDown.EventType = KEY_EVENT;
        irDown.Event.KeyEvent.bKeyDown = TRUE;
        irDown.Event.KeyEvent.wRepeatCount = 1;
        irDown.Event.KeyEvent.uChar.AsciiChar = c;
        irDown.Event.KeyEvent.dwControlKeyState = 0;

        SHORT vk = VkKeyScanA(c);
        if (vk != -1) {
            irDown.Event.KeyEvent.wVirtualKeyCode = LOBYTE(vk);
            irDown.Event.KeyEvent.wVirtualScanCode = MapVirtualKeyA(LOBYTE(vk), MAPVK_VK_TO_VSC);

            if (HIBYTE(vk) & 1) {
                irDown.Event.KeyEvent.dwControlKeyState |= SHIFT_PRESSED;
            }
        }

        INPUT_RECORD irUp = irDown;
        irUp.Event.KeyEvent.bKeyDown = FALSE;

        buffer.push_back(irDown);
        buffer.push_back(irUp);
    }

    if (!buffer.empty()) {
        DWORD written;
        WriteConsoleInput(hConsoleInput, buffer.data(), (DWORD)buffer.size(), &written);
    }
}

// Print Usage
void PrintUsage() {
    std::cout << "Messenger v4.0 - Authenticated Input Injection" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage:" << std::endl;
    std::cout << "  messenger.exe <PID> <command> <timestamp> <sig>" << std::endl;
    std::cout << std::endl;
    std::cout << "All commands require HMAC-SHA256 authentication." << std::endl;
    std::cout << std::endl;
    std::cout << "Special Commands:" << std::endl;
    std::cout << "  --enter        Send Enter key" << std::endl;
    std::cout << "  --tab          Send Tab key" << std::endl;
    std::cout << "  --escape       Send Escape key" << std::endl;
    std::cout << "  --shift-down   Press Shift key down" << std::endl;
    std::cout << "  --shift-up     Release Shift key" << std::endl;
    std::cout << std::endl;
    std::cout << "Text Injection:" << std::endl;
    std::cout << "  <text>         Send text followed by Enter" << std::endl;
    std::cout << std::endl;
    std::cout << "Arguments:" << std::endl;
    std::cout << "  PID            Target process ID" << std::endl;
    std::cout << "  command        Text or special command (--enter, etc.)" << std::endl;
    std::cout << "  timestamp      Unix timestamp from Python" << std::endl;
    std::cout << "  sig            HMAC-SHA256(secret, PID|command|timestamp)" << std::endl;
    std::cout << std::endl;
    std::cout << "Return Codes:" << std::endl;
    std::cout << "  0    Success" << std::endl;
    std::cout << "  1    Usage error" << std::endl;
    std::cout << "  2    AttachConsole failed" << std::endl;
    std::cout << "  401  Authentication failed" << std::endl;
    std::cout << "  403  Target PID/name mismatch" << std::endl;
    std::cout << "  408  Timestamp expired" << std::endl;
    std::cout << "  503  Auth pipe not found" << std::endl;
}

//()()()()()()()//
int main(int argc, char* const argv[]) { // Added const for linter to shut up
    // Argument parsing requires 4 args
    if (argc < 5) {
        PrintUsage();
        return ERR_USAGE;
    }

    DWORD targetPID = std::atoi(argv[1]);
    std::string command = ProcessEscapeSequences(argv[2]);
    std::string timestamp = argv[3];
    std::string providedSig = argv[4];

    // All commands require authentication (including special keys)
    // Step 1: Verify timestamp freshness (before pipe connection)
    if (!VerifyTimestamp(timestamp)) {
        return ERR_TIMESTAMP_EXPIRED;
    }

    // Step 2: Get auth payload from pipe
    // This also triggers server-side binary verification [SHA256 of this compiled binary]
    int authError = 0;
    AuthPayload auth = ReadAuthFromPipe(&authError);
    if (!auth.valid) {
        return authError;  // Returns 503 or 401 depending on failure type
    }

    // Step 3: Verify target binding (PID + process name)
    if (!VerifyTarget(targetPID, auth)) {
        return ERR_TARGET_MISMATCH;
    }

    // Step 4: Verify HMAC signature
    // Decode hex-encoded secret to raw bytes (Python sends hex, uses raw for HMAC)
    std::vector<uint8_t> secretBytes = HexDecode(auth.secret);
    if (secretBytes.empty()) {
        return ERR_AUTH_FAILED;  // Invalid secret format
    }

    std::string message = std::to_string(targetPID) + "|" + command + "|" + timestamp;
    std::string expectedSig = ComputeHMAC(secretBytes, message);

    if (expectedSig.empty() || !ConstantTimeCompare(expectedSig, providedSig)) {
        return ERR_AUTH_FAILED;
    }

    // --- Authentication passed, proceed with injection ---

    // Attach console (hide immediately when running without console)
    if (AllocConsole()) {
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    FreeConsole();

    // Attach to target console
    if (!AttachConsole(targetPID)) {
        return ERR_ATTACH_FAILED;
    }

    // Get input handle
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE) {
        FreeConsole();
        return ERR_ATTACH_FAILED;
    }

    // Execute command
    if (command == "--enter") {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeEnter(hStdIn);
    }
    else if (command == "--tab") {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeTab(hStdIn);
    }
    else if (command == "--escape") {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeEscape(hStdIn);
    }
    else if (command == "--shift-down") {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeShift(hStdIn, true);
    }
    else if (command == "--shift-up") {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeShift(hStdIn, false);
    }
    else {
        // Text injection
        SendText(hStdIn, command);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeEnter(hStdIn);
    }

    FreeConsole();
    return SUCCESS;
}
