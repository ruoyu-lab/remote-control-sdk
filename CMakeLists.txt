cmake_minimum_required(VERSION 3.25)
# The version is defined once, in the public header; editing it reconfigures.
file(READ "${CMAKE_CURRENT_LIST_DIR}/include/rcsdk/rcsdk.h" rcsdk_header)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/include/rcsdk/rcsdk.h")
foreach(part MAJOR MINOR PATCH)
    string(REGEX MATCH "#define RCSDK_VERSION_${part} ([0-9]+)u" rcsdk_match "${rcsdk_header}")
    list(APPEND rcsdk_version "${CMAKE_MATCH_1}")
endforeach()
list(JOIN rcsdk_version "." rcsdk_version)
# The release stage, "beta" before 0.1.0 is a release: in the version text only.
string(REGEX MATCH "#define RCSDK_VERSION_LABEL \"([a-z0-9]*)\"" rcsdk_match "${rcsdk_header}")
set(rcsdk_version_label "${CMAKE_MATCH_1}")
# The oldest system the library loads on: macOS with ScreenCaptureKit as the
# SDK uses it, iOS of the same year (VideoToolbox's hardware decoder choice);
# without it the build machine's own version would be the minimum.
# (A toolchain file, such as Qt's for iOS, says what it targets only at
# project(): without an iOS sysroot here it sets the minimum itself.)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_OSX_SYSROOT MATCHES "^iphone")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0" CACHE STRING "Minimum iOS version")
elseif(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")
endif()
project(RemoteControlSDK VERSION ${rcsdk_version} LANGUAGES CXX)
set(rcsdk_version_text "${PROJECT_VERSION}") # What rcs_version() says.
if(rcsdk_version_label)
    string(APPEND rcsdk_version_text "-${rcsdk_version_label}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

# iOS: static libraries (a dynamic one must be a framework there). Android:
# static too, linked into the application's own native library.
if(IOS OR ANDROID)
    option(BUILD_SHARED_LIBS "Build RemoteControl::Core as a shared library" OFF)
else()
    option(BUILD_SHARED_LIBS "Build RemoteControl::Core as a shared library" ON)
endif()
# Examples, tools and tests default on only when this is the top-level project,
# so add_subdirectory() from a host application builds just the library. The
# tools measure macOS and Windows; iOS and Android build none.
option(RCSDK_BUILD_EXAMPLES "Build the integration examples" ${PROJECT_IS_TOP_LEVEL})
if(PROJECT_IS_TOP_LEVEL AND NOT IOS AND NOT ANDROID)
    option(RCSDK_BUILD_TOOLS "Build the diagnostic and measurement tools" ON)
else()
    option(RCSDK_BUILD_TOOLS "Build the diagnostic and measurement tools" OFF)
endif()
option(RCSDK_BUILD_QML_DEMO "Build the Qt Quick demo with GPU video (requires Qt 6.6)" OFF)
# The forwarder (server/forwarder): a server beside the SDK, for Linux servers
# (where the SDK's ends are not supported, the forwarder is) and desktops.
if(PROJECT_IS_TOP_LEVEL AND NOT IOS AND NOT ANDROID AND NOT EMSCRIPTEN)
    option(RCSDK_BUILD_FORWARDER "Build the forwarder, RCS_TRANSPORT_FORWARDER's server" ON)
else()
    option(RCSDK_BUILD_FORWARDER "Build the forwarder, RCS_TRANSPORT_FORWARDER's server" OFF)
endif()
# Cryptography and TLS: the system's (Security, CommonCrypto and
# Network.framework on macOS and iOS; CNG and Schannel on Windows), or BearSSL
# (third_party/bearssl) where the system has none, as on Android.
# Apple systems build either, so that BearSSL can be tested on a Mac.
if(APPLE OR WIN32)
    set(rcsdk_crypto_default system)
else()
    set(rcsdk_crypto_default bearssl)
endif()
set(RCSDK_CRYPTO_BACKEND "${rcsdk_crypto_default}" CACHE STRING "Cryptography and TLS: system or bearssl")
set_property(CACHE RCSDK_CRYPTO_BACKEND PROPERTY STRINGS system bearssl)
if(NOT RCSDK_CRYPTO_BACKEND MATCHES "^(system|bearssl)$" OR (WIN32 AND RCSDK_CRYPTO_BACKEND STREQUAL "bearssl"))
    message(FATAL_ERROR "RCSDK_CRYPTO_BACKEND is system or bearssl, not \"${RCSDK_CRYPTO_BACKEND}\" (Windows: system)")
endif()

# The codec libraries built into the SDK (rcs_backend; docs/guide/13-tuning.md):
# software encoders and decoders the same on every
# system, each of which a build may leave out. Not in a browser, which
# decodes with WebCodecs. OpenH264 only where the system has no H.264 of its
# own (the systems without a platform layer); elsewhere it
# may be turned on.
if(EMSCRIPTEN)
    set(rcsdk_codec_default OFF)
else()
    set(rcsdk_codec_default ON)
endif()
if(APPLE OR WIN32 OR ANDROID OR EMSCRIPTEN)
    set(rcsdk_openh264_default OFF)
else()
    set(rcsdk_openh264_default ON)
endif()
option(RCSDK_WITH_LIBVPX "Build in libvpx: VP8 and VP9 encoding and decoding in software" ${rcsdk_codec_default})
option(RCSDK_WITH_DAV1D "Build in dav1d: AV1 decoding in software" ${rcsdk_codec_default})
option(RCSDK_WITH_LIBAOM "Build in libaom: AV1 encoding in software" ${rcsdk_codec_default})
option(RCSDK_WITH_OPENH264 "Build in OpenH264: H.264 encoding and decoding in software" ${rcsdk_openh264_default})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
find_package(Threads REQUIRED)
if(WIN32)
    add_compile_definitions(WINVER=0x0A00 _WIN32_WINNT=0x0A00 # Windows 10 APIs.
                            NOMINMAX) # std::min and std::max, not windows.h's macros.
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    enable_language(C) # The C ABI tests (the core's, the components') are compiled as C.
endif()
# The fuzz targets (tests/fuzz, libFuzzer): everything is built for them, with
# AddressSanitizer, UndefinedBehaviorSanitizer and libFuzzer's coverage, and
# RCSDK_FUZZING opens their ways in. Needs clang with libFuzzer (on macOS
# Homebrew's LLVM: Apple's clang has none) and BUILD_TESTING.
option(RCSDK_BUILD_FUZZERS "Build the fuzz targets (libFuzzer with ASan and UBSan; tests/fuzz)" OFF)
if(RCSDK_BUILD_FUZZERS)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fsanitize=address,undefined,fuzzer-no-link>"
                        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fno-sanitize-recover=undefined>"
                        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-fno-omit-frame-pointer>")
    add_link_options(-fsanitize=address,undefined)
    add_compile_definitions(RCSDK_FUZZING)
endif()
if(NOT APPLE AND NOT WIN32 AND NOT EMSCRIPTEN) # ELF: the shared libraries export rcs_* only.
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--version-script=${PROJECT_SOURCE_DIR}/cmake/exports.map")
endif()
add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(modules)
add_subdirectory(components)
if(EMSCRIPTEN)
    add_subdirectory(web) # The browser package.
endif()
# The core with every official module and component built.
add_library(rcsdk_all INTERFACE)
add_library(RemoteControl::All ALIAS rcsdk_all)
set_target_properties(rcsdk_all PROPERTIES EXPORT_NAME All)
target_link_libraries(rcsdk_all INTERFACE rcsdk_core ${rcsdk_module_targets} ${rcsdk_component_targets})
install(TARGETS rcsdk_all EXPORT RemoteControlSDKTargets)
if(RCSDK_BUILD_FORWARDER)
    add_subdirectory(server/forwarder)
endif()
if(RCSDK_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(RCSDK_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

configure_package_config_file(
    cmake/RemoteControlSDKConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteControlSDKConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RemoteControlSDK")
# Until 1.0 each minor version has an interface of its own.
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteControlSDKConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(EXPORT RemoteControlSDKTargets NAMESPACE RemoteControl::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RemoteControlSDK")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteControlSDKConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/RemoteControlSDKConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RemoteControlSDK")
