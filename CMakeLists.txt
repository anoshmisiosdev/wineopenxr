cmake_minimum_required(VERSION 3.20)
project(wineopenxr C OBJC)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

set(WINE_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/extern/wine/include")

set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "" FORCE)
add_subdirectory(extern/OpenXR-SDK)
message(STATUS "OpenXR SDK: ${CMAKE_SOURCE_DIR}/extern/OpenXR-SDK")

set(MINGW_PREFIX "x86_64-w64-mingw32" CACHE STRING "MinGW cross-compiler prefix")
find_program(MINGW_CC "${MINGW_PREFIX}-gcc" REQUIRED)
find_program(MINGW_DLLTOOL "${MINGW_PREFIX}-dlltool" REQUIRED)
message(STATUS "MinGW CC: ${MINGW_CC}")

set(WINE_NTDLL_LIB "${CMAKE_BINARY_DIR}/libntdll.a")
add_custom_command(
    OUTPUT "${WINE_NTDLL_LIB}"
    COMMAND "${MINGW_DLLTOOL}" -d "${CMAKE_SOURCE_DIR}/cmake/ntdll.def" -l "${WINE_NTDLL_LIB}"
    DEPENDS "${CMAKE_SOURCE_DIR}/cmake/ntdll.def"
    VERBATIM
)
add_custom_target(libntdll DEPENDS "${WINE_NTDLL_LIB}")

add_subdirectory(src/unix)

include(ExternalProject)
ExternalProject_Add(wineopenxr_pe
    SOURCE_DIR "${CMAKE_SOURCE_DIR}/src/pe"
    BINARY_DIR "${CMAKE_BINARY_DIR}/src/pe"
    DEPENDS libntdll
    CMAKE_ARGS
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_SOURCE_DIR}/cmake/x86_64-w64-mingw32.cmake
        -DWINE_NTDLL_LIB=${WINE_NTDLL_LIB}
        -DSIGN_BUILTIN_SCRIPT=${CMAKE_SOURCE_DIR}/scripts/sign_builtin.py
        -DPROJECT_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/src/include
        -DOPENXR_SDK_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/extern/OpenXR-SDK/include
        -DWINEOPENXR_DEF=${CMAKE_SOURCE_DIR}/wineopenxr.def
    BUILD_BYPRODUCTS "${CMAKE_BINARY_DIR}/src/pe/wineopenxr.dll"
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND ""
)
