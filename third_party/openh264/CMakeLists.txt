# OpenH264 (H.264 encoding and decoding: RCS_BACKEND_OPENH264), a static
# library the SDK links, so that only what it calls goes in: every symbol
# hidden and renamed with the prefix rcsdk_ (rcsdk_openh264_names.h: its C
# functions and assembly, and its C++ namespaces and classes). What
# OpenH264's meson.build does: four parts (common, processing, encoder,
# decoder), each with its own headers (the encoder's and decoder's share
# names), and their assembly (arm64: NEON; x86-64: SSE2 to AVX2, with NASM),
# which picks the instruction set at run time. Another architecture, several
# at once (a universal binary) or MSVC on arm64 (whose assembler does not
# take GNU syntax) gets the portable C++. See README.rcsdk.md for what was
# vendored.
set(openh264_dir "${CMAKE_CURRENT_SOURCE_DIR}/codec")

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(openh264_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(openh264_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
set(openh264_asm "")
if(openh264_arch MATCHES "^(arm64|aarch64|ARM64)$" AND NOT MSVC)
    set(openh264_asm arm64)
    enable_language(ASM)
elseif(openh264_arch MATCHES "^(x86_64|AMD64|amd64)$")
    set(openh264_asm x86)
    enable_language(ASM_NASM)
    if(NOT CMAKE_ASM_NASM_COMPILER_LOADED)
        message(FATAL_ERROR "OpenH264 needs NASM on x86-64 (or RCSDK_WITH_OPENH264=OFF)")
    endif()
endif()

# One part: its C++ sources, its assembly for this architecture (not the
# files the others include), and its headers.
function(openh264_part name)
    file(GLOB sources "${openh264_dir}/${name}/*.cpp" "${openh264_dir}/${name}/*/*.cpp"
                      "${openh264_dir}/${name}/*/*/*.cpp")
    set(asm "")
    if(openh264_asm STREQUAL "arm64")
        file(GLOB_RECURSE asm "${openh264_dir}/${name}/*.S")
        list(FILTER asm EXCLUDE REGEX "arm_arch64_common_macro\\.S$")
    elseif(openh264_asm STREQUAL "x86")
        file(GLOB_RECURSE asm "${openh264_dir}/${name}/*.asm")
        list(FILTER asm EXCLUDE REGEX "asm_inc\\.asm$")
    endif()
    add_library(rcsdk_openh264_${name} OBJECT ${sources} ${asm})
    set_target_properties(rcsdk_openh264_${name} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        CXX_STANDARD 17)
    list(TRANSFORM ARGN PREPEND "${openh264_dir}/")
    target_include_directories(rcsdk_openh264_${name} PRIVATE "${openh264_dir}/api/wels" "${openh264_dir}/common/inc"
                                                             ${ARGN})
    if(openh264_asm STREQUAL "arm64")
        target_include_directories(rcsdk_openh264_${name} PRIVATE "${openh264_dir}/common/arm64")
        target_compile_definitions(rcsdk_openh264_${name} PRIVATE HAVE_NEON_AARCH64)
    elseif(openh264_asm STREQUAL "x86")
        target_compile_definitions(rcsdk_openh264_${name} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:HAVE_AVX2 X86_ASM>)
        # x86-64's calling convention, Mach-O's underscore, and every function hidden.
        if(WIN32)
            set(abi "-DWIN64")
        elseif(APPLE)
            set(abi "-DUNIX64;-DPREFIX;-DWELS_PRIVATE_EXTERN=private_extern")
        else()
            set(abi "-DUNIX64;-DWELS_PRIVATE_EXTERN=hidden")
        endif()
        set_source_files_properties(${asm} PROPERTIES COMPILE_OPTIONS
            "-I${openh264_dir}/common/x86/;-DHAVE_AVX2;${abi};-P${CMAKE_CURRENT_SOURCE_DIR}/rcsdk_openh264_names.asm;-w-all")
    endif()
    if(MSVC)
        target_compile_options(rcsdk_openh264_${name} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX>:/FI${CMAKE_CURRENT_SOURCE_DIR}/rcsdk_openh264_names.h;/w>")
    else()
        target_compile_options(rcsdk_openh264_${name} PRIVATE
            "$<$<COMPILE_LANGUAGE:CXX,ASM>:SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/rcsdk_openh264_names.h\">"
            "$<$<COMPILE_LANGUAGE:CXX,ASM>:-w>"
            # Optimized in Debug builds too: unoptimized, encoding is many times slower.
            "$<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:CXX>>:-O2>")
    endif()
endfunction()

openh264_part(common)
openh264_part(processing processing/interface processing/src/common processing/src/adaptivequantization
                         processing/src/downsample processing/src/scrolldetection processing/src/vaacalc)
openh264_part(encoder processing/interface encoder/core/inc encoder/plus/inc)
openh264_part(decoder decoder/core/inc decoder/plus/inc)

add_library(rcsdk_openh264 STATIC $<TARGET_OBJECTS:rcsdk_openh264_common> $<TARGET_OBJECTS:rcsdk_openh264_processing>
                                  $<TARGET_OBJECTS:rcsdk_openh264_encoder> $<TARGET_OBJECTS:rcsdk_openh264_decoder>)
set_target_properties(rcsdk_openh264 PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(rcsdk_openh264 PUBLIC "$<BUILD_INTERFACE:${openh264_dir}/api;${CMAKE_CURRENT_SOURCE_DIR}>")
target_link_libraries(rcsdk_openh264 PRIVATE Threads::Threads)
target_compile_definitions(rcsdk_openh264 INTERFACE RCSDK_WITH_OPENH264=1)
if(NOT BUILD_SHARED_LIBS) # A static core is linked with it.
    install(TARGETS rcsdk_openh264 EXPORT RemoteControlSDKTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
