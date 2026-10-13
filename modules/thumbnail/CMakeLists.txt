# Thumbnails of what a sharing session would share, taken where its agent
# runs (rcsdk/thumbnail.h), "rcsdk.thumbnail".
function(rcsdk_thumbnail_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_thumbnail src/thumbnail.cpp)
add_library(RemoteControl::Thumbnail ALIAS rcsdk_thumbnail)
set_target_properties(rcsdk_thumbnail PROPERTIES
    EXPORT_NAME Thumbnail
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_thumbnail
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_thumbnail PUBLIC rcsdk_core PRIVATE Threads::Threads)
target_compile_definitions(rcsdk_thumbnail PRIVATE RCSDK_BUILDING_THUMBNAIL)
rcsdk_thumbnail_warnings(rcsdk_thumbnail)
install(TARGETS rcsdk_thumbnail EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The same, beside the SDK's internal objects, for tests that reach both.
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_library(rcsdk_thumbnail_internal STATIC src/thumbnail.cpp)
    target_include_directories(rcsdk_thumbnail_internal PUBLIC include)
    target_link_libraries(rcsdk_thumbnail_internal PUBLIC rcsdk_internal)
    rcsdk_thumbnail_warnings(rcsdk_thumbnail_internal)
endif()
