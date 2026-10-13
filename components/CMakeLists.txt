# Official components: libraries beside the core for work that is no part of
# a session, built from the public headers alone (rcsdk/supervisor.h). A host
# links those it uses; RemoteControl::All has every one built.
set(RCSDK_COMPONENTS "all" CACHE STRING "The official components to build: all, or some of supervisor")
set(components ${RCSDK_COMPONENTS})
if(components STREQUAL "all")
    set(components supervisor)
endif()
set(targets "")
foreach(component IN LISTS components)
    add_subdirectory(${component})
    list(APPEND targets rcsdk_${component})
endforeach()
set(rcsdk_component_targets ${targets} PARENT_SCOPE)
