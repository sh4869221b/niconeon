#!/usr/bin/env python3
"""Resolve every declared PE DLL import; never silently ignore an unresolved DLL."""
import argparse
from collections import deque
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
from time import monotonic


def timed_probe(timings, phase, operation, *args, **kwargs):
    if timings is None:
        return operation(*args, **kwargs)
    start = monotonic()
    result = operation(*args, **kwargs)
    count, elapsed = timings.get(phase, (0, 0.0))
    timings[phase] = (count + 1, elapsed + monotonic() - start)
    return result


def report_probe(profile, timings):
    if timings is not None:
        for phase, (count, elapsed) in timings.items():
            print(f"MESA_PROBE profile={profile} part={phase} calls={count} ms={round(elapsed * 1000)}")


def dll_index(directory):
    return {file.name.casefold(): file for file in directory.iterdir()
            if file.is_file() and file.suffix.casefold() == ".dll"}


def retain_sqlite_driver(bundle):
    """Keep only the application's supported SQL backend in an owned staging bundle."""
    directory = bundle / "sqldrivers"
    drivers = dll_index(directory) if directory.is_dir() else {}
    if "qsqlite.dll" not in drivers:
        raise RuntimeError("Required Qt SQLite driver is missing from the staging bundle")
    for name, path in drivers.items():
        if name != "qsqlite.dll":
            print(f"Omitted unused SQL plugin: {path.name}")
            path.unlink()


def deploy_mesa_fallback(bundle, sdk_bin):
    """Use Qt's fallback name, never shadow the system hardware OpenGL loader."""
    if "opengl32.dll" in dll_index(bundle):
        raise RuntimeError("Bundled opengl32.dll would override the system OpenGL driver")
    sdk = dll_index(sdk_bin)
    names = {"opengl32sw.dll": "opengl32.dll", "libgallium_wgl.dll": "libgallium_wgl.dll"}
    for original in names.values():
        if original not in sdk:
            raise RuntimeError(f"Missing Mesa runtime {original}; install mingw-w64-ucrt-x86_64-mesa")
    for destination, original in names.items():
        shutil.copy2(sdk[original], bundle / destination)
    return {bundle / destination: sdk[original] for destination, original in names.items()}


