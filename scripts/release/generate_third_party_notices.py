#!/usr/bin/env python3
"""Generate deterministic native runtime notices (Python 3.11+, no package installs)."""
from pathlib import Path
import sys
import tomllib

root = Path(__file__).resolve().parents[2]
destination = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "THIRD_PARTY_NOTICES.txt"
components = tomllib.loads((root / "packaging/licenses/manual_components.toml").read_text())["component"]
lines = [
    "Niconeon Third-Party Notices", "============================", "",
    "This document lists third-party software notices for Niconeon distributions.", "",
    "Project", "-------", "Niconeon", "Source code license: MIT",
    "Source code license file: LICENSE", "Binary distribution terms: GPL-3.0-or-later",
    "Binary distribution license file: COPYING", "Source code availability: SOURCE_CODE.md", "",
    "Native Runtime Components", "-------------------------",
]
for component in sorted(components, key=lambda row: row["name"].casefold()):
    lines.extend([f"- {component['name']}", f"  License: {component['license']}",
                  f"  Source: {component['source']}", f"  Notes: {component['notes']}", ""])
lines.extend([
    "Distribution inventory", "----------------------",
    "The application links Qt 6 and libmpv dynamically. libmpv can include FFmpeg and",
    "other platform-specific codec, audio, rendering, font, and network libraries.",
    "The distributor must retain each bundled dependency's copyright notices and",
    "satisfy the licenses of the actual binaries, build options, and corresponding sources.",
    "This direct-runtime summary is not a substitute for that package-specific inventory.",
    "",
])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_text("\n".join(lines), encoding="utf-8")
print(f"created: {destination}")
