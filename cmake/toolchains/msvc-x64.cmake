# cmake/toolchains/msvc-x64.cmake
#
# Standalone 64-bit x64 MSVC + Windows SDK toolchain file for OpenT1 (C++20).
# Automatically locates Hostx64/x64 cl.exe, link.exe, lib.exe and the
# Windows 10/11 SDK so CMake + Ninja can configure and build from any shell
# without requiring vcvarsall.bat to be sourced beforehand.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Locate latest MSVC Hostx64/x64 toolset
file(GLOB _MSVC_CL_CANDIDATES
     "C:/Program Files/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"
     "C:/Program Files (x86)/Microsoft Visual Studio/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe"
)
list(SORT _MSVC_CL_CANDIDATES)
list(REVERSE _MSVC_CL_CANDIDATES)
list(GET _MSVC_CL_CANDIDATES 0 _MSVC_CL_EXE)

get_filename_component(_MSVC_BIN_DIR "${_MSVC_CL_EXE}" DIRECTORY)
get_filename_component(_MSVC_HOST_DIR "${_MSVC_BIN_DIR}" DIRECTORY)
get_filename_component(_MSVC_BIN_ROOT "${_MSVC_HOST_DIR}" DIRECTORY)
get_filename_component(_MSVC_TOOLS_DIR "${_MSVC_BIN_ROOT}" DIRECTORY)

set(_MSVC_INC_DIR "${_MSVC_TOOLS_DIR}/include")
set(_MSVC_LIB_DIR "${_MSVC_TOOLS_DIR}/lib/x64")
set(_MSVC_ATLMFC_INC_DIR "${_MSVC_TOOLS_DIR}/atlmfc/include")

# Locate latest Windows 10/11 SDK
file(GLOB _WINSDK_UCRT_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/Include/*/ucrt")
list(SORT _WINSDK_UCRT_CANDIDATES)
list(REVERSE _WINSDK_UCRT_CANDIDATES)
list(GET _WINSDK_UCRT_CANDIDATES 0 _WINSDK_UCRT_INC)

get_filename_component(_WINSDK_VER_INC_DIR "${_WINSDK_UCRT_INC}" DIRECTORY)
get_filename_component(_WINSDK_VERSION "${_WINSDK_VER_INC_DIR}" NAME)
get_filename_component(_WINSDK_INC_ROOT "${_WINSDK_VER_INC_DIR}" DIRECTORY)
get_filename_component(_WINSDK_ROOT "${_WINSDK_INC_ROOT}" DIRECTORY)

set(_WINSDK_UM_INC "${_WINSDK_ROOT}/Include/${_WINSDK_VERSION}/um")
set(_WINSDK_SHARED_INC "${_WINSDK_ROOT}/Include/${_WINSDK_VERSION}/shared")
set(_WINSDK_WINRT_INC "${_WINSDK_ROOT}/Include/${_WINSDK_VERSION}/winrt")
set(_WINSDK_UCRT_LIB "${_WINSDK_ROOT}/Lib/${_WINSDK_VERSION}/ucrt/x64")
set(_WINSDK_UM_LIB "${_WINSDK_ROOT}/Lib/${_WINSDK_VERSION}/um/x64")
set(_WINSDK_BIN_X64 "${_WINSDK_ROOT}/bin/${_WINSDK_VERSION}/x64")

set(CMAKE_C_COMPILER
    "${_MSVC_BIN_DIR}/cl.exe"
    CACHE FILEPATH "C compiler"
)
set(CMAKE_CXX_COMPILER
    "${_MSVC_BIN_DIR}/cl.exe"
    CACHE FILEPATH "CXX compiler"
)
set(CMAKE_AR
    "${_MSVC_BIN_DIR}/lib.exe"
    CACHE FILEPATH "Archiver"
)
set(CMAKE_LINKER
    "${_MSVC_BIN_DIR}/link.exe"
    CACHE FILEPATH "Linker"
)
set(CMAKE_RC_COMPILER
    "${_WINSDK_BIN_X64}/rc.exe"
    CACHE FILEPATH "Resource compiler"
)
set(CMAKE_MT
    "${_WINSDK_BIN_X64}/mt.exe"
    CACHE FILEPATH "Manifest tool"
)

# Populate environment for try_compile steps during CMake configuration
set(ENV{PATH} "${_MSVC_BIN_DIR};${_WINSDK_BIN_X64};$ENV{PATH}")
set(ENV{INCLUDE}
    "${_MSVC_INC_DIR};${_WINSDK_UCRT_INC};${_WINSDK_UM_INC};${_WINSDK_SHARED_INC};${_WINSDK_WINRT_INC}"
)
set(ENV{LIB} "${_MSVC_LIB_DIR};${_WINSDK_UCRT_LIB};${_WINSDK_UM_LIB}")

# Persist system include and library search paths into generated Ninja build rules
set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES
    "${_MSVC_INC_DIR}" "${_WINSDK_UCRT_INC}" "${_WINSDK_UM_INC}" "${_WINSDK_SHARED_INC}"
    "${_WINSDK_WINRT_INC}"
)
set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
    "${_MSVC_INC_DIR}" "${_WINSDK_UCRT_INC}" "${_WINSDK_UM_INC}" "${_WINSDK_SHARED_INC}"
    "${_WINSDK_WINRT_INC}"
)
if(EXISTS "${_MSVC_ATLMFC_INC_DIR}")
    list(APPEND CMAKE_C_STANDARD_INCLUDE_DIRECTORIES "${_MSVC_ATLMFC_INC_DIR}")
    list(APPEND CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${_MSVC_ATLMFC_INC_DIR}")
endif()

set(_SYS_LIBPATHS
    "/LIBPATH:\"${_MSVC_LIB_DIR}\" /LIBPATH:\"${_WINSDK_UCRT_LIB}\" /LIBPATH:\"${_WINSDK_UM_LIB}\""
)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_SYS_LIBPATHS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_SYS_LIBPATHS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_SYS_LIBPATHS}")

set(_RC_INCS
    "-I\"${_MSVC_INC_DIR}\" -I\"${_WINSDK_UCRT_INC}\" -I\"${_WINSDK_UM_INC}\" -I\"${_WINSDK_SHARED_INC}\""
)
if(EXISTS "${_MSVC_ATLMFC_INC_DIR}")
    set(_RC_INCS "${_RC_INCS} -I\"${_MSVC_ATLMFC_INC_DIR}\"")
endif()
set(CMAKE_RC_FLAGS_INIT "/nologo ${_RC_INCS}")
