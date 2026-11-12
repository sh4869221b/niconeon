# Source Code Availability

Niconeon binary distributions are provided under GNU GPL v3.0 or later (`COPYING`).

The complete corresponding source code is available from this repository:

- Repository: https://github.com/sh4869221b/niconeon
- Tagged release source archives: `niconeon-X.Y.Z-source.zip` in each GitHub Release

To build from source, follow the instructions in `README.md`.

This file is included in binary distributions to document where the corresponding source can be obtained.

## libmpv corresponding source

AppImage and Windows distributions include libmpv 0.42.0, which includes upstream commit
72d43dc9c999a21d867cdc0f934f3e4cd2195aa9 (scaler LUT padding initialization).
The exact source archive, Linux static-libplacebo integration patch, build script, license
information, and built-runtime SHA-256 are included in the distribution's
`libmpv-source` license directory. The build script verifies the archive checksum;
packaging checks that the actual runtime matches this build before deployment.
See `scripts/deps/build_mpv.sh` and `docs/build-and-validation.md` in this repository.

## Linux libplacebo corresponding source

Linux AppImages include libplacebo 7.387.0 statically linked inside libmpv from its checksum-verified upstream
source because Debian 13 ships an older version than mpv 0.42.0 requires. The
`libplacebo-source` license directory retains its exact source archive, build script,
license notices, the matching upstream Vulkan-Headers source archive and licenses,
and static-library hash. Packaging verifies the static library
and the separately recorded libmpv runtime hash before deployment. Linux symbols
are hidden inside libmpv to avoid interposing on an older libplacebo used by FFmpeg. See `scripts/deps/build_libplacebo.sh`.

## Linux Qt corresponding source

Linux AppImages include a shared Qt 6.12.0 SDK built from the official QtBase,
QtShaderTools, QtSVG and QtDeclarative archives, with the applicable official
CVE-2026-105924 and CVE-2026-116337 patches. The `qt-source` license directory
retains the exact archives, patches, verified checksums, build script, module
license texts, configuration and SDK runtime hashes. Packaging checks that the
application uses that SDK before deployment tools rewrite library paths.
See `scripts/deps/build_qt.sh` and `scripts/deps/qt-sources.sha256`.

## Linux FFmpeg and subtitle/color libraries

Linux AppImages include FFmpeg 9.0.2, libass 0.17.5 and LittleCMS 2.20 built in a
separate prefix on Debian 13. `ffmpeg-source` and `media-support-source` retain the
original release archives, license texts, build instructions/configuration and
runtime hashes. FFmpeg's baseline and resulting feature inventories are retained;
packaging refuses an unverified or mismatched runtime. FFmpeg also links the same
private static libplacebo, whose source bundle is retained separately.
Other system libraries retain their own distributor license/source obligations.

## Windows QtBase security rebuild

Windows bundles retain `windows-qt-source`: the exact official MSYS2 QtBase
6.12.0-1 recipe and platform patches, upstream QtBase archive, the three official
CVE-2026-105924/CVE-2026-116337 patches, original/modified recipes, license texts,
build configuration/log, package inventory and pristine installed DLL hashes.
This is a local UCRT64 security rebuild, not a binary published by MSYS2. The
matching QtDeclarative/Tools packages remain unchanged; the fixes affect QtCore
and private QtNetwork internals without changing their public ABI.
