cmake_minimum_required(VERSION 3.16)
project(headless-tty VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Platform check - ConPTY is Windows only
if(NOT WIN32)
    message(FATAL_ERROR "headless-tty currently only supports Windows (ConPTY)")
endif()

# Library sources
set(LIB_SOURCES
    src/pty.cpp
    src/session.cpp
)

set(LIB_HEADERS
    include/headless_tty/pty.hpp
    include/headless_tty/types.hpp
    include/headless_tty/protocol.hpp
    include/headless_tty/ring.hpp
    include/headless_tty/session.hpp
)

# Create the library
add_library(headless-tty-lib STATIC ${LIB_SOURCES} ${LIB_HEADERS})
target_include_directories(headless-tty-lib PUBLIC include)
target_link_libraries(headless-tty-lib PUBLIC advapi32)

# CLI executable
add_executable(headless-tty src/main.cpp)
target_link_libraries(headless-tty PRIVATE headless-tty-lib)

# Attach client, a console program unlike the server
add_executable(htty-client src/client.cpp)
target_include_directories(htty-client PRIVATE include)

# Install targets
install(TARGETS headless-tty-lib
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY include/headless_tty DESTINATION include)
install(TARGETS headless-tty htty-client RUNTIME DESTINATION bin)
