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
    cliff_probe.cpp
    leaf_batch_avx2.cpp
)

add_executable(perft_cpu ${PERFT_SOURCES})
target_include_directories(perft_cpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Plan A AVX2 TUs: cliff_probe.cpp + leaf_batch_avx2.cpp are compiled
# /arch:AVX2 in isolation. All other TUs stay at default arch.
# /GL- (disable whole-program opt) prevents LTCG from inlining the entry
# points back into the caller — keeping the AVX2/non-AVX2 boundary at the
# function entry, which is what amortizes the (small) save/restore cost
# across many leaves. With /GL on, MSVC's LTCG would erase the boundary and
# the per-TU /arch:AVX2 distinction would be meaningless.
if(MSVC AND (CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|X86|x86"))
    # cliff_probe stays /GL- (Phase 0 microbench needs a true non-inlinable call).
    set_source_files_properties(cliff_probe.cpp     PROPERTIES COMPILE_FLAGS "/arch:AVX2 /GL-")
    # leaf_batch_avx2 keeps /GL — under /GL- the linker picks the native COMDAT
    # template instantiation of countMovesPair[Cached] from this TU for ALL
    # call sites, killing LTCG inlining of those bodies into themselves and
    # producing a ~+50ms regression. With /GL kept, LTCG sees uniform IL and
    # the __declspec(noinline) on the four entry points preserves the per-call
    # AVX2/non-AVX2 boundary.
    set_source_files_properties(leaf_batch_avx2.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")
endif()

if(MSVC)
    # PERFT_PGO: OFF (default; no PGO), GEN (instrument), USE (consume .pgd).
    if(NOT DEFINED PERFT_PGO)
        set(PERFT_PGO "OFF")
    endif()

    # /Ob3: aggressive inlining. /Oi: enable intrinsics. /Ot: favor speed.
    # /fp:fast: relax floating-point (no perft math depends on strict FP).
    if(PERFT_DUMP_ASM)
        # /FAs emits per-TU asm dumps. Incompatible with /GL+/LTCG (which defers
        # codegen to link time, leaving per-TU asm empty), so the asm-build
        # config disables them. Used only for hot-path inspection.
        target_compile_options(perft_cpu PRIVATE /O2 /Ob3 /Oi /Ot /fp:fast /Zi /FAs)
        target_link_options(perft_cpu PRIVATE /DEBUG /OPT:REF /OPT:ICF)
    elseif(PERFT_PGO STREQUAL "GEN")
        target_compile_options(perft_cpu PRIVATE /O2 /Ob3 /Oi /Ot /fp:fast /Zi /GL)
        target_link_options(perft_cpu PRIVATE /DEBUG /OPT:REF /OPT:ICF /LTCG /GENPROFILE)
    elseif(PERFT_PGO STREQUAL "USE")
        target_compile_options(perft_cpu PRIVATE /O2 /Ob3 /Oi /Ot /fp:fast /Zi /GL)
        target_link_options(perft_cpu PRIVATE /DEBUG /OPT:REF /OPT:ICF /LTCG /USEPROFILE)
    else()
        target_compile_options(perft_cpu PRIVATE /O2 /Ob3 /Oi /Ot /fp:fast /Zi /GL)
        target_link_options(perft_cpu PRIVATE /DEBUG /OPT:REF /OPT:ICF /LTCG)
    endif()

    # ARM64 needs an explicit arch hint to enable newer extensions (CNT, RBIT, etc.).
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        target_compile_options(perft_cpu PRIVATE /arch:armv8.4)
    endif()
else()
    target_compile_options(perft_cpu PRIVATE -O3 -flto)
    target_link_options(perft_cpu PRIVATE -flto)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        target_compile_options(perft_cpu PRIVATE -mpopcnt -mbmi)
    endif()
endif()
