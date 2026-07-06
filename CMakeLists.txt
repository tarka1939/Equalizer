cmake_minimum_required(VERSION 3.20)
project(Equalizer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
# eq-daemon executable
# ──────────────────────────────────────────────────────────────────────────────
add_subdirectory(daemon)

# ──────────────────────────────────────────────────────────────────────────────
# WavEqTest standalone tool
# ──────────────────────────────────────────────────────────────────────────────
add_executable(WavEqTest Tools/WavEqTest.cpp)
target_link_libraries(WavEqTest PRIVATE dsp)
target_include_directories(WavEqTest PRIVATE DSP)
