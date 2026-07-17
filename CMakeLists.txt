cmake_minimum_required(VERSION 3.20)
project(Equalizer VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ──────────────────────────────────────────────────────────────────────────────
# Testing
# ──────────────────────────────────────────────────────────────────────────────
option(EQUALIZER_BUILD_TESTS "Build unit test executables (dsp_tests, overlap_add_tests, eq_pipeline_tests, eq_state_tests, ipc_server_tests)" ON)
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
#
# KissFFT (vendored under Equalizer/kissfft-131.2.0/, BSD-3-Clause) is compiled
# straight into this library alongside DSP/OverlapAdd.{h,cpp}. It was already
# wired into the Windows APO project (Equalizer.vcxproj) but not into this
# cross-platform CMake build until the OLA (Overlap-Add) engine needed it here
# too. Only kiss_fft.c/kiss_fftr.c (the real-FFT subset OverlapAdd uses) are
# built -- not kiss_fftnd*/kfc, which this project doesn't need.
# ──────────────────────────────────────────────────────────────────────────────
set(KISSFFT_DIR ${CMAKE_SOURCE_DIR}/Equalizer/kissfft-131.2.0)

add_library(dsp STATIC
    DSP/Biquad.cpp
    DSP/Biquad.h
    DSP/Equalizer10Band.h
    DSP/Equalizer10Band.inline.h
    DSP/OverlapAdd.h
    DSP/OverlapAdd.cpp
    DSP/EqPipeline.h
    DSP/EqPipeline.cpp
    ${KISSFFT_DIR}/kiss_fft.c
    ${KISSFFT_DIR}/kiss_fft.h
    ${KISSFFT_DIR}/kiss_fftr.c
    ${KISSFFT_DIR}/kiss_fftr.h
)
target_include_directories(dsp PUBLIC DSP ${KISSFFT_DIR})
target_compile_options(dsp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
)
# kiss_fft.c/kiss_fftr.c are plain C89-ish C, not C++; silence warnings that
# don't apply to vendored third-party code (kept out of the -Wall/-Wextra
# blanket above, which is only meant for our own DSP/*.cpp).
set_source_files_properties(
    ${KISSFFT_DIR}/kiss_fft.c
    ${KISSFFT_DIR}/kiss_fftr.c
    PROPERTIES COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU,Clang>:-w>"
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

    add_executable(overlap_add_tests DSP/tests/test_overlap_add.cpp)
    target_link_libraries(overlap_add_tests PRIVATE dsp)
    target_compile_options(overlap_add_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
    )
    add_test(NAME overlap_add_tests COMMAND overlap_add_tests)

    add_executable(eq_pipeline_tests DSP/tests/test_eq_pipeline.cpp)
    target_link_libraries(eq_pipeline_tests PRIVATE dsp)
    target_compile_options(eq_pipeline_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O2>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /O2>
    )
    add_test(NAME eq_pipeline_tests COMMAND eq_pipeline_tests)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# eq-daemon executable
# ──────────────────────────────────────────────────────────────────────────────
add_subdirectory(daemon)

# ──────────────────────────────────────────────────────────────────────────────
# WavEqTest standalone tool
#
# Pre-existing gap fixed here: WavEqTest.cpp uses BandEqualizer (from the
# Windows APO project directory, Equalizer/BandEqualizer.{h,cpp}), but that
# source file was never part of this target -- it would fail to link with
# "undefined reference to BandEqualizer::BandEqualizer()" on any non-Windows
# build (found while verifying the build still works after wiring kissfft
# into the dsp library above). BandEqualizer itself has no Windows-specific
# dependencies (plain std::array data holder), so it builds cleanly here too.
# ──────────────────────────────────────────────────────────────────────────────
add_executable(WavEqTest Tools/WavEqTest.cpp Equalizer/BandEqualizer.cpp Equalizer/BandEqualizer.h)
target_link_libraries(WavEqTest PRIVATE dsp)
target_include_directories(WavEqTest PRIVATE DSP)
