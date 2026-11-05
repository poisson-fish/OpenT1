# Third-party dependencies

OpenT1 uses the project license in [LICENSE](LICENSE). The following dependencies
and their included components retain their own licenses. These notices describe
the pinned source builds; an installed SDL3 package can use different inputs.

| Dependency | Build use | License notices |
| --- | --- | --- |
| [SDL3 3.4.2](https://libsdl.org/release/SDL3-3.4.2.tar.gz) | Windowing, input, GPU presentation; fetched when no installed SDL3 3.2+ package is selected | [zlib license](LICENSES/SDL3.txt) |
| [OpenAL Soft 1.25.2](https://openal-soft.org/openal-releases/openal-soft-1.25.2.tar.bz2) | Audio; built as a shared library when audio is enabled | [Library GPL v2](LICENSES/OpenALSoft-LGPL-2.0.txt), version 2 or later as specified by its source notices |
| OpenAL Soft components | Spherical-harmonic and FFT code included in the dependency | [BSD 3-Clause notices](LICENSES/OpenALSoft-BSD-3-Clause.txt), [PFFFT notice](LICENSES/OpenALSoft-PFFFT.txt) |
| Microsoft GSL | Headers included in the OpenAL Soft source package | [MIT license](LICENSES/OpenALSoft-GSL.txt) |
| {fmt} 11.2.0 | Formatting fallback included in the OpenAL Soft source package | [MIT license and optional exception](LICENSES/OpenALSoft-fmt.txt) |
| OpenAL API headers | Public interface headers in the OpenAL Soft source package | [Unlicense notice](LICENSES/OpenAL-headers.txt) |
| LLVM libc++, libc++abi and libunwind 20 | Homebrew C++ runtime libraries collected for macOS binary packages | [libc++](LICENSES/LLVM-libcxx.txt), [libc++abi](LICENSES/LLVM-libcxxabi.txt), [libunwind](LICENSES/LLVM-libunwind.txt): Apache 2.0 with LLVM exceptions and additional notices |

The library downloads are checked against the SHA-256 digests in
`CMakeLists.txt` and `cmake/OpenALSoft.cmake`. The source archives also contain
per-file notices, which remain part of those source distributions. Preserve
applicable notices when distributing dependencies or their source.

## SDL alteration

`cmake/SDL3EventWatch.cmake` generates an altered build-local copy of
`src/events/SDL_eventwatch.c` for the fetched SDL version. It preserves the
outer dispatch state during nested event-watch callbacks and delays compaction
until the outermost dispatch returns. The generated source identifies the
alteration and retains the upstream notice. The downloaded source tree is not
modified. Installed SDL packages bypass this alteration.

## OpenAL alteration

On macOS, `cmake/OpenALSoftCompat.cmake` generates an altered build-local copy
of OpenAL Soft's `alc/alc.cpp`. It uses a lambda to dereference effect slot
clusters, supporting libc++ 23's templated `unique_ptr::operator*`. The generated
source identifies the alteration and retains the upstream notice. The downloaded
source tree is not modified.

## Distribution

Keep these notices and their referenced license texts with binary distributions.
Include corresponding source, build inputs, and dependency alterations in the
manner required by the applicable licenses. OpenAL Soft remains a separate shared
library; copying its runtime library alone does not supply its license text or
source. Platform libraries selected by SDL or OpenAL depend on the configuration
and may have additional distribution requirements.

The Runtime install component and `tools/package_release.py` assemble explicit
binary inputs. CI ships `OpenT1-source.tar.gz` with the tracked engine sources,
exact original SDL/OpenAL source archives, and dependency alteration/build recipes
under `cmake/`. macOS packaging collects and repairs Homebrew C++ runtime library
references, retains the LLVM license texts above (from llvmorg-20.1.8), and ad-hoc
signs the changed Mach-O files. LLVM sources are available from
https://github.com/llvm/llvm-project/tree/llvmorg-20.1.8.
See [distribution inputs](docs/development.md#distribution-inputs) and the
[release guide](docs/wiki/Releasing.md) before publishing a release.
