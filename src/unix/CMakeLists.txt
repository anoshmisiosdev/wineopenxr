add_library(wineopenxr_unix SHARED
    openxr.c
    openxr_thunks.c
    session.m
    metal.m
)

target_include_directories(wineopenxr_unix PRIVATE
    "${CMAKE_SOURCE_DIR}/src/include"
    "${WINE_INCLUDE_DIR}"
    "${CMAKE_SOURCE_DIR}/extern/OpenXR-SDK/include"
)

target_compile_options(wineopenxr_unix PRIVATE
    -Wall -Wextra -Wformat -Werror=implicit-function-declaration
    -DWINE_NO_LONG_TYPES
    -D__WINESRC__
    -DWINE_UNIX_LIB
)

# __wine_dbg_header, __wine_dbg_output, and NtQueryPerformanceCounter resolve
# at runtime from ntdll.so, already loaded by Wine's unix call dispatcher
target_link_options(wineopenxr_unix PRIVATE
    "LINKER:-U,___wine_dbg_header"
    "LINKER:-U,___wine_dbg_output"
    "LINKER:-U,_NtQueryPerformanceCounter"
)

target_link_libraries(wineopenxr_unix PRIVATE
    openxr_loader
    c++
    "-framework Foundation"
    "-framework Metal"
    "-framework IOSurface"
)

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_link_options(wineopenxr_unix PRIVATE -Wl,-S)
endif()

set_target_properties(wineopenxr_unix PROPERTIES
    OUTPUT_NAME "wineopenxr"
    SUFFIX ".so"
    PREFIX ""
    OSX_ARCHITECTURES "x86_64"
)
