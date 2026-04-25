cmake_minimum_required(VERSION 3.20)
project(wineopenxr_pe C)

add_library(wineopenxr SHARED
    openxr_loader.c
    loader_thunks.c
    "${WINEOPENXR_DEF}"
)

# Do not add the wine submodule's include tree here. Its windows.h and
# windef.h would shadow MinGW's and break d3d11/dxgi that MinGW needs.
# The vendored wine/debug.h under PROJECT_INCLUDE_DIR is enough
target_include_directories(wineopenxr PRIVATE
    "${PROJECT_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${OPENXR_SDK_INCLUDE_DIR}"
)

# UCRT avoids a msvcrt/ucrt FILE* struct mismatch across the PE boundary
target_compile_options(wineopenxr PRIVATE
    -Wall -Wextra -Wformat
    -mcrtdll=ucrt
    -DCOBJMACROS
    -DWINE_NO_LONG_TYPES
)

# --exclude-all-symbols suppresses MinGW's auto-export. The .def positional
# input re-adds exactly one symbol. --kill-at is a no-op on x86_64 stdcall
# (no @N decorations) but matches the idiom across Wine's other PE builtins
target_link_options(wineopenxr PRIVATE
    -mcrtdll=ucrt
    "LINKER:--exclude-all-symbols"
    "LINKER:--kill-at"
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(wineopenxr PRIVATE -flto)
    target_link_options(wineopenxr PRIVATE -s -flto)
endif()

target_link_libraries(wineopenxr PRIVATE "${WINE_NTDLL_LIB}")

set_target_properties(wineopenxr PROPERTIES
    PREFIX ""
    OUTPUT_NAME "wineopenxr"
    SUFFIX ".dll"
)

# Write the 32-byte "Wine builtin DLL" signature into the PE header so
# WINEDLLOVERRIDES="wineopenxr=b" picks us up. See scripts/sign_builtin.py
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET wineopenxr POST_BUILD
    COMMAND ${Python3_EXECUTABLE} "${SIGN_BUILTIN_SCRIPT}" "$<TARGET_FILE:wineopenxr>"
    COMMENT "Signing wineopenxr.dll as Wine builtin"
    VERBATIM
)
