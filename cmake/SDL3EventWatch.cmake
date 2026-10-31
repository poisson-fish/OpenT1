# OpenT1 alteration to pinned SDL 3.4.2: nested window-message dispatch must
# preserve the outer dispatching flag. Otherwise removal compacts a list whose
# outer iterator still holds its old count, replaying retired GPU/window watches.
# Generate a build-local source; never change the downloaded reference tree.
set(eventwatch_original "${sdl3_SOURCE_DIR}/src/events/SDL_eventwatch.c")
file(SHA256 "${eventwatch_original}" eventwatch_hash)
if(NOT eventwatch_hash STREQUAL "7341a7fe299706b15fac6728932216a2315f5ec401bbfda27477ef31b29b984c")
    message(
        FATAL_ERROR "Bundled SDL event-watch source changed; review the nested-dispatch correction"
    )
endif()
file(READ "${eventwatch_original}" eventwatch_source)
string(
    REPLACE
        "int i, count = list->count;"
        "int i, count = list->count;\n        const bool outer_dispatching = list->dispatching; // OpenT1: preserve nested dispatch"
        eventwatch_source
        "${eventwatch_source}"
)
string(
    REPLACE
        "list->dispatching = false;\n\n        if (list->removed)"
        "list->dispatching = outer_dispatching;\n\n        if (!list->dispatching && list->removed)"
        eventwatch_source "${eventwatch_source}"
)
set(eventwatch_patched "${CMAKE_CURRENT_BINARY_DIR}/sdl-patches/SDL_eventwatch.c")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdl-patches")
set(eventwatch_source
    "/* Altered by OpenT1: defer watch compaction until the outermost dispatch returns. */\n${eventwatch_source}"
)
# configure_file preserves the timestamp when the generated content is equal.
file(CONFIGURE OUTPUT "${eventwatch_patched}" CONTENT "${eventwatch_source}" @ONLY)
foreach(sdl_target IN ITEMS SDL3-static SDL3-shared)
    if(TARGET ${sdl_target})
        get_target_property(eventwatch_sources ${sdl_target} SOURCES)
        list(TRANSFORM eventwatch_sources REPLACE "^${eventwatch_original}$"
                                                  "${eventwatch_patched}"
        )
        if(NOT eventwatch_patched IN_LIST eventwatch_sources)
            message(FATAL_ERROR "Cannot locate the bundled SDL event-watch compile input")
        endif()
        set_property(TARGET ${sdl_target} PROPERTY SOURCES "${eventwatch_sources}")
        target_include_directories(${sdl_target} PRIVATE "${sdl3_SOURCE_DIR}/src/events")
    endif()
endforeach()
