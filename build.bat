@echo off
cd /d "%~dp0"

echo Compiling resources...
llvm-rc /fo resources/app.res resources/app.rc
if %ERRORLEVEL% NEQ 0 (
    echo Resource compilation failed
    exit /b 1
)

echo Building executable...
clang++ -O3 -Wall -Wextra -std=c++17 -fno-exceptions -I include -o headless-tty.exe src/pty.cpp src/main.cpp resources/app.res -static -luser32 -lshell32 -Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup

if %ERRORLEVEL%==0 echo Build successful

@REM echo Building helper...
@REM g++ -std=c++23 -o messenger.exe Helper/messenger.cpp -static -s -mwindows -lbcrypt
@REM if %ERRORLEVEL%==0 echo Build successful