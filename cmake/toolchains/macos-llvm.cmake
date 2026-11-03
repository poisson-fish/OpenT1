# Use Homebrew LLVM and its matching libc++; Apple's library may lack the
# C++20 thread facilities required by OpenT1. Do not depend on a shell's exports.
if(NOT CMAKE_HOST_APPLE)
    message(FATAL_ERROR "The macos-llvm toolchain requires macOS")
endif()

# A custom installation can be selected with -DOPENT1_LLVM_ROOT=/path/to/llvm.
if(NOT OPENT1_LLVM_ROOT)
    find_program(_OPENT1_BREW brew HINTS /opt/homebrew/bin /usr/local/bin)
    if(_OPENT1_BREW)
        foreach(_formula IN ITEMS llvm llvm@20)
            execute_process(
                COMMAND "${_OPENT1_BREW}" --prefix "${_formula}"
                OUTPUT_VARIABLE _llvm_prefix
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _llvm_result
            )
            if(_llvm_result EQUAL 0 AND EXISTS "${_llvm_prefix}/bin/clang++")
                set(OPENT1_LLVM_ROOT "${_llvm_prefix}" CACHE PATH "LLVM installation prefix")
                break()
            endif()
        endforeach()
    endif()
endif()

if(NOT EXISTS "${OPENT1_LLVM_ROOT}/bin/clang"
   OR NOT EXISTS "${OPENT1_LLVM_ROOT}/bin/clang++"
   OR NOT EXISTS "${OPENT1_LLVM_ROOT}/include/c++/v1/stop_token"
   OR NOT EXISTS "${OPENT1_LLVM_ROOT}/lib/c++/libc++.dylib"
)
    message(
        FATAL_ERROR "Install Homebrew LLVM with 'brew install llvm', or set "
                    "OPENT1_LLVM_ROOT to an LLVM installation with matching libc++."
    )
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES OPENT1_LLVM_ROOT)

set(CMAKE_C_COMPILER "${OPENT1_LLVM_ROOT}/bin/clang" CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER "${OPENT1_LLVM_ROOT}/bin/clang++" CACHE FILEPATH "C++ compiler")
set(CMAKE_CXX_FLAGS_INIT "-nostdinc++ -isystem \"${OPENT1_LLVM_ROOT}/include/c++/v1\"")
foreach(_kind IN ITEMS EXE SHARED MODULE)
    set(
        CMAKE_${_kind}_LINKER_FLAGS_INIT
        "-L\"${OPENT1_LLVM_ROOT}/lib/c++\" -Wl,-rpath,\"${OPENT1_LLVM_ROOT}/lib/c++\""
    )
endforeach()

# Pass an explicit SDK to both Clang and clang-scan-deps (used by OpenAL).
if(NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND /usr/bin/xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE _opent1_sdk
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(CMAKE_OSX_SYSROOT "${_opent1_sdk}" CACHE PATH "macOS SDK")
endif()
