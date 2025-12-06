/* this tiny executable file must be spawned with UAC elevation. 
 mostly needed if you want an "ink framework" app such as gemini cli or claude code, you will need their pid and this to send message
Such apps while will get the message from emulator but won't process return key to end. 
 messenger intended_pid "hello world"
 sends enter automatically after text. 

*/

#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>


// Why this at all?
// Node.js (libuv or ink framework apps on Windows in Raw Mode requires a "Perfect" Input Record. Claude and some other apps like gemini runs like this. 
//If any component (VK, ScanCode, Char) is missing, it treats it as text data, not a control action. This means a simple character injections system will not work.

// HELPER: Process command line escape sequences (e.g. \t)

std::string ProcessEscapeSequences(const std::string& input) {
    std::string result;
    for (size_t i = 0; i < input.length(); i++) {
        if (input[i] == '\\' && i + 1 < input.length()) {
            switch (input[i + 1]) {
                case 'n':  result += '\n'; i++; break; // Line Feed
                case 'r':  result += '\r'; i++; break; // Carriage Return
                case 't':  result += '\t'; i++; break; // Tab
                case '\\': result += '\\'; i++; break; // Literal Backslash
                default:   result += input[i]; break;
            }
        } else {
            result += input[i];
        }
    }
    return result;
}

// CORE LOGIC: Send Special Keys for Node.js/Ink

// Send Return/Enter Key
void SendRawModeEnter(HANDLE hConsoleInput) {

    INPUT_RECORD records[2] = {0};

    // key down event
    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;

    //The Virtual Key (Signal to OS)
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;

    //The Hardware Scan Code (Signal to Node.js TTY adapter)
    // 0x1C is the standard "Enter" key. 0xE01C is the Numpad Enter.
    records[0].Event.KeyEvent.wVirtualScanCode = 0x1C;

    //The Character (Signal to the Data Stream)
    // Must be '\r' (13). If 0, Node ignores it. If '\n', Node treats as Ctrl+J.
    records[0].Event.KeyEvent.uChar.AsciiChar = '\r';

    records[0].Event.KeyEvent.dwControlKeyState = 0;

    // key ip event
    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    if (!WriteConsoleInput(hConsoleInput, records, 2, &written)) {
        // If writing fails, we can't do much since stdout is detached, but we avoid crashing.
    }
}

// Send Tab Key
void SendRawModeTab(HANDLE hConsoleInput) {
    INPUT_RECORD records[2] = {0};

    // key down event
    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;

    // Virtual Key Code for Tab
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_TAB;

    // Scan Code for Tab (0x0F)
    records[0].Event.KeyEvent.wVirtualScanCode = 0x0F;

    // Character code for Tab ('\t' = 9)
    records[0].Event.KeyEvent.uChar.AsciiChar = '\t';

    records[0].Event.KeyEvent.dwControlKeyState = 0;

    // key ip event
    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    WriteConsoleInput(hConsoleInput, records, 2, &written);
}

// Send Escape Key
void SendRawModeEscape(HANDLE hConsoleInput) {
    INPUT_RECORD records[2] = {0};

    // key down event
    records[0].EventType = KEY_EVENT;
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[0].Event.KeyEvent.wRepeatCount = 1;

    // Virtual Key Code for Escape
    records[0].Event.KeyEvent.wVirtualKeyCode = VK_ESCAPE;

    // Scan Code for Escape (0x01)
    records[0].Event.KeyEvent.wVirtualScanCode = 0x01;

    // Character code for Escape (0x1B = 27)
    records[0].Event.KeyEvent.uChar.AsciiChar = 0x1B;

    records[0].Event.KeyEvent.dwControlKeyState = 0;

    // key ip event
    records[1] = records[0];
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    DWORD written;
    WriteConsoleInput(hConsoleInput, records, 2, &written);
}

// Send Shift Key (modifier key - sent as key down/up pair)
void SendRawModeShift(HANDLE hConsoleInput, bool keyDown) {
    INPUT_RECORD record = {0};

    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = keyDown ? TRUE : FALSE;
    record.Event.KeyEvent.wRepeatCount = 1;

    // Virtual Key Code for Left Shift
    record.Event.KeyEvent.wVirtualKeyCode = VK_LSHIFT;

    // Scan Code for Left Shift (0x2A)
    record.Event.KeyEvent.wVirtualScanCode = 0x2A;

    // Shift has no character value
    record.Event.KeyEvent.uChar.AsciiChar = 0;

    // Set shift state if key is down
    record.Event.KeyEvent.dwControlKeyState = keyDown ? SHIFT_PRESSED : 0;

    DWORD written;
    WriteConsoleInput(hConsoleInput, &record, 1, &written);
}

