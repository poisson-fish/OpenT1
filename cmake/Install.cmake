# Release staging is an explicit component; never copy the build tree.
install(TARGETS opent1 opent1_asset_tool OpenAL
    RUNTIME DESTINATION . COMPONENT Runtime
    LIBRARY DESTINATION . COMPONENT Runtime)
if(TARGET SDL3::SDL3-shared)
    install(IMPORTED_RUNTIME_ARTIFACTS SDL3::SDL3-shared
        RUNTIME DESTINATION . COMPONENT Runtime LIBRARY DESTINATION . COMPONENT Runtime)
endif()
if(UNIX AND NOT APPLE)
    set_target_properties(opent1 OpenAL PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION . COMPONENT Runtime)
endif()
install(FILES LICENSE THIRD_PARTY.md VERSION DESTINATION . COMPONENT Runtime)
install(FILES docs/release-readme.md DESTINATION . RENAME README.md COMPONENT Runtime)
install(DIRECTORY LICENSES/ DESTINATION LICENSES COMPONENT Runtime)
install(DIRECTORY docs/wiki/ DESTINATION docs COMPONENT Runtime FILES_MATCHING PATTERN "*.md")
install(DIRECTORY examples/scripts/ DESTINATION examples/scripts COMPONENT Runtime)
if(APPLE)
    configure_file("${CMAKE_CURRENT_LIST_DIR}/FixupMacOS.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/FixupMacOS.cmake" @ONLY)
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/FixupMacOS.cmake" COMPONENT Runtime)
endif()
