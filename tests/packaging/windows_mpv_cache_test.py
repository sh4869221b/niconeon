#!/usr/bin/env python3
"""Offline contract for restoring the pinned Windows libmpv build."""
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("cache", ROOT / "scripts/deps/windows_mpv_cache.py")
cache = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cache)


class WindowsMpvCacheTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        self.prefix, self.destination = root / "producer", root / "consumer"
        self.bundle, self.built = root / "cache.zip", root / "fresh.dll"
        self.files = {name: name.encode() for name in cache.FILES}
        stream = io.BytesIO()
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name in (*cache.HEADERS, "Copyright"):
                info = tarfile.TarInfo("mpv-0.42.0/" + name)
                data = self.files[cache.SOURCE + name if name == "Copyright" else name]
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        self.files[cache.SOURCE + cache.ARCHIVE] = stream.getvalue()
        self.enterContext(patch.object(cache, "UPSTREAM_SHA256", cache.digest(stream.getvalue())))
        for name in cache.RECIPE:
            self.files[cache.SOURCE + name] = (cache.DEPS / name).read_bytes()
        self.files[cache.SOURCE + "library.sha256"] = (
            cache.digest(self.files[cache.DLL]) + "  libmpv-2.dll\n").encode()
        for name, data in self.files.items():
            dest = self.prefix / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        self.built.write_bytes(self.files[cache.DLL])
        (self.prefix / "bin/other.dll").touch()

    def pack(self):
        cache.pack(self.prefix, self.bundle, "exact-key", self.built)

    def test_round_trip_excludes_other_package_files(self):
        self.pack()
        cache.restore(self.destination, self.bundle, "exact-key")
        self.assertEqual({path.relative_to(self.destination).as_posix(): path.read_bytes()
                          for path in self.destination.rglob("*") if path.is_file()}, self.files)

    def test_rejects_wrong_key_and_unexpected_entries_without_overlay(self):
        self.pack()
        with self.assertRaises(ValueError):
            cache.restore(self.destination, self.bundle, "old-inventory")
        with zipfile.ZipFile(self.bundle, "a") as archive:
            archive.writestr("../extra", "unexpected")
        with self.assertRaises(ValueError):
            cache.restore(self.destination, self.bundle, "exact-key")
        self.assertFalse(self.destination.exists())

    def test_rejects_modified_member_with_fresh_zip_crc(self):
        self.pack()
        with zipfile.ZipFile(self.bundle) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        with zipfile.ZipFile(self.bundle, "w") as archive:
            for name, data in files.items():
                archive.writestr(name, b"replacement" if name == "lib/libmpv.dll.a" else data)
        with self.assertRaises(ValueError):
            cache.restore(self.destination, self.bundle, "exact-key")
        self.assertFalse(self.destination.exists())

    def test_rejects_distro_dll_and_mismatched_source(self):
        self.built.write_bytes(b"not installed")
        with self.assertRaises(ValueError):
            self.pack()
        for name in (cache.SOURCE + cache.ARCHIVE, cache.SOURCE + "Copyright",
                     cache.SOURCE + "library.sha256", cache.SOURCE + "build_mpv.sh",
                     cache.SOURCE + "mpv-static-libplacebo.patch", *cache.HEADERS):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    cache.verify_source({**self.files, name: b"changed"})


if __name__ == "__main__":
    unittest.main()
