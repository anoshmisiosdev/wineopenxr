set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

find_program(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc     REQUIRED)
find_program(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++     REQUIRED)
find_program(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres REQUIRED)

# Anchor CMake's find_* at the MinGW sysroot so lookups do not escape to
# the host (darwin-arm64). Derive from the compiler so the toolchain works
# on homebrew /opt/homebrew/... and /usr/x86_64-w64-mingw32 alike
get_filename_component(MINGW_BIN_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
get_filename_component(MINGW_ROOT    "${MINGW_BIN_DIR}/.."  ABSOLUTE)
set(CMAKE_FIND_ROOT_PATH "${MINGW_ROOT}/${TOOLCHAIN_PREFIX}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Without this, CMake checks runtime deps against the host (darwin-arm64)
# and aborts on the PE binary with "file unknown"
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
