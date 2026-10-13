# Official modules (rcsdk/module.h): each a library of its own beside the
# core, built from the public headers alone, as anyone's module would be.
# A host links and registers those it uses; RemoteControl::All has every one
# built.
set(RCSDK_MODULES "all" CACHE STRING
    "The official modules to build: all, or some of clipboard;file_transfer;power;text_caret;thumbnail")
set(modules ${RCSDK_MODULES})
if(modules STREQUAL "all")
    set(modules clipboard file_transfer power text_caret thumbnail)
endif()
# The clipboard carries copies of files through file transfer.
if("clipboard" IN_LIST modules AND NOT "file_transfer" IN_LIST modules)
    list(APPEND modules file_transfer)
endif()
set(targets "")
foreach(module IN LISTS modules)
    add_subdirectory(${module})
    list(APPEND targets rcsdk_${module})
endforeach()
set(rcsdk_module_targets ${targets} PARENT_SCOPE)
