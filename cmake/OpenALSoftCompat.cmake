# OpenAL 1.25.2 takes the address of unique_ptr::operator*. libc++ 23 makes
# that member a template, so use a lambda with the same reference semantics.
# Compile a build-local copy and preserve the pinned reference source.
set(alsoft_original "${openal_soft_SOURCE_DIR}/alc/alc.cpp")
file(SHA256 "${alsoft_original}" alsoft_hash)
if(NOT alsoft_hash STREQUAL "12846e8c1bce29aa157b562540c73f446f8fa7fc64255faae8e26c46f416c41e")
    message(FATAL_ERROR "Bundled OpenAL source changed; review the libc++ compatibility correction")
endif()
file(READ "${alsoft_original}" alsoft_source)
string(
    REPLACE
        "std::views::transform(&ContextBase::EffectSlotCluster::operator*)"
        "std::views::transform([](const ContextBase::EffectSlotCluster &cluster) -> auto& { return *cluster; })"
        alsoft_source "${alsoft_source}"
)
set(alsoft_source
    "/* Altered by OpenT1: use a lambda to dereference effect slot clusters with libc++ 23. */\n${alsoft_source}"
)
set(alsoft_patched "${CMAKE_CURRENT_BINARY_DIR}/openal-patches/alc.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/openal-patches")
file(CONFIGURE OUTPUT "${alsoft_patched}" CONTENT "${alsoft_source}" @ONLY)

get_target_property(alsoft_sources OpenAL SOURCES)
list(TRANSFORM alsoft_sources REPLACE "^(${openal_soft_SOURCE_DIR}/)?alc/alc\\.cpp$"
                                     "${alsoft_patched}"
)
if(NOT alsoft_patched IN_LIST alsoft_sources)
    message(FATAL_ERROR "Cannot locate the bundled OpenAL compile input")
endif()
set_property(TARGET OpenAL PROPERTY SOURCES "${alsoft_sources}")
# Preserve the original file's relative include lookup after relocating it.
target_include_directories(OpenAL PRIVATE "${openal_soft_SOURCE_DIR}/alc")
