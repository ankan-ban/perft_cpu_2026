cmake_minimum_required(VERSION 3.18)

project(perft_cpu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PERFT_SOURCES
    perft.cpp
    launcher.cpp
    move_gen_init.cpp
    util.cpp
    GlobalVars.cpp
    Magics.cpp
    zobrist.cpp
    randoms.cpp
)

add_executable(perft_cpu ${PERFT_SOURCES})
target_include_directories(perft_cpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    # /Ob3: aggressive inlining. /Oi: enable intrinsics. /Ot: favor speed.
    # /fp:fast: relax floating-point (no perft math depends on strict FP).
    # /GL + /LTCG omitted — they hung > 3 min on this heavily-templated codebase;
    # /Ob3 alone gives full per-TU inlining which is what the hot path needs.
    target_compile_options(perft_cpu PRIVATE /O2 /Ob3 /Oi /Ot /fp:fast /Zi /GL)
    target_link_options(perft_cpu PRIVATE /DEBUG /OPT:REF /OPT:ICF /LTCG /USEPROFILE)

    # ARM64 needs an explicit arch hint to enable newer extensions (CNT, RBIT, etc.).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        target_compile_options(perft_cpu PRIVATE /arch:armv8.4)
    endif()

    # PERFT_DUMP_ASM=ON emits source-interleaved listings for hot-path inspection.
    # Off by default — bloats build ~50×.
    if(PERFT_DUMP_ASM)
        target_compile_options(perft_cpu PRIVATE /FAs)
    endif()
else()
    target_compile_options(perft_cpu PRIVATE -O3 -flto)
    target_link_options(perft_cpu PRIVATE -flto)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(perft_cpu PRIVATE -mpopcnt -mbmi)
    endif()
endif()
