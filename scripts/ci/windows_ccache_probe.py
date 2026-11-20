#!/usr/bin/env python3
"""Exact-build checks for the disposable MSYS2 ccache trial."""
import ctypes
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


def pe_metadata(data, checksum_function=None):
    """Allow only the COFF timestamp and its correctly derived optional checksum."""
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError("expected a Windows PE image")
    header = int.from_bytes(data[60:64], "little")
    if not 64 <= header <= len(data) - 92 or data[header:header + 4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    timestamp = header + 8
    optional = header + 24
    optional_size = int.from_bytes(data[header + 20:header + 22], "little")
    if (not 68 <= optional_size <= len(data) - optional or
            data[optional:optional + 2] not in {b"\x0b\x01", b"\x0b\x02"}):
        raise ValueError("invalid PE optional header")
    checksum = optional + 64
    time_value = int.from_bytes(data[timestamp:timestamp + 4], "little")
    checksum_value = int.from_bytes(data[checksum:checksum + 4], "little")
    time_normalized = data[:timestamp] + b"\0" * 4 + data[timestamp + 4:]
    if checksum_function is None:
        checksum_function = native_pe_checksum
    original, computed = checksum_function(data)
    if original != checksum_value:
        raise ValueError("Win32 disagrees with the PE optional header checksum")
    both = None
    if computed == original:
        both = sha256(time_normalized[:checksum] + b"\0" * 4 + time_normalized[checksum + 4:])
    return {"time_hash": sha256(time_normalized), "time": time_value,
            "valid_checksum_hash": both, "checksum": checksum_value, "computed": computed}


def native_pe_checksum(data):
    # Microsoft PE/COFF specification and ImageHlp's checksum over these exact bytes.
    # https://learn.microsoft.com/windows/win32/api/imagehlp/nf-imagehlp-checksummappedfile
    from ctypes import wintypes
    load_library_search_system32 = 0x00000800
    imagehlp = ctypes.WinDLL("imagehlp.dll", winmode=load_library_search_system32,
                             use_last_error=True)
    checksum = imagehlp.CheckSumMappedFile
    checksum.argtypes = [ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                         ctypes.POINTER(wintypes.DWORD)]
    checksum.restype = ctypes.c_void_p
    buf = ctypes.create_string_buffer(data)
    original, computed = wintypes.DWORD(), wintypes.DWORD()
    if not checksum(buf, len(data), ctypes.byref(original), ctypes.byref(computed)):
        raise ctypes.WinError(ctypes.get_last_error())
    return original.value, computed.value


def parity_kind(cold, warm):
    if cold[0] == warm[0]:
        return "raw"
    a, b = cold[1], warm[1]
    if a is None or b is None or a["time"] == b["time"]:
        return "different"
    if (a["time_hash"] == b["time_hash"] and
            (a["checksum"] == b["checksum"] == 0 or
             (a["valid_checksum_hash"] and b["valid_checksum_hash"]))):
        return "timestamp"
    if (a["valid_checksum_hash"] and a["valid_checksum_hash"] == b["valid_checksum_hash"] and
            a["checksum"] != b["checksum"]):
        return "timestamp_and_valid_checksum"
    return "different"


def snapshot(build, out):
    files = {}
    suffixes = {".obj", ".o", ".a", ".lib", ".exe", ".dll", ".cpp", ".h", ".hpp"}
    for path in sorted(build.rglob("*")):
        if path.suffix in suffixes and path.is_file():
            payload = path.read_bytes()
            image = path.suffix in {".exe", ".dll"}
            files[path.relative_to(build).as_posix()] = [
                sha256(payload), pe_metadata(payload) if image else None,
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
    counts = {"raw": 0, "timestamp": 0, "timestamp_and_valid_checksum": 0, "different": 0}
    bad = []
    checksums_logged = 0
    for name, cold_file in cold.items():
        warm_file = warm[name]
        kind = parity_kind(cold_file, warm_file)
        counts[kind] += 1
        if kind == "different":
            bad.append(name)
        if ((kind == "different" and len(bad) <= 3) or
                (kind == "timestamp_and_valid_checksum" and checksums_logged == 0)):
            print(f"CCACHE_PROBE_PE kind={kind} file={name!r} "
                  f"cold={cold_file!r} warm={warm_file!r}")
            checksums_logged += int(kind == "timestamp_and_valid_checksum")
    print(f"CCACHE_PROBE_PARITY paths={len(cold)} " + " ".join(f"{k}={v}" for k, v in counts.items()))
    print("CCACHE_PROBE_APP_SHA256 cold=" + cold["niconeon.exe"][0] + " warm=" + warm["niconeon.exe"][0])
    stock_tests, cold_tests, warm_tests = (ctests(stock / "tests.xml"), ctests(state / "cold.xml"),
                                          ctests(state / "warm.xml"))
    if not stock_tests == cold_tests == warm_tests:
        raise ValueError("stock/cold/warm CTest coverage differs")
    print(f"CCACHE_PROBE_MATCHING_CTESTS count={len(stock_tests)}")
    report_stats(state / "cold.stats", "cold")
    if not report_stats(state / "warm.stats", "warm"):
        raise ValueError("warm run produced no compiler cache hits")
    if bad:
        raise ValueError("cold/warm output differs: " + ", ".join(bad[:10]))



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
