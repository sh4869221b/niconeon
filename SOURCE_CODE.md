# Source Code Availability

Niconeon binary distributions are provided under GNU GPL v3.0 or later (`COPYING`).

The complete corresponding source code is available from this repository:

- Repository: https://github.com/sh4869221b/niconeon
- Tagged release source archives: `niconeon-X.Y.Z-source.zip` in each GitHub Release

To build from source, follow the instructions in `README.md`.

This file is included in binary distributions to document where the corresponding source can be obtained.

## Modified libmpv corresponding source

AppImage and Windows distributions include libmpv 0.41.0 with upstream commit
72d43dc9c999a21d867cdc0f934f3e4cd2195aa9 (scaler LUT padding initialization).
The exact original source archive, patch, reproducible build script, license
information, and built-runtime SHA-256 are included in the distribution's
`libmpv-source` license directory. The build script verifies the archive checksum;
packaging checks that the actual runtime matches this build before deployment.
See `scripts/deps/build_mpv.sh` and `docs/build-and-validation.md` in this repository.
