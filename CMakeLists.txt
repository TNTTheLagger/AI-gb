cmake_minimum_required(VERSION 3.10)
project(gbcpu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- the emulator ------------------------------------------------------
# The same source list builds on every platform; platform_posix.c and
# platform_win32.c each compile to nothing when they are not the host.
add_executable(gbcpu
    src/main.c
    src/gb.c
    src/cpu.c
    src/memory.c
    src/io.c
    src/ppu.c
    src/apu.c
    src/timer.c
    src/joypad.c
    src/serial.c
    src/cart.c
    src/frontend_term.c
    src/platform_posix.c
    src/platform_win32.c
    src/platform_keys.c
    src/keymap.c
    src/layout.c
)

target_include_directories(gbcpu PRIVATE src)

# --- the unit tests ----------------------------------------------------
enable_testing()

add_executable(gbtests
    tests/main.c
    tests/fixture.c
    tests/test_cpu.c
    tests/test_timings.c
    tests/test_memory.c
    tests/test_cart.c
    tests/test_timer.c
    tests/test_ppu.c
    tests/test_apu.c
    tests/test_input.c
    tests/test_state.c
    tests/test_platform.c
    tests/test_layout.c
    tests/test_integration.c
    src/gb.c
    src/cpu.c
    src/memory.c
    src/io.c
    src/ppu.c
    src/apu.c
    src/timer.c
    src/joypad.c
    src/serial.c
    src/cart.c
    src/platform_posix.c
    src/platform_win32.c
    src/platform_keys.c
    src/keymap.c
    src/layout.c
)

target_include_directories(gbtests PRIVATE src tests)

# Multi-config generators (Visual Studio) need to be told which build to
# run the test in.
add_test(NAME unit COMMAND gbtests)
set_tests_properties(unit PROPERTIES
    CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel")

# --- platform tweaks ---------------------------------------------------
if(MSVC)
    # /wd4996: we use our own snprintf, not the deprecated CRT one
    target_compile_options(gbcpu PRIVATE /W3 /wd4996)
    target_compile_options(gbtests PRIVATE /W3 /wd4996)
    # Console subsystem, and make sure the CRT matches the runtime the
    # emulator is launched with.
    set_target_properties(gbcpu gbtests PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
else()
    target_compile_options(gbcpu PRIVATE -Wall -Wextra)
    target_compile_options(gbtests PRIVATE -Wall -Wextra)

    # libm is a separate library on most Unix systems, but is folded into
    # the C runtime on macOS and on Windows.
    if(NOT APPLE AND NOT WIN32)
        find_library(M_LIB m)
        if(M_LIB)
            target_link_libraries(gbcpu PRIVATE ${M_LIB})
            target_link_libraries(gbtests PRIVATE ${M_LIB})
        endif()
    endif()

    # clock_gettime lives in librt on glibc older than 2.17.
    include(CheckLibraryExists)
    check_library_exists(rt clock_gettime "" HAVE_CLOCK_GETTIME_IN_RT)
    if(HAVE_CLOCK_GETTIME_IN_RT)
        target_link_libraries(gbcpu PRIVATE rt)
        target_link_libraries(gbtests PRIVATE rt)
    endif()
endif()

# --- install -----------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS gbcpu RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
