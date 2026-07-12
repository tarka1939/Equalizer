cmake_minimum_required(VERSION 3.20)
project(Equalizer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ──────────────────────────────────────────────────────────────────────────────
# Testing
# ──────────────────────────────────────────────────────────────────────────────
option(EQUALIZER_BUILD_TESTS "Build unit test executables (dsp_tests, eq_state_tests, ipc_server_tests)" ON)
include(CTest)

# ──────────────────────────────────────────────────────────────────────────────
# Platform detection
# ──────────────────────────────────────────────────────────────────────────────
if(UNIX AND NOT APPLE)
    set(PLATFORM_LINUX TRUE)
elseif(APPLE)
    set(PLATFORM_MAC TRUE)
elseif(WIN32)
    set(PLATFORM_WINDOWS TRUE)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# DSP static library (platform-agnostic)
# ──────────────────────────────────────────────────────────────────────────────
add_library(dsp STATIC
    DSP/Biquad.cpp
    DSP/Biquad.h
    DSP/Equalizer10Band.h
    DSP/Equalizer10Band.inline.h
)
target_include_directories(dsp PUBLIC DSP)
target_compile_options(dsp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
)

# ──────────────────────────────────────────────────────────────────────────────
# DSP unit tests (Biquad + Equalizer10Band) — platform-agnostic, no PipeWire
# dependency, so these build even where eq-daemon itself cannot (e.g. no
# libpipewire-0.3-dev installed).
# ──────────────────────────────────────────────────────────────────────────────
if(EQUALIZER_BUILD_TESTS)
    add_executable(dsp_tests DSP/tests/test_biquad.cpp)
    target_link_libraries(dsp_tests PRIVATE dsp)
    target_compile_options(dsp_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
    )
    add_test(NAME dsp_tests COMMAND dsp_tests)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# eq-daemon executable
# ──────────────────────────────────────────────────────────────────────────────
add_subdirectory(daemon)

# ──────────────────────────────────────────────────────────────────────────────
# WavEqTest standalone tool
# ──────────────────────────────────────────────────────────────────────────────
add_executable(WavEqTest Tools/WavEqTest.cpp)
target_link_libraries(WavEqTest PRIVATE dsp)
target_include_directories(WavEqTest PRIVATE DSP)
