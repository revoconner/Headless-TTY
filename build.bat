@echo off
cd /d "%~dp0"

echo Compiling resources...
llvm-rc /fo resources/app.res resources/app.rc
if %ERRORLEVEL% NEQ 0 (
    echo Resource compilation failed
    exit /b 1
)

echo Building server...
clang++ -O3 -Wall -Wextra -std=c++17 -fno-exceptions -I include -o htty-server.exe src/pty.cpp src/session.cpp src/main.cpp resources/app.res -static -ladvapi32 -Wl,/SUBSYSTEM:WINDOWS -Wl,/ENTRY:mainCRTStartup
if %ERRORLEVEL%==0 echo Build successful

echo Building front end...
clang++ -O3 -Wall -Wextra -std=c++17 -fno-exceptions -I include -o headless-tty.exe src/htty.cpp src/client.cpp resources/app.res -static
if %ERRORLEVEL%==0 echo Build successful