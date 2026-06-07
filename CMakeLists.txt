cmake_minimum_required(VERSION 3.20)
project(pixelShuffle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(COMMON_FLAGS
    -fno-stack-protector
    -march=native
    -mtune=native
)

# Optional profiling instrumentation (cmake -DENABLE_PROFILING=ON ..)
option(ENABLE_PROFILING "Define ENABLE_PROFILING" OFF)
if(ENABLE_PROFILING)
    add_compile_definitions(ENABLE_PROFILING)
endif()

option(ENABLE_NEON_SWAP_KERNEL "Enable the NEON-accelerated pngLAB swap kernel on ARM64" ON)

# ---------------------------------------------------------------------------
#  Dependencies
# ---------------------------------------------------------------------------
# -- libpng (try system-wide first, then fall back to Homebrew path)
find_package(PNG)
if(NOT PNG_FOUND)
    # Homebrew default prefix on Apple Silicon:
    set(PNG_INCLUDE_DIR "/opt/homebrew/opt/libpng/include/libpng16")
    set(PNG_LIBRARY     "/opt/homebrew/opt/libpng/lib/libpng16.dylib")
    if(EXISTS "${PNG_LIBRARY}")
        add_library(PNG::PNG UNKNOWN IMPORTED)
        set_target_properties(PNG::PNG PROPERTIES
            IMPORTED_LOCATION             "${PNG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PNG_INCLUDE_DIR}"
        )
    else()
        message(FATAL_ERROR "Could not find libpng; set PNG_ROOT or edit CMakeLists.txt")
    endif()
endif()

find_package(OpenMP QUIET)

# ---------------------------------------------------------------------------
#  Helper function to reduce repetition
# ---------------------------------------------------------------------------
function(add_pixelshuffle_exe target src optim_flags extra_defs extra_libs)
    add_executable(${target} ${src})
    target_compile_options(${target} PRIVATE
        ${COMMON_FLAGS}
        ${optim_flags}        # -Ofast, -O3, or -O0
    )
    target_compile_definitions(${target} PRIVATE ${extra_defs})
    target_link_libraries(${target} PRIVATE PNG::PNG ${extra_libs})
endfunction()

set(PNGLAB_ARCH_DEFS "")
if(ENABLE_NEON_SWAP_KERNEL AND (CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"))
    list(APPEND PNGLAB_ARCH_DEFS PNG_LAB_ENABLE_NEON)
endif()

set(PNGLAB_HARNESS_DEFS "SHOULD_SWAP_AB_HARNESS")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    list(APPEND PNGLAB_HARNESS_DEFS PNG_LAB_ENABLE_NEON)
endif()

# ---------------------------------------------------------------------------
#  Build targets (pngRGB, pngLAB, hungarian)
# ---------------------------------------------------------------------------
# Release/RelWithDebInfo: use original high-perf flags
# Debug: stay at -O0 like the Makefile’s debug target
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(OFLAG "-O0")
else()
    set(OFLAG "-O3")
endif()

#add_pixelshuffle_exe(pngRGB pngRGB.cpp "${OFLAG}" "OUTPUT;ANIMATION" "")
add_pixelshuffle_exe(pngLAB pngLAB.cpp "${OFLAG}" "OUTPUT;ANIMATION;${PNGLAB_ARCH_DEFS}" "")
add_pixelshuffle_exe(pngLAB_clean pngLAB_clean.cpp "${OFLAG}" "OUTPUT;ANIMATION;${PNGLAB_ARCH_DEFS}" "")
add_pixelshuffle_exe(pngLAB_spiral pngLAB_spiral.cpp "${OFLAG}" "OUTPUT;ANIMATION" "")
add_pixelshuffle_exe(pngLAB_pretty pngLAB_pretty.cpp "${OFLAG}" "OUTPUT;ANIMATION" "")
add_pixelshuffle_exe(pngLAB_bench_core pngLAB.cpp "${OFLAG}" "PROFILE_STATS;${PNGLAB_ARCH_DEFS}" "")
add_pixelshuffle_exe(pngLAB_bench_e2e pngLAB.cpp "${OFLAG}" "PROFILE_STATS;OUTPUT;ANIMATION;${PNGLAB_ARCH_DEFS}" "")
add_pixelshuffle_exe(shouldSwap_ab_harness pngLAB.cpp "${OFLAG}" "${PNGLAB_HARNESS_DEFS}" "")

# hungarian uses OpenMP and a slightly different optimisation level
set(HUNGARIAN_OFLAG "-O3")
add_pixelshuffle_exe(hungarian hungarian.cpp "${HUNGARIAN_OFLAG}" "" ""
)

if(OpenMP_CXX_FOUND)
    target_link_libraries(hungarian PRIVATE OpenMP::OpenMP_CXX)
    target_compile_options(hungarian PRIVATE -fopenmp)
endif()

# ---------------------------------------------------------------------------
#  Custom targets replicating Makefile conveniences
# ---------------------------------------------------------------------------

# Generate annotated assembly (.s) files – exactly what the original ‘> file.s’ did
add_custom_target(asm
    COMMAND ${CMAKE_COMMAND} --build . --target pngRGB --config $<CONFIG>
    COMMAND ${CMAKE_CXX_COMPILER} ${COMMON_FLAGS} ${OFLAG}
            -DOUTPUT -DANIMATION -S ${CMAKE_SOURCE_DIR}/pngRGB.cpp -o pngRGB.s
    COMMAND ${CMAKE_COMMAND} --build . --target pngLAB --config $<CONFIG>
    COMMAND ${CMAKE_CXX_COMPILER} ${COMMON_FLAGS} ${OFLAG}
            -DOUTPUT -DANIMATION -S ${CMAKE_SOURCE_DIR}/pngLAB.cpp -o pngLAB.s
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Rebuilding annotated assembly listings (pngRGB.s, pngLAB.s)"
)

# ‘profile’ target — runs the same valgrind callgrind command
add_custom_target(profile
    DEPENDS pngLAB
    COMMAND valgrind -v --tool=callgrind --log-fd=1
            --dump-instr=yes --collect-jumps=yes --cache-sim=yes
            --branch-sim=yes --simulate-wb=yes --simulate-hwpref=yes --cacheuse=yes
            $<TARGET_FILE:pngLAB> ${CMAKE_SOURCE_DIR}/images/mona.png
            ${CMAKE_SOURCE_DIR}/images/gothic.png test.png
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running pngLAB under Callgrind"
)

# ‘make clean’ becomes ‘cmake --build . --target clean’, already provided by CMake
