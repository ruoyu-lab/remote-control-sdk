# libopus, compiled into the SDK as an object library: every symbol is hidden
# (shared builds) and renamed with the prefix rcsdk_ (opus/rcsdk_opus_names.h,
# static builds too), so hosts may link their own libopus.
# Float build, no DNN features (deep PLC, DRED, OSCE). SIMD: NEON on arm64 and
# SSE/SSE2 on x86-64, which those architectures guarantee, so no runtime CPU
# detection; the architecture is the target's (on Apple, CMAKE_OSX_ARCHITECTURES
# when set, e.g. an x86-64 build on an arm64 Mac), and a build for several at
# once (a universal binary) uses the portable C. See opus/README.rcsdk.md for
# what was vendored.
enable_language(C)

set(opus_dir "${CMAKE_CURRENT_SOURCE_DIR}/opus")

# The source lists libopus maintains for its own builds (Automake syntax).
function(opus_sources out file variable)
    file(READ "${opus_dir}/${file}" text)
    string(REGEX REPLACE "\\\\\n" " " text "${text}")
    string(REGEX MATCH "(^|\n)${variable} = ([^\n]*)" match "${text}")
    separate_arguments(list UNIX_COMMAND "${CMAKE_MATCH_2}")
    list(TRANSFORM list PREPEND "${opus_dir}/")
    set(${out} ${list} PARENT_SCOPE)
endfunction()

file(STRINGS "${opus_dir}/package_version" opus_version REGEX "^PACKAGE_VERSION=")
string(REGEX REPLACE "^PACKAGE_VERSION=\"?([^\"]*)\"?$" "\\1" opus_version "${opus_version}")

opus_sources(opus_src opus_sources.mk OPUS_SOURCES)
opus_sources(opus_float opus_sources.mk OPUS_SOURCES_FLOAT)
opus_sources(celt_src celt_sources.mk CELT_SOURCES)
opus_sources(silk_src silk_sources.mk SILK_SOURCES)
opus_sources(silk_float silk_sources.mk SILK_SOURCES_FLOAT)
add_library(rcsdk_opus OBJECT ${opus_src} ${opus_float} ${celt_src} ${silk_src} ${silk_float})

