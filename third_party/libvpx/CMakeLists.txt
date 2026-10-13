# libvpx (VP8 and VP9 encoding and decoding: RCS_BACKEND_LIBVPX), a static
# library the SDK links, so that only what it calls goes in: every symbol
# hidden and renamed with the prefix rcsdk_ (rcsdk_libvpx_names.h), so hosts
# may link their own libvpx. Built from the configuration of the target's
# architecture (config/<architecture>/, written by generate-config.sh with
# libvpx's own configure): its sources and run-time CPU dispatch (x86-64:
# SSE2 to AVX-512, with NASM; arm64: NEON, dot product, int8 matrix multiply);
# another architecture, or several at once (a universal binary), gets the
# portable C. See README.rcsdk.md for what was vendored.
set(vpx_dir "${CMAKE_CURRENT_SOURCE_DIR}")

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(vpx_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(vpx_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(vpx_arch MATCHES "^(arm64|aarch64|ARM64)$")
    set(vpx_config arm64)
elseif(vpx_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(vpx_config x86_64)
else()
    set(vpx_config generic)
endif()
if(WIN32 AND NOT vpx_config STREQUAL "generic")
    set(vpx_config "windows-${vpx_config}")
endif()
set(vpx_config_dir "${vpx_dir}/config/${vpx_config}")

file(STRINGS "${vpx_config_dir}/sources.txt" vpx_sources)
list(TRANSFORM vpx_sources PREPEND "${vpx_dir}/")
list(FILTER vpx_sources EXCLUDE REGEX "\\.asm$")
file(STRINGS "${vpx_config_dir}/sources.txt" vpx_asm REGEX "\\.asm$")
list(TRANSFORM vpx_asm PREPEND "${vpx_dir}/")
if(vpx_asm)
    enable_language(ASM_NASM)
    if(NOT CMAKE_ASM_NASM_COMPILER_LOADED)
        message(FATAL_ERROR "libvpx needs NASM on x86-64 (or RCSDK_WITH_LIBVPX=OFF)")
    endif()
    list(APPEND vpx_sources ${vpx_asm})
    set_source_files_properties(${vpx_asm} PROPERTIES
        COMPILE_OPTIONS "-I${vpx_dir}/;-I${vpx_config_dir}/;-Dprivate_prefix=rcsdk_vpx;-P${vpx_dir}/rcsdk_libvpx_names.asm")
endif()

add_library(rcsdk_libvpx STATIC ${vpx_sources} "${vpx_config_dir}/vpx_config.c")
set_target_properties(rcsdk_libvpx PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(rcsdk_libvpx
    PUBLIC "$<BUILD_INTERFACE:${vpx_dir}>"
    PRIVATE "${vpx_config_dir}" "${vpx_dir}/config")

# Each SIMD source is built for its instruction set; run-time detection
# decides which runs.
foreach(source IN LISTS vpx_sources)
    if(MSVC)
        if(source MATCHES "_avx512\\.c$")
            set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
        elseif(source MATCHES "_avx2\\.c$")
            set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
        elseif(source MATCHES "_avx\\.c$")
            set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/arch:AVX")
        endif()
    elseif(source MATCHES "_avx512\\.c$")
        set_source_files_properties("${source}" PROPERTIES
            COMPILE_OPTIONS "-mavx512f;-mavx512cd;-mavx512bw;-mavx512dq;-mavx512vl")
    elseif(source MATCHES "_avx2\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-mavx2")
    elseif(source MATCHES "_avx\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-mavx")
    elseif(source MATCHES "_sse4\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-msse4.1")
    elseif(source MATCHES "_ssse3\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-mssse3")
    elseif(source MATCHES "_sse3\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-msse3")
    elseif(source MATCHES "_neon_i8mm\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-march=armv8.2-a+dotprod+i8mm")
    elseif(source MATCHES "_neon_dotprod\\.c$")
        set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-march=armv8.2-a+dotprod")
    endif()
endforeach()

if(MSVC)
    target_compile_options(rcsdk_libvpx PRIVATE "$<$<COMPILE_LANGUAGE:C>:/FI${vpx_dir}/rcsdk_libvpx_names.h;/w>")
else()
    target_compile_options(rcsdk_libvpx PRIVATE
        "$<$<COMPILE_LANGUAGE:C>:SHELL:-include \"${vpx_dir}/rcsdk_libvpx_names.h\">" "$<$<COMPILE_LANGUAGE:C>:-w>")
endif()
# Optimized in Debug builds too: unoptimized, real-time encoding is many times slower.
if(NOT MSVC)
    target_compile_options(rcsdk_libvpx PRIVATE $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-O2>)
endif()
target_link_libraries(rcsdk_libvpx PRIVATE Threads::Threads)
target_compile_definitions(rcsdk_libvpx INTERFACE RCSDK_WITH_LIBVPX=1)
if(NOT BUILD_SHARED_LIBS) # A static core is linked with it.
    install(TARGETS rcsdk_libvpx EXPORT RemoteControlSDKTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
