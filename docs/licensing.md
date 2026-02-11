# Licensing

## Project License

- Niconeon source code in this repository is licensed under the MIT License.
- Binary distributions are provided under GPL-3.0-or-later terms.
- See:
  - `LICENSE` (MIT text for source code)
  - `COPYING` (GPLv3 text for binary distribution terms)
  - `SOURCE_CODE.md` (how to obtain corresponding source code)

## Third-Party Notices

- Runtime and dependency notices are aggregated in `THIRD_PARTY_NOTICES.txt`.
- Manual runtime components are defined in `packaging/licenses/manual_components.toml`.

## How to Regenerate

```bash
scripts/release/generate_third_party_notices.sh
```

Prerequisites:

- `cargo-license` installed (`cargo install --locked cargo-license`)
- `python3` (with standard-library `tomllib`, Python 3.11+)

## Distribution Policy

- Binary distributions must include:
  - `LICENSE`
  - `COPYING`
  - `SOURCE_CODE.md`
  - `THIRD_PARTY_NOTICES.txt`
- The UI provides an About dialog that surfaces the same information.
- UI resources use `QT_RESOURCE_ALIAS` for these files so CMake resource registration remains valid
  even when the source paths are outside `app-ui/`.
