# libFuzzer targets (RCSDK_BUILD_FUZZERS): stateful ones, each driving one
# piece of the SDK through the steps its input spells out (see each file's
# head). Not tests: run by hand, docs/BUILDING.md (模糊测试).
function(rcsdk_fuzz_target name)
    add_executable(rcsdk_fuzz_${name} ${name}.cpp)
    target_link_libraries(rcsdk_fuzz_${name} PRIVATE ${ARGN})
    target_link_options(rcsdk_fuzz_${name} PRIVATE -fsanitize=fuzzer)
endfunction()

rcsdk_fuzz_target(udp_video rcsdk_internal)
rcsdk_fuzz_target(secure_channel rcsdk_internal)
rcsdk_fuzz_target(file_transfer rcsdk_file_transfer_internal)
# The forwarder's entries (server/forwarder), where it is built.
if(TARGET rcsfwd_objects)
    rcsdk_fuzz_target(forwarder_entry rcsfwd_objects)
endif()
