#!/usr/bin/env python3
"""Resolve every declared PE DLL import; never silently ignore an unresolved DLL."""
import argparse
from collections import deque
from pathlib import Path
import re
import shutil
import subprocess


def dll_index(directory):
    return {file.name.casefold(): file for file in directory.iterdir()
            if file.is_file() and file.suffix.casefold() == ".dll"}


def collect(bundle, sdk_bin, system_dir, report, objdump):
    roots = sorted(file for file in bundle.rglob("*")
                   if file.is_file() and file.suffix.casefold() in {".exe", ".dll"})
    pending = deque(roots)
    visited = set()
    bundled = dll_index(bundle)
    sdk = dll_index(sdk_bin)
    system = dll_index(system_dir)
    rows = ["requiring_file\tdll\tresolution"]
    missing = []
    while pending:
        binary = pending.popleft()
        key = str(binary.resolve()).casefold()
        if key in visited:
            continue
        visited.add(key)
        result = subprocess.run([objdump, "-p", str(binary)], check=True,
                                text=True, encoding="utf-8", errors="replace", capture_output=True)
        imports = re.findall(r"^\s*DLL Name:\s*(\S+)\s*$", result.stdout, re.MULTILINE)
        local = dll_index(binary.parent)
        for name in imports:
            name = name.strip()
            normalized = name.casefold()
            parent = binary.relative_to(bundle).as_posix()
            if normalized.startswith(("api-ms-", "ext-ms-")):
                rows.append(f"{parent}\t{name}\tWindows API set")
                continue
            dependency = bundled.get(normalized) or local.get(normalized)
            if dependency:
                pending.append(dependency)
                rows.append(f"{parent}\t{name}\tbundled")
            elif normalized in system:
                rows.append(f"{parent}\t{name}\tWindows system")
            elif normalized in sdk:
                source = sdk[normalized]
                destination = bundle / source.name
                shutil.copy2(source, destination)
                bundled[normalized] = destination
                pending.append(destination)
                rows.append(f"{parent}\t{name}\tcopied from UCRT64")
                print(f"Deployed {name}, required by {parent}")
            else:
                rows.append(f"{parent}\t{name}\tMISSING")
                missing.append(f"{name}, required by {parent}")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text("\n".join(rows) + "\n", encoding="utf-8")
    if missing:
        raise RuntimeError("Unresolved Windows DLL imports:\n" + "\n".join(sorted(set(missing))))
    print(f"Validated recursive PE imports for {len(visited)} binaries; report: {report}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--sdk-bin", required=True, type=Path)
    parser.add_argument("--system-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()
    collect(args.bundle.resolve(), args.sdk_bin.resolve(), args.system_dir.resolve(),
            args.report.resolve(), args.objdump)
