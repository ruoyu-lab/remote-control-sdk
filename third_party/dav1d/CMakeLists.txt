# dav1d (AV1 decoding: RCS_BACKEND_DAV1D), a static library the SDK links,
# so that only what it calls goes in: every symbol hidden and renamed with the
# prefix rcsdk_ (rcsdk_dav1d_names.h; the assembly's own prefix is
# rcsdk_dav1d_ too). What dav1d's meson.build does for the library, for the
# target: config.h from the checks it makes, 8- and 10-bit pictures (the
# template sources built once for each), and its assembly with run-time CPU
# detection (arm64: NEON, dot product, int8 matrix multiply, SVE and SVE2,
# where the assembler knows them; x86-64: SSE2 to AVX-512, with NASM).
# Another architecture, several at once (a universal binary) or MSVC on arm64
# (whose assembler does not take GNU syntax) gets the portable C. See
# README.rcsdk.md for what was vendored.
set(dav1d_dir "${CMAKE_CURRENT_SOURCE_DIR}")

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(dav1d_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(dav1d_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(ARCH_AARCH64 OFF)
set(ARCH_X86_64 OFF)
if(dav1d_arch MATCHES "^(arm64|aarch64|ARM64)$" AND NOT MSVC)
    set(ARCH_AARCH64 ON)
elseif(dav1d_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(ARCH_X86_64 ON)
endif()
set(ARCH_X86 ${ARCH_X86_64})
if(ARCH_AARCH64 OR ARCH_X86_64)
    set(HAVE_ASM ON)
endif()
if(APPLE)
    set(PREFIX ON)
endif()

# The checks meson.build makes, compiled only (no program is linked, so
# cross builds check the same way).
include(CheckCSourceCompiles)
include(CheckIncludeFile)
include(CheckSymbolExists)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
endif()
set(CMAKE_REQUIRED_QUIET ON)
if(NOT WIN32)
    check_symbol_exists(posix_memalign stdlib.h HAVE_POSIX_MEMALIGN)
    check_symbol_exists(memalign malloc.h HAVE_MEMALIGN)
    check_symbol_exists(aligned_alloc stdlib.h HAVE_ALIGNED_ALLOC)
    check_symbol_exists(pthread_getaffinity_np pthread.h HAVE_PTHREAD_GETAFFINITY_NP)
    check_symbol_exists(pthread_setname_np pthread.h HAVE_PTHREAD_SETNAME_NP)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    check_symbol_exists(dlsym dlfcn.h HAVE_DLSYM)
endif()
if(ARCH_AARCH64)
    check_symbol_exists(getauxval sys/auxv.h HAVE_GETAUXVAL)
endif()
check_include_file(unistd.h HAVE_UNISTD_H)
check_include_file(stdatomic.h DAV1D_HAVE_STDATOMIC_H)

# The arm64 assembler: the highest .arch level it takes (with +crc, which
# makes older Clangs apply it at once), and each extension's directive and
# instructions. (The checks are macros, which read escapes once more: "\\\\n"
# here is "\n" in the C source.)
if(HAVE_ASM AND ARCH_AARCH64)
    check_c_source_compiles("__asm__(\".arch armv8-a\\\\n\");" HAVE_AS_ARCH_DIRECTIVE)
    set(AS_ARCH_LEVEL "armv8-a")
    if(HAVE_AS_ARCH_DIRECTIVE)
        foreach(level IN ITEMS 8.2 8.4 8.6)
            string(REPLACE "." "_" name "${level}")
            check_c_source_compiles("__asm__(\".arch armv${level}-a\\\\n\");" DAV1D_AS_ARCH_${name})
            if(DAV1D_AS_ARCH_${name})
                set(AS_ARCH_LEVEL "armv${level}-a")
            endif()
        endforeach()
        check_c_source_compiles("__asm__(\".arch ${AS_ARCH_LEVEL}+crc\\\\n\");" DAV1D_AS_ARCH_CRC)
        if(DAV1D_AS_ARCH_CRC)
            string(APPEND AS_ARCH_LEVEL "+crc")
        endif()
    endif()
    set(arch "")
    if(HAVE_AS_ARCH_DIRECTIVE)
        set(arch "\".arch ${AS_ARCH_LEVEL}\\\\n\"")
    endif()
    foreach(extension IN ITEMS "dotprod:udot v0.4s, v0.16b, v0.16b" "i8mm:usdot v0.4s, v0.16b, v0.16b"
                               "sve:whilelt p0.s, x0, x1" "sve2:sqrdmulh z0.s, z0.s, z0.s")
        string(REGEX REPLACE ":.*" "" name "${extension}")
        string(REGEX REPLACE "^[^:]*:" "" instruction "${extension}")
        string(TOUPPER "${name}" upper)
        check_c_source_compiles("__asm__(${arch} \".arch_extension ${name}\\\\n\");"
                                HAVE_AS_ARCHEXT_${upper}_DIRECTIVE)
        set(enable "")
        if(HAVE_AS_ARCHEXT_${upper}_DIRECTIVE)
            set(enable "\".arch_extension ${name}\\\\n\"")
        endif()
        check_c_source_compiles("__asm__(${arch} ${enable} \"${instruction}\\\\n\");" HAVE_${upper})
    endforeach()
endif()
unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
unset(CMAKE_REQUIRED_DEFINITIONS)
set(dav1d_config_dir "${CMAKE_CURRENT_BINARY_DIR}/config")
configure_file("${dav1d_dir}/config.h.in" "${dav1d_config_dir}/config.h")

set(dav1d_sources cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c intra_edge.c itx_1d.c lf_mask.c
                  lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c refmvs.c scan.c tables.c thread_task.c
                  warpmv.c wedge.c)
# Built once for each bit depth (BITDEPTH 8 and 16).
set(dav1d_template_sources cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c filmgrain_tmpl.c ipred_prepare_tmpl.c
                           ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c loopfilter_tmpl.c looprestoration_tmpl.c
                           lr_apply_tmpl.c mc_tmpl.c recon_tmpl.c)
if(WIN32)
    list(APPEND dav1d_sources win32/thread.c)
endif()
if(HAVE_ASM AND ARCH_AARCH64)
    enable_language(ASM)
    list(APPEND dav1d_sources arm/cpu.c
        # Both bit depths
        arm/64/itx.S arm/64/looprestoration_common.S arm/64/msac.S arm/64/refmvs.S
        # 8-bit
        arm/64/cdef.S arm/64/filmgrain.S arm/64/ipred.S arm/64/loopfilter.S arm/64/looprestoration.S arm/64/mc.S
        arm/64/mc_dotprod.S
        # 16-bit
        arm/64/cdef16.S arm/64/filmgrain16.S arm/64/ipred16.S arm/64/itx16.S arm/64/loopfilter16.S
        arm/64/looprestoration16.S arm/64/mc16.S arm/64/mc16_sve.S)
elseif(HAVE_ASM AND ARCH_X86_64)
    enable_language(ASM_NASM)
    if(NOT CMAKE_ASM_NASM_COMPILER_LOADED)
        message(FATAL_ERROR "dav1d needs NASM on x86-64 (or RCSDK_WITH_DAV1D=OFF)")
    endif()
    if(APPLE)
        set(DAV1D_ASM_PREFIX "%define PREFIX 1")
    endif()
    configure_file("${dav1d_dir}/config.asm.in" "${dav1d_config_dir}/config.asm")
    set(dav1d_asm
        # Both bit depths
        cpuid.asm msac.asm pal.asm refmvs.asm itx_avx512.asm cdef_avx2.asm itx_avx2.asm cdef_sse.asm itx_sse.asm
        # 8-bit
        cdef_avx512.asm filmgrain_avx512.asm ipred_avx512.asm loopfilter_avx512.asm looprestoration_avx512.asm
        mc_avx512.asm filmgrain_avx2.asm ipred_avx2.asm loopfilter_avx2.asm looprestoration_avx2.asm mc_avx2.asm
        filmgrain_sse.asm ipred_sse.asm loopfilter_sse.asm looprestoration_sse.asm mc_sse.asm
        # 16-bit
        cdef16_avx512.asm filmgrain16_avx512.asm ipred16_avx512.asm itx16_avx512.asm loopfilter16_avx512.asm
        looprestoration16_avx512.asm mc16_avx512.asm cdef16_avx2.asm filmgrain16_avx2.asm ipred16_avx2.asm
        itx16_avx2.asm loopfilter16_avx2.asm looprestoration16_avx2.asm mc16_avx2.asm cdef16_sse.asm
        filmgrain16_sse.asm ipred16_sse.asm itx16_sse.asm loopfilter16_sse.asm looprestoration16_sse.asm
        mc16_sse.asm)
    list(TRANSFORM dav1d_asm PREPEND "${dav1d_dir}/src/x86/")
    set_source_files_properties(${dav1d_asm} PROPERTIES
        COMPILE_OPTIONS "-I${dav1d_dir}/src/;-I${dav1d_config_dir}/")
    list(APPEND dav1d_sources x86/cpu.c)
endif()
list(TRANSFORM dav1d_sources PREPEND "${dav1d_dir}/src/")
list(TRANSFORM dav1d_template_sources PREPEND "${dav1d_dir}/src/")

# What every part is built with.
function(dav1d_target target)
    set_target_properties(${target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden
        C_STANDARD 99)
    target_include_directories(${target} PRIVATE "${dav1d_config_dir}" "${dav1d_dir}" "${dav1d_dir}/include"
                                                 "${dav1d_dir}/include/dav1d")
    if(NOT DAV1D_HAVE_STDATOMIC_H)
        target_include_directories(${target} PRIVATE "${dav1d_dir}/include/compat/msvc")
    endif()
    target_compile_definitions(${target} PRIVATE "DAV1D_API=" $<$<COMPILE_LANGUAGE:ASM>:PRIVATE_PREFIX=rcsdk_dav1d_>)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:/FI${dav1d_dir}/rcsdk_dav1d_names.h;/w>")
    else()
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C>:SHELL:-include \"${dav1d_dir}/rcsdk_dav1d_names.h\">"
            "$<$<COMPILE_LANGUAGE:C,ASM>:-w>"
            # Optimized in Debug builds too: unoptimized, decoding is many times slower.
            "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-O2>")
    endif()
endfunction()

foreach(depth IN ITEMS 8 16)
    add_library(rcsdk_dav1d_${depth} OBJECT ${dav1d_template_sources})
    dav1d_target(rcsdk_dav1d_${depth})
    target_compile_definitions(rcsdk_dav1d_${depth} PRIVATE BITDEPTH=${depth})
endforeach()
add_library(rcsdk_dav1d STATIC ${dav1d_sources} ${dav1d_asm} $<TARGET_OBJECTS:rcsdk_dav1d_8>
                               $<TARGET_OBJECTS:rcsdk_dav1d_16>)
dav1d_target(rcsdk_dav1d)
target_include_directories(rcsdk_dav1d PUBLIC "$<BUILD_INTERFACE:${dav1d_dir}/include;${dav1d_dir}>")
target_compile_definitions(rcsdk_dav1d INTERFACE RCSDK_WITH_DAV1D=1 "DAV1D_API=")
if(NOT WIN32)
    target_link_libraries(rcsdk_dav1d PRIVATE Threads::Threads)
endif()
if(HAVE_DLSYM)
    target_link_libraries(rcsdk_dav1d PRIVATE ${CMAKE_DL_LIBS})
endif()
if(NOT BUILD_SHARED_LIBS) # A static core is linked with it.
    install(TARGETS rcsdk_dav1d EXPORT RemoteControlSDKTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
