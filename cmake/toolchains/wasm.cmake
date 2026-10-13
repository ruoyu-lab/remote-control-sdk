# WebAssembly with threads (pthreads on SharedArrayBuffer), from the
# Emscripten SDK: EMSDK (environment), else ~/emsdk. What the browser viewer
# is built with (docs/guide/23-browser.md); the tests run in the
# SDK's own node (CMAKE_CROSSCOMPILING_EMULATOR).
foreach(candidate "$ENV{EMSDK}" "$ENV{HOME}/emsdk")
    if(candidate AND EXISTS "${candidate}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
        set(rcsdk_emsdk "${candidate}")
        break()
    endif()
endforeach()
if(NOT rcsdk_emsdk)
    message(FATAL_ERROR "No Emscripten SDK found: set EMSDK to it")
endif()
# Set before Emscripten's file, which keeps an emulator already chosen.
file(GLOB rcsdk_emsdk_nodes "${rcsdk_emsdk}/node/*/bin/node")
if(rcsdk_emsdk_nodes)
    list(GET rcsdk_emsdk_nodes 0 rcsdk_emsdk_node)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${rcsdk_emsdk_node}" CACHE FILEPATH "Runs the tests")
endif()
include("${rcsdk_emsdk}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")

# Threads everywhere, and WebAssembly's own exceptions. Programs (the tests)
# run main() on a thread of their own, so that the runtime's main thread,
# which starts threads and carries out proxied calls, never waits.
set(CMAKE_C_FLAGS_INIT "-pthread -fwasm-exceptions")
set(CMAKE_CXX_FLAGS_INIT "-pthread -fwasm-exceptions")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-pthread -fwasm-exceptions -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=48 -sALLOW_MEMORY_GROWTH=1 \
-Wno-pthreads-mem-growth -sINITIAL_MEMORY=256MB -sSTACK_SIZE=1MB -sDEFAULT_PTHREAD_STACK_SIZE=1MB -sEXIT_RUNTIME=1")