// Send Standard Text
void SendText(HANDLE hConsoleInput, const std::string& text) {
    std::vector<INPUT_RECORD> buffer;

    for (char c : text) {
        INPUT_RECORD irDown = {0};
        irDown.EventType = KEY_EVENT;
        irDown.Event.KeyEvent.bKeyDown = TRUE;
        irDown.Event.KeyEvent.wRepeatCount = 1;
        irDown.Event.KeyEvent.uChar.AsciiChar = c;
        irDown.Event.KeyEvent.dwControlKeyState = 0;

        // Attempt to map the char to a scan code to look "real"
        SHORT vk = VkKeyScanA(c);
        if (vk != -1) {
            irDown.Event.KeyEvent.wVirtualKeyCode = LOBYTE(vk);
            irDown.Event.KeyEvent.wVirtualScanCode = MapVirtualKeyA(LOBYTE(vk), MAPVK_VK_TO_VSC);

            // Handle Shift state if needed (simple implementation)
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

int main(int argc, char* argv[]) {
    //Argument Parsing
    if (argc < 3) {
        std::cout << "Ink Injector v2.0 - Send text and special keys to Node.js/Ink apps" << std::endl;
        std::cout << "\nUsage: ink_injector.exe <PID> <Command>" << std::endl;
        std::cout << "\nCommands:" << std::endl;
        std::cout << "  <text>         Send text followed by Enter key" << std::endl;
        std::cout << "  --enter        Send only Enter key" << std::endl;
        std::cout << "  --tab          Send only Tab key" << std::endl;
        std::cout << "  --escape       Send only Escape key" << std::endl;
        std::cout << "  --shift-down   Press Shift key down" << std::endl;
        std::cout << "  --shift-up     Release Shift key" << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  ink_injector.exe 1234 \"what is 2+2\"" << std::endl;
        std::cout << "  ink_injector.exe 1234 --escape" << std::endl;
        std::cout << "  ink_injector.exe 1234 --tab" << std::endl;
        return 1;
    }

    DWORD targetPID = std::atoi(argv[1]);
    std::string message = ProcessEscapeSequences(argv[2]);

    //attach console then hide immediately when running without console such as pythonw or compiled no debugger
    if (AllocConsole()) {
        // Hide the console window immediately to prevent flash
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    }
    // We cannot attach to a new console while attached to one. Maybe don't detach when running consecutive op on the same console? later implementations
    FreeConsole();

    //Attach to the target console - this works even if the target is 'GUI-less' inside Windows Terminal
    if (!AttachConsole(targetPID)) {
        // Capture error BEFORE AllocConsole overwrites it
        DWORD err = GetLastError();

        // Re-create a console to show the error
        AllocConsole();
        freopen("CONOUT$", "w", stdout);
        std::cerr << "[ERROR] AttachConsole failed. Error Code: " << err << std::endl;
        if (err == 5) std::cerr << "Hint: Access Denied. Run this tool as Administrator." << std::endl;
        if (err == 87) std::cerr << "Hint: PID invalid or process has no console." << std::endl;

        // Wait so user can read error
        Sleep(5000);
        return 1;
    }

    //Get the Input Handle of the *target* console
    HANDLE hStdIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdIn == INVALID_HANDLE_VALUE) {
        FreeConsole();
        return 1;
    }

    //Check for special key commands
    // Note: 50ms delay after AttachConsole allows console to stabilize
    if (message == "--enter") {
        // Send only Enter key
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeEnter(hStdIn);
    }
    else if (message == "--tab") {
        // Send only Tab key
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeTab(hStdIn);
    }
    else if (message == "--escape") {
        // Send only Escape key
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeEscape(hStdIn);
    }
    else if (message == "--shift-down") {
        // Send Shift key down
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeShift(hStdIn, true);
    }
    else if (message == "--shift-up") {
        // Send Shift key up
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        SendRawModeShift(hStdIn, false);
    }
    else {
        // Normal mode: Send text + Enter
        //  Send the Text
        SendText(hStdIn, message);

        //  Brief Sleep
        // This ensures Node.js processes the text buffer before receiving the submit signal.
        // 50ms is usually sufficient.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        //  Send the 'Raw Mode' Enter Key
        SendRawModeEnter(hStdIn);
    }

    //Cleanup
    FreeConsole();
    return 0;
}
