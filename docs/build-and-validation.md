# C++23 build and validation

## Supported toolchain

- GCC 14+ or Clang 19+ with a C++23 standard library providing `std::expected`
- CMake 3.25+, Ninja, pkgconf
- Qt 6.8+: Core, Gui, OpenGL, Quick, Qml, QuickControls2, Network, Sql, Concurrent
- Qt Test and QuickTest for the test suite
- Qt QML imports: QtQuick Controls/Dialogs/Layouts, QtQml WorkerScript, QtCore, QtTest
- QSQLITE driver and dynamically linked libmpv
- Python 3.11+ for deterministic third-party notice generation

The supported application is one native executable, `niconeon` (`niconeon.exe` on Windows).
CMake is the only build entry point. No generated RPC bridge or separate core executable is needed.

## Linux (Debian 13)

```sh
sudo apt-get install g++ cmake ninja-build pkgconf qt6-base-dev qt6-declarative-dev \
  libqt6sql6-sqlite libmpv-dev qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
  qml6-module-qtquick-layouts qml6-module-qtqml-workerscript qml6-module-qtcore \
  qml6-module-qttest fonts-dejavu-core fonts-noto-cjk
cmake --preset debug
cmake --build --preset debug --parallel
ctest --preset debug
./build/debug/niconeon
```

An existing workspace-local toolchain can be selected by sourcing its environment script before
these commands. Neither the project nor presets hard-code an agent/container path. For arbitrary
Qt prefixes set `CMAKE_PREFIX_PATH`; make pkgconf resolve the corresponding mpv installation.

## Windows

Use an MSYS2 **UCRT64** shell, not MINGW64 and not MSVC. Install the
`mingw-w64-ucrt-x86_64-` packages for gcc, cmake, ninja, pkgconf, qt6-base,
qt6-declarative, qt6-tools and mpv. Run the `windows-debug` or `windows-release`
configure/build/test presets; their outputs are in `build/windows-debug` and `build/windows-release`.
The Qt, libmpv, C++ runtime and application must all use the same UCRT64 toolchain.

## Presets and test layers

```sh
cmake --workflow --preset debug
cmake --workflow --preset release
cmake --workflow --preset asan-ubsan
```

`debug` and `release` build both application and tests. To isolate domain/render tests while
working on application code, configure with `-DNICONEON_BUILD_APP=OFF` in a separate build directory.

Explicit platform aliases are also provided: `linux-debug` / `linux-release` share the
`build/debug` / `build/release` directories, while `windows-debug` / `windows-release` have
platform-named build directories. Platform presets are available only on the matching host OS.

The default unit and QML tests use the offscreen Qt platform. They exercise application logic,
SQLite, rendering data and controls but do not establish that a hardware/OpenGL playback path works.

The actual OpenGL renderer integration test is opt-in and requires a working display:

```sh
cmake --preset debug -DNICONEON_BUILD_UI_E2E=ON
cmake --build --preset debug --parallel
xvfb-run -a -s '-screen 0 1280x1024x24 -ac' ctest --preset debug -L opengl
```

A missing or blocked display is an unverified integration stage; passing software/offscreen tests
must not be reported as GPU/OpenGL validation. Linux CI runs this separately using Mesa + Xvfb.

## Sanitizers and analysis

`asan-ubsan` combines AddressSanitizer and UndefinedBehaviorSanitizer, retains frame pointers and
stops on the first finding. Leak detection remains enabled. Qt and platform libraries are usually
uninstrumented; distinguish application findings from verified third-party shutdown allocations
before adding a narrow suppression.

`tsan` is separate because ThreadSanitizer cannot be combined with AddressSanitizer. It is intended
for worker/cancellation tests; availability depends on the host's virtual-memory layout and Qt
runtime. A TSan startup failure (for example "unexpected memory mapping") is a tool/runtime limit,
not a passing race check. Do not disable ASLR or other security settings to force it to run.

```sh
cmake --preset tsan
cmake --build --preset tsan --parallel
ctest --preset tsan -L unit
cmake --preset tidy
cmake --build --preset tidy --parallel
cmake --build build/tidy --target format-check
```

clang-format 19 and clang-tidy 19 are the reference tools. Format checks cover maintained C++
source/tests, not generated CMake/Qt output. Compiler warnings are enabled on all project libraries.

## Packaging

```sh
scripts/release/generate_third_party_notices.sh
scripts/release/package_linux.sh VERSION
scripts/release/package_linux_appimage.sh VERSION
# UCRT64 shell:
NICONEON_BUILD_DIR=build/windows-release scripts/release/package_windows_msys2.sh VERSION
```

All scripts take `NICONEON_BUILD_DIR` (default `build/release`). The Linux ZIP contains the
application and license/source metadata and requires compatible system Qt/libmpv libraries.
AppImage and Windows packaging collect dynamic dependencies. AppImage `AppRun` sets local library,
plugin and QML paths and executes only `usr/bin/niconeon`. The Windows bundle includes `qt.conf`,
Qt QML imports, the SQLite driver, libmpv and its recursive UCRT64 dependency tree.

The checked-in notices describe direct runtime components. A release distributor must also retain
the exact bundled native packages' notices and source obligations. Source archives use the selected
Git commit; commit the intended source before producing a release archive.

## Instrumentation caveats observed during migration

On Debian 13 / Qt 6.8.2, the four domain/filter/timeline/storage suites pass ThreadSanitizer.
Network/worker/controller suites stop on `eventfd` synchronization reports within the distribution's
uninstrumented Qt/GLib event dispatcher. Selecting Qt's UNIX dispatcher with `QT_NO_GLIB=1` yields
similar reports from `QCoreApplication::postEvent`. These runs are inconclusive for a clean worker
race check; they are not suppressed or represented as a pass. A fully TSan-instrumented Qt runtime
is the next step for conclusive investigation.

The command executor also runs under tracing, so LeakSanitizer terminates with its documented
`ptrace` limitation there. AddressSanitizer and UBSan checks can run with local `detect_leaks=0`;
the checked-in preset keeps leak detection enabled for normal desktop/CI processes.

## Release-publication boundary

This migration validates build, test and distribution packaging through read-only repository
permissions and workflow artifact uploads. The manual `Release Rebuild` workflow now stops at
those artifacts; it does not create or update a GitHub Release.

The existing tag-release promotion workflow (`release.yml`) remains byte-identical to the baseline.
That publication path has not been fully qualified for this migration. Actual release publication
requires separate authorization and end-to-end release validation; successful PR packaging alone
is not a release-promotion approval.