if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(opus_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(opus_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(opus_arch MATCHES "^(arm64|aarch64|ARM64)$")
    opus_sources(celt_neon celt_sources.mk CELT_SOURCES_ARM_NEON_INTR)
    opus_sources(silk_neon silk_sources.mk SILK_SOURCES_ARM_NEON_INTR)
    target_sources(rcsdk_opus PRIVATE ${celt_neon} ${silk_neon})
    target_compile_definitions(rcsdk_opus PRIVATE OPUS_ARM_MAY_HAVE_NEON OPUS_ARM_MAY_HAVE_NEON_INTR
                                                  OPUS_ARM_PRESUME_NEON OPUS_ARM_PRESUME_NEON_INTR)
elseif(opus_arch MATCHES "^(x86_64|AMD64|amd64)$")
    opus_sources(celt_sse celt_sources.mk CELT_SOURCES_SSE)
    opus_sources(celt_sse2 celt_sources.mk CELT_SOURCES_SSE2)
    target_sources(rcsdk_opus PRIVATE ${celt_sse} ${celt_sse2})
    target_compile_definitions(rcsdk_opus PRIVATE OPUS_X86_MAY_HAVE_SSE OPUS_X86_PRESUME_SSE
                                                  OPUS_X86_MAY_HAVE_SSE2 OPUS_X86_PRESUME_SSE2)
endif()

set_target_properties(rcsdk_opus PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
target_include_directories(rcsdk_opus
    PUBLIC "${opus_dir}/include"
    PRIVATE "${opus_dir}" "${opus_dir}/celt" "${opus_dir}/silk" "${opus_dir}/silk/float" "${opus_dir}/silk/fixed")
# OPUS_EXPORT empty: without it, opus marks its API visible.
if(MSVC)
    target_compile_options(rcsdk_opus PRIVATE "/FI${opus_dir}/rcsdk_opus_names.h")
else()
    target_compile_options(rcsdk_opus PRIVATE "SHELL:-include \"${opus_dir}/rcsdk_opus_names.h\"")
endif()
target_compile_definitions(rcsdk_opus
    PUBLIC "OPUS_EXPORT="
    PRIVATE OPUS_BUILD USE_ALLOCA HAVE_LRINT HAVE_LRINTF ENABLE_HARDENING "PACKAGE_VERSION=\"${opus_version}\"")
if(NOT MSVC)
    target_compile_options(rcsdk_opus PRIVATE -w) # Third-party code: its warnings are not ours to fix.
endif()

# Monocypher (the short-PIN PAKE: X25519, Elligator 2, SHA-512), compiled into
# the SDK the same way: every symbol hidden and renamed with the prefix rcsdk_
# (monocypher/rcsdk_monocypher_names.h). See monocypher/README.rcsdk.md.
set(monocypher_dir "${CMAKE_CURRENT_SOURCE_DIR}/monocypher")
add_library(rcsdk_monocypher OBJECT "${monocypher_dir}/monocypher.c" "${monocypher_dir}/monocypher-ed25519.c")
set_target_properties(rcsdk_monocypher PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden)
if(MSVC)
    target_compile_options(rcsdk_monocypher PRIVATE "/FI${monocypher_dir}/rcsdk_monocypher_names.h")
else()
    target_compile_options(rcsdk_monocypher PRIVATE "SHELL:-include \"${monocypher_dir}/rcsdk_monocypher_names.h\"" -w)
endif()

# BearSSL (cryptography and TLS where the system has none: RCSDK_CRYPTO_BACKEND
# bearssl), a static library the SDK links, so that only what it calls goes
# in: every symbol hidden and renamed with the prefix rcsdk_
# (bearssl/rcsdk_bearssl_names.h). Code built on it is told so by
# RCSDK_CRYPTO_BEARSSL. Every source of BearSSL is compiled, with its own
# defaults. See bearssl/README.rcsdk.md.
if(RCSDK_CRYPTO_BACKEND STREQUAL "bearssl")
    set(bearssl_dir "${CMAKE_CURRENT_SOURCE_DIR}/bearssl")
    file(GLOB_RECURSE bearssl_sources "${bearssl_dir}/src/*.c")
    add_library(rcsdk_bearssl STATIC ${bearssl_sources})
    set_target_properties(rcsdk_bearssl PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
    target_include_directories(rcsdk_bearssl
        PUBLIC "$<BUILD_INTERFACE:${bearssl_dir}/inc>"
        PRIVATE "${bearssl_dir}/src")
    target_compile_definitions(rcsdk_bearssl INTERFACE RCSDK_CRYPTO_BEARSSL)
    target_compile_options(rcsdk_bearssl PRIVATE "SHELL:-include \"${bearssl_dir}/rcsdk_bearssl_names.h\"" -w)
    # Optimized in Debug builds too: unoptimized, its constant-time code is
    # many times slower (a password's 600 000 PBKDF2 rounds take seconds).
    target_compile_options(rcsdk_bearssl PRIVATE $<$<CONFIG:Debug>:-O2>)
    if(NOT BUILD_SHARED_LIBS) # A static core is linked with it.
        install(TARGETS rcsdk_bearssl EXPORT RemoteControlSDKTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
endif()

# The codec libraries (RCSDK_WITH_*), each a static library of its own with
# its symbols hidden and renamed with the prefix rcsdk_. See each one's
# README.rcsdk.md.
if(RCSDK_WITH_OPENH264)
    add_subdirectory(openh264)
endif()
if(RCSDK_WITH_LIBVPX)
    add_subdirectory(libvpx)
endif()
if(RCSDK_WITH_DAV1D)
    add_subdirectory(dav1d)
endif()
if(RCSDK_WITH_LIBAOM)
    add_subdirectory(libaom)
endif()
