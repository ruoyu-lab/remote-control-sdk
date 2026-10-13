# libaom (AV1 encoding: RCS_BACKEND_LIBAOM), a static library the SDK links,
# so that only what it calls goes in: every symbol hidden and renamed with the
# prefix rcsdk_ (rcsdk_libaom_names.h), so hosts may link their own libaom.
# Built from the configuration of the target's architecture (config/
# <architecture>/, written by generate-config.sh with libaom's own CMake):
# its sources and run-time CPU dispatch (x86-64: SSE2 to AVX2, with NASM;
# arm64: NEON, dot product, int8 matrix multiply, CRC32); another
# architecture, or several at once (a universal binary), gets the portable C.
# See README.rcsdk.md for what was vendored.
set(aom_dir "${CMAKE_CURRENT_SOURCE_DIR}")

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(aom_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(aom_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(aom_arch MATCHES "^(arm64|aarch64|ARM64)$")
    set(aom_config arm64)
elseif(aom_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(aom_config x86_64)
else()
    set(aom_config generic)
endif()
set(aom_config_dir "${aom_dir}/config/${aom_config}")

# Each line: a source, and the instruction set it is built for where it has
# one (the flags libaom's CMake gives each; MSVC needs one for AVX alone).
set(isa_flags_sse2 "-msse2")
set(isa_flags_sse3 "-msse3")
set(isa_flags_ssse3 "-mssse3")
set(isa_flags_sse4_1 "-msse4.1")
set(isa_flags_sse4_2 "-msse4.2")
set(isa_flags_avx "-mavx")
set(isa_flags_avx2 "-mavx2")
set(isa_flags_avx512 "-march=skylake-avx512")
set(isa_flags_neon_dotprod "-march=armv8.2-a+dotprod")
set(isa_flags_neon_i8mm "-march=armv8.2-a+dotprod+i8mm")
set(isa_flags_arm_crc32 "-march=armv8-a+crc")
file(STRINGS "${aom_config_dir}/sources.txt" aom_lines)
# Windows x86-64 also gets the x87 control word functions MSVC's code calls
# (libaom's own CMake adds them for that target alone).
if(WIN32 AND aom_config STREQUAL "x86_64")
    list(APPEND aom_lines aom_ports/float.asm)
endif()
set(aom_sources "${aom_config_dir}/config/aom_config.c")
set(aom_asm)
foreach(line IN LISTS aom_lines)
    string(REPLACE " " ";" fields "${line}")
    list(GET fields 0 source)
    set(source "${aom_dir}/${source}")
    list(APPEND aom_sources "${source}")
    if(source MATCHES "\\.asm$")
        list(APPEND aom_asm "${source}")
        continue()
    endif()
    list(LENGTH fields count)
    if(count EQUAL 1)
        continue()
    endif()
    list(GET fields 1 isa)
    if(MSVC)
        set(flags "")
        if(isa STREQUAL "avx")
            set(flags "/arch:AVX")
        elseif(isa STREQUAL "avx2")
            set(flags "/arch:AVX2")
        endif()
    elseif(DEFINED isa_flags_${isa})
        set(flags "${isa_flags_${isa}}")
    else()
        message(FATAL_ERROR "libaom: ${source} is for an instruction set this file does not know (${isa})")
    endif()
    if(flags)
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "${flags}")
    endif()
endforeach()
if(aom_asm)
    enable_language(ASM_NASM)
    if(NOT CMAKE_ASM_NASM_COMPILER_LOADED)
        message(FATAL_ERROR "libaom needs NASM on x86-64 (or RCSDK_WITH_LIBAOM=OFF)")
    endif()
    set_source_files_properties(${aom_asm} PROPERTIES
        COMPILE_OPTIONS "-I${aom_dir}/;-I${aom_config_dir}/;-Dprivate_prefix=rcsdk_aom;-P${aom_dir}/rcsdk_libaom_names.asm")
endif()

add_library(rcsdk_libaom STATIC ${aom_sources})
set_target_properties(rcsdk_libaom PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    C_STANDARD 11)
target_include_directories(rcsdk_libaom
    PUBLIC "$<BUILD_INTERFACE:${aom_dir}>"
    PRIVATE "${aom_config_dir}")
if(MSVC)
    target_compile_options(rcsdk_libaom PRIVATE "$<$<COMPILE_LANGUAGE:C>:/FI${aom_dir}/rcsdk_libaom_names.h;/w>")
else()
    target_compile_options(rcsdk_libaom PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:SHELL:-include \"${aom_dir}/rcsdk_libaom_names.h\">" "$<$<COMPILE_LANGUAGE:C>:-w>"
        # Optimized in Debug builds too: unoptimized, real-time encoding is many times slower.
        "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-O2>")
endif()
if(NOT WIN32)
    target_link_libraries(rcsdk_libaom PRIVATE Threads::Threads)
endif()
target_compile_definitions(rcsdk_libaom INTERFACE RCSDK_WITH_LIBAOM=1)
if(NOT BUILD_SHARED_LIBS) # A static core is linked with it.
    install(TARGETS rcsdk_libaom EXPORT RemoteControlSDKTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
