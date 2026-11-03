# OpenAL Soft is pinned even when a system OpenAL implementation exists.
# Build a replaceable shared library; license notices are in LICENSES/.
include(FetchContent)
set(ALSOFT_UTILS
    OFF
    CACHE BOOL "" FORCE
)
set(ALSOFT_EXAMPLES
    OFF
    CACHE BOOL "" FORCE
)
set(ALSOFT_TESTS
    OFF
    CACHE BOOL "" FORCE
)
set(ALSOFT_INSTALL
    OFF
    CACHE BOOL "" FORCE
)
set(LIBTYPE
    SHARED
    CACHE STRING "" FORCE
)
fetchcontent_declare(
    openal_soft
    URL https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2
    URL_HASH SHA256=1dbaac44e7579d5bc8847ca8db4b2e8b9fd3961041f35ee20def4958301e1089
    DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TIMEOUT 120
    INACTIVITY_TIMEOUT 30
)
fetchcontent_makeavailable(openal_soft)
if(APPLE)
    include(cmake/OpenALSoftCompat.cmake)
    # Homebrew backends can add include roots containing a different fmt.
    # Use bundled headers to match the fmt library OpenAL links against.
    foreach(_target IN ITEMS OpenAL alsoft.common alsoft.core-modules)
        if(TARGET ${_target})
            target_include_directories(
                ${_target} BEFORE PRIVATE "$<TARGET_PROPERTY:alsoft::fmt,INTERFACE_INCLUDE_DIRECTORIES>"
            )
        endif()
    endforeach()
    # OpenAL 1.25.2 predates Apple's nonblocking CoreAudio callback annotations.
    # Keep the diagnostic as a warning, matching upstream's 681d049 fix:
    # https://github.com/kcat/openal-soft/commit/681d049c06c65a346f8c77c9f4deecee64cda352
    if(HAVE_WFUNCTION_EFFECTS)
        target_compile_options(OpenAL PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=function-effects>)
    endif()
endif()
add_library(
    opent1_audio STATIC src/audio/wave.cpp src/audio/openal_audio.cpp src/audio/audio_runtime.cpp
)
target_link_libraries(
    opent1_audio
    PUBLIC opent1_math
    PRIVATE OpenAL::OpenAL
)
add_library(opent1_audio_sim STATIC src/sim/sim_audio.cpp)
target_link_libraries(opent1_audio_sim PUBLIC opent1_audio opent1_sim)