def capture_msys2_notices(bundle, sdk_bin, binaries, aliases, pacman="pacman", profile=False):
    """Retain package versions, source provenance, and supplied license texts."""
    timings = {} if profile else None
    sdk = timed_probe(timings, "sdk_index", dll_index, sdk_bin)
    license_root = sdk_bin.parent / "share/licenses"
    destination = bundle / "licenses/software-opengl"
    destination.mkdir(parents=True, exist_ok=True)
    packages = set()
    rows = ["bundled_dll\tmsys2_source\tpackage\tsha256"]

    def query(*arguments):
        part = {"-Qqo": "package_owner", "-Qi": "package_info", "-Qlq": "package_files"}[arguments[0]]
        return timed_probe(timings, part, subprocess.run, [pacman, *arguments], check=True, text=True,
                           encoding="utf-8", capture_output=True).stdout.strip()

    for binary in sorted(binaries):
        source = aliases.get(binary) or sdk.get(binary.name.casefold())
        if source is None:
            raise RuntimeError(f"Cannot establish MSYS2 provenance for {binary.name}")
        package = query("-Qqo", str(source))
        if not re.fullmatch(r"mingw-w64-ucrt-x86_64-[a-zA-Z0-9_+.\-]+", package):
            raise RuntimeError(f"Unexpected MSYS2 package owner for {binary.name}: {package}")
        packages.add(package)
        contents = timed_probe(timings, "inventory_read", binary.read_bytes)
        digest = timed_probe(timings, "inventory_sha256", hashlib.sha256, contents).hexdigest()
        rows.append(f"{binary.relative_to(bundle).as_posix()}\t{source.name}\t{package}\t{digest}")
    for package in sorted(packages):
        (destination / f"{package}.txt").write_text(query("-Qi", package) + "\n", encoding="utf-8")
        copied = 0
        for name in query("-Qlq", package).splitlines():
            source = Path(name)
            if timed_probe(timings, "license_file_exists", source.is_file) and source.is_relative_to(license_root):
                target = destination / source.relative_to(license_root)
                target.parent.mkdir(parents=True, exist_ok=True)
                timed_probe(timings, "license_copy", shutil.copy2, source, target)
                copied += 1
        if not copied:
            raise RuntimeError(f"No installed license texts found for software OpenGL dependency {package}")
    (destination / "inventory.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    report_probe("notices", timings)


def collect(bundle, sdk_bin, system_dir, report, objdump, roots=None, profile=None, repeated=()):
    timings = {} if profile is not None else None
    if roots is None:
        roots = timed_probe(timings, "glob_roots", sorted,
                            (file for file in bundle.rglob("*")
                             if file.is_file() and file.suffix.casefold() in {".exe", ".dll"}))
    pending = deque(roots)
    visited = set()
    bundled = timed_probe(timings, "bundle_index", dll_index, bundle)
    # The only writes during this traversal copy SDK DLLs into the bundle
    # root, updating bundled immediately. A previously dangling symlink can
    # become resolvable after a copy; keep rescanning directories with DLL links.
    directory_indexes = {}
    sdk = timed_probe(timings, "sdk_index", dll_index, sdk_bin)
    system = timed_probe(timings, "system32_index", dll_index, system_dir)
    rows = ["requiring_file\tdll\tresolution"]
    missing = []
    while pending:
        binary = pending.popleft()
        key = timed_probe(timings, "resolve", binary.resolve)
        if key in visited:
            continue
        visited.add(key)
        timed = profile == "mesa" or (profile is not None and key in repeated)
        if timed:
            before = monotonic()
        result = timed_probe(timings, "objdump", subprocess.run, [objdump, "-p", str(binary)],
                             check=True, text=True, encoding="utf-8", errors="replace", capture_output=True)
        if timed:
            elapsed = round((monotonic() - before) * 1000)
            print(f"MESA_PROBE phase={profile} file={binary.relative_to(bundle).as_posix()!r} "
                  f"ms={elapsed} bytes={binary.stat().st_size} output_chars={len(result.stdout)}")
        imports = timed_probe(timings, "import_pattern", re.findall,
                              r"^\s*DLL Name:\s*(\S+)\s*$", result.stdout, re.MULTILINE)
        parent_dir = binary.parent
        if parent_dir not in directory_indexes:
            if any(path.suffix.casefold() == ".dll" and path.is_symlink()
                   for path in parent_dir.iterdir()):
                directory_indexes[parent_dir] = None
            else:
                directory_indexes[parent_dir] = (bundled if parent_dir == bundle else
                                                 timed_probe(timings, "local_index", dll_index, parent_dir))
        local = directory_indexes[parent_dir]
        if local is None:
            local = timed_probe(timings, "local_index", dll_index, parent_dir)
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
                if profile == "mesa":
                    before = monotonic()
                shutil.copy2(source, destination)
                if profile == "mesa":
                    elapsed = round((monotonic() - before) * 1000)
                    print(f"MESA_PROBE copy={source.name!r} ms={elapsed} bytes={destination.stat().st_size}")
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
    report_probe(profile, timings)
    print(f"Validated recursive PE imports for {len(visited)} binaries; report: {report}")
    return visited


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--sdk-bin", required=True, type=Path)
    parser.add_argument("--system-dir", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--objdump", default="objdump")
    parser.add_argument("--sqlite-only", action="store_true",
                        help="Keep only qsqlite in the staging bundle before checking imports")
    parser.add_argument("--software-opengl", action="store_true",
                        help="Deploy Mesa under Qt's fallback name and retain dependency license texts")
    args = parser.parse_args()
    bundle, sdk, system = args.bundle.resolve(), args.sdk_bin.resolve(), args.system_dir.resolve()
    profiling = os.environ.get("NICONEON_PE_PROFILE") == "1"
    if profiling:
        for _ in range(3):
            start = monotonic()
            subprocess.run([args.objdump, "--version"], check=True, capture_output=True)
            print(f"MESA_PROBE process_reference_ms={round((monotonic() - start) * 1000)}")
    scanned_mesa = set()
    if args.sqlite_only:
        retain_sqlite_driver(bundle)
    if args.software_opengl:
        start = monotonic()
        aliases = deploy_mesa_fallback(bundle, sdk)
        if profiling:
            print(f"MESA_PROBE seed_copy_and_index_ms={round((monotonic() - start) * 1000)}")
        binaries = collect(bundle, sdk, system, bundle / "software-opengl-dependencies.txt",
                           args.objdump, roots=aliases, profile="mesa" if profiling else None)
        scanned_mesa = binaries
        print(f"WINDOWS_PACKAGE_MESA_PE_MS={round((monotonic() - start) * 1000)}")
        start = monotonic()
        capture_msys2_notices(bundle, sdk, binaries, aliases, profile=profiling)
        print(f"WINDOWS_PACKAGE_MESA_NOTICES_MS={round((monotonic() - start) * 1000)}")
    start = monotonic()
    collect(bundle, sdk, system, args.report.resolve(), args.objdump,
            profile="repeat" if profiling else None, repeated=scanned_mesa)
    print(f"WINDOWS_PACKAGE_ALL_PE_MS={round((monotonic() - start) * 1000)}")
