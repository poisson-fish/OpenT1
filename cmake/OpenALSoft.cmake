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
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
fetchcontent_makeavailable(openal_soft)
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
