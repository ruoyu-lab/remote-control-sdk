# The browser package (WebAssembly builds only): rcsdk_core.mjs and .wasm,
# the module web/rcsdk.mjs loads, next to rcsdk.mjs itself in
# <build>/web/. docs/guide/23-browser.md.
add_executable(rcsdk_web glue.cpp)
# The official modules a page can use: the clipboard and file transfer (target HOST).
target_link_libraries(rcsdk_web PRIVATE RemoteControl::Core RemoteControl::Clipboard RemoteControl::FileTransfer)
set_target_properties(rcsdk_web PROPERTIES
    OUTPUT_NAME rcsdk_core
    SUFFIX ".mjs"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")
# A library for the page, not a program: no main on a thread of its own, the
# runtime stays (the tests' settings are undone here; later -s settings win).
target_link_options(rcsdk_web PRIVATE
    -sPROXY_TO_PTHREAD=0 -sEXIT_RUNTIME=0 -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createRcsdk
    -sENVIRONMENT=web,worker -sPTHREAD_POOL_SIZE=24 -sINITIAL_MEMORY=128MB -sALLOW_MEMORY_GROWTH=1
    "-sEXPORTED_FUNCTIONS=_malloc,_free"
    "-sEXPORTED_RUNTIME_METHODS=HEAPU8,UTF8ToString,stringToNewUTF8"
    "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/pre.js")
set_property(TARGET rcsdk_web APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/pre.js")
# The package's own JavaScript, copied whenever it changes.
set(rcsdk_web_scripts rcsdk.mjs rcsdk_audio.js)
set(rcsdk_web_copies)
foreach(script IN LISTS rcsdk_web_scripts)
    add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/web/${script}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/${script}" "${CMAKE_BINARY_DIR}/web/${script}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${script}")
    list(APPEND rcsdk_web_copies "${CMAKE_BINARY_DIR}/web/${script}")
endforeach()
add_custom_target(rcsdk_web_scripts ALL DEPENDS ${rcsdk_web_copies})
add_dependencies(rcsdk_web rcsdk_web_scripts)
