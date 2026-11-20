#!/usr/bin/env python3
"""Exact-build checks for the disposable MSYS2 ccache trial."""
import hashlib
import json
from pathlib import Path
import re
import sys
from xml.etree import ElementTree


def normalized_commands(build, launcher):
    entries = json.loads((build / "compile_commands.json").read_text())
    figures = []
    for entry in entries:
        cleaned = {key: val.replace(build.name, "<build-dir>") for key, val in entry.items()}
        # Exported command databases may omit the launcher. Remove only the
        # exact expected first token if CMake included it.
        first = re.match(r'^(?:"([^"]+)"|(\S+))\s+', cleaned["command"])
        if first and (first[1] or first[2]).replace("\\", "/").casefold() == launcher.casefold():
            cleaned["command"] = cleaned["command"][first.end():]
        figures.append(json.dumps(cleaned, sort_keys=True))
    return sorted(figures)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def pe_timestamp_normalized_digest(data):
    """Compare a PE allowing only its well-understood COFF timestamp to vary."""
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("expected a Windows PE image")
    header = int.from_bytes(data[60:64], "little")
    if not 64 <= header <= len(data) - 24 or data[header:header + 4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    timestamp = header + 8
    return sha256(data[:timestamp] + b"\0" * 4 + data[timestamp + 4:])


def snapshot(build, out):
    files = {}
    suffixes = {".obj", ".o", ".a", ".lib", ".exe", ".dll", ".cpp", ".h", ".hpp"}
    for path in sorted(build.rglob("*")):
        if path.suffix in suffixes and path.is_file():
            payload = path.read_bytes()
            image = path.suffix in {".exe", ".dll"}
            files[path.relative_to(build).as_posix()] = [
                sha256(payload), pe_timestamp_normalized_digest(payload) if image else None,
            ]
    if not any(name.endswith((".obj", ".o")) for name in files) or "niconeon.exe" not in files:
        raise ValueError("missing object or application executable")
    out.write_text(json.dumps(files, sort_keys=True))
    for line in reversed((build / ".ninja_log").read_text().splitlines()):
        fields = line.split("\t")
        if len(fields) >= 4 and fields[3] == "niconeon.exe":
            print(f"CCACHE_PROBE_APP_LINK_MS={int(fields[1]) - int(fields[0])}")
            break


def ctests(path):
    cases = [(case.get("name", ""), case.find("failure") is None and case.find("skipped") is None)
             for case in ElementTree.parse(path).iter("testcase")]
    if len(cases) != 17 or not all(passed for _, passed in cases):
        raise ValueError(f"expected 17 successful CTest cases: {path.name}")
    return sorted(name for name, _ in cases)


def report_stats(path, phase):
    stats = {name: int(count) for name, count in
             (line.split() for line in path.read_text().splitlines() if line.strip())}
    hits = stats.get("direct_cache_hit", 0) + stats.get("preprocessed_cache_hit", 0)
    misses = stats.get("cache_miss", 0)
    print(f"CCACHE_PROBE_STATS phase={phase} hits={hits} misses={misses} "
          f"cacheable_hit_percent={100 * hits / max(hits + misses, 1):.1f}")
    return hits


def compare(stock, state):
    cold = json.loads((state / "cold.json").read_text())
    warm = json.loads((state / "warm.json").read_text())
    if cold.keys() != warm.keys():
        raise ValueError("cold/warm object, source or binary inventory differs")
    different = [name for name in cold if cold[name][0] != warm[name][0]]
    non_pe = [name for name in different if cold[name][1] is None or cold[name][1] != warm[name][1]]
    print(f"CCACHE_PROBE_PARITY paths={len(cold)} COFF_timestamp_only={len(different) - len(non_pe)} "
          f"other_mismatches={len(non_pe)}")
    if non_pe:
        raise ValueError("cold/warm output differs: " + ", ".join(non_pe[:10]))
    print("CCACHE_PROBE_APP_SHA256 cold=" + cold["niconeon.exe"][0] + " warm=" + warm["niconeon.exe"][0])
    if ctests(stock / "tests.xml") != ctests(state / "cold.xml") or ctests(state / "cold.xml") != ctests(state / "warm.xml"):
        raise ValueError("stock/cold/warm CTest coverage differs")
    report_stats(state / "cold.stats", "cold")
    if not report_stats(state / "warm.stats", "warm"):
        raise ValueError("warm run produced no compiler cache hits")


if __name__ == "__main__":
    action, path_a, path_b = sys.argv[1:4]
    a, b = Path(path_a), Path(path_b)
    if action == "commands":
        stock, probe = normalized_commands(a, sys.argv[4]), normalized_commands(b, sys.argv[4])
        if not stock or stock != probe:
            raise ValueError("stock and probe compiler invocation sets differ after build path/launcher normalization")
        print(f"CCACHE_PROBE_FLAGS_MATCH units={len(stock)}")
    elif action == "snapshot":
        snapshot(a, b)
    elif action == "compare":
        compare(a, b)
    else:
        raise ValueError(action)
