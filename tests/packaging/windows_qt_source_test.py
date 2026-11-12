#!/usr/bin/env python3
"""Offline tests of the unchanged Windows Qt source copier with synthetic pins."""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


@unittest.skipIf(os.name == "nt", "Run shell fixture tests on Linux or MSYS Python")
class WindowsQtSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="windows-qt-source-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "qt prefix"
        self.source = self.prefix / "share/niconeon/windows-qt-source"
        self.source.mkdir(parents=True)
        self.deps = self.root / "scripts/deps"
        self.deps.mkdir(parents=True)
        self.script = self.root / "scripts/release/copy_windows_qt_source.sh"
        self.script.parent.mkdir()
        shutil.copyfile(ROOT / "scripts/release/copy_windows_qt_source.sh", self.script)
        self.destination = self.root / "license destination"
        for name in ("PKGBUILD", "PKGBUILD.upstream", "README.txt", "build_windows_qt.sh",
                     "build.log", "CMakeCache.txt", "package-inventory.txt", "packages.sha256",
                     "LICENSES/LGPL-3.0.txt", "recipe/PKGBUILD", "recipe/platform.patch",
                     "qtbase-everywhere-src-6.12.0.tar.xz", "CVE-fixture.diff"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic fixture: " + name)
        recipe = self.manifest(self.source / "recipe", ["PKGBUILD", "platform.patch"])
        qt = self.manifest(self.source, ["qtbase-everywhere-src-6.12.0.tar.xz", "CVE-fixture.diff"])
        for name, content in (("windows-qt-recipe.sha256", recipe), ("qt-sources.sha256", qt)):
            (self.deps / name).write_text(content)
            (self.source / name).write_text(content)
        (self.prefix / "bin").mkdir()
        for name in ("Qt6Core.dll", "Qt6Network.dll"):
            (self.prefix / "bin" / name).write_bytes(("fake DLL " + name).encode())
        (self.source / "runtime.sha256").write_text(
            self.manifest(self.prefix, ["bin/Qt6Core.dll", "bin/Qt6Network.dll"]))
        self.refresh()

    @staticmethod
    def manifest(root, names):
        return "".join(hashlib.sha256((root / name).read_bytes()).hexdigest()
                       + "  " + name + "\n" for name in names)

    def refresh(self):
        names = sorted("./" + p.relative_to(self.source).as_posix()
                       for p in self.source.rglob("*")
                       if p.is_file() and p.name != "evidence.sha256")
        (self.source / "evidence.sha256").write_text(self.manifest(self.source, names))

    def run_copier(self, success=False):
        result = subprocess.run(["bash", str(self.script), str(self.destination), str(self.prefix)],
                                capture_output=True, text=True)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((self.destination / "windows-qt-source").exists())

    def test_builder_crlf_manifests_keep_checksums_enforced(self):
        # Exercise the real builder up to its checksum gate, without network or
        # Windows tools. The intentionally wrong Qt source must still fail.
        builder = self.deps / "build_windows_qt.sh"
        shutil.copyfile(ROOT / "scripts/deps/build_windows_qt.sh", builder)
        mock = self.root / "mock-bin"
        mock.mkdir()
        (mock / "pacman").write_text(
            '#!/usr/bin/env bash\n'
            'if [[ $# == 2 ]]; then printf "%s 6.12.0-1\\n" "$2"; '
            'else echo "fixture 1"; fi\n')
        payload = b"synthetic upstream recipe\n"
        digest = hashlib.sha256(payload).hexdigest()
        (self.deps / "windows-qt-recipe.sha256").write_bytes(
            (digest + "  PKGBUILD\r\n").encode())
        (self.deps / "qt-sources.sha256").write_bytes(
            ("0" * 64 + "  qtbase-everywhere-src-6.12.0.tar.xz\r\n").encode())
        log = self.root / "curl-urls"
        (mock / "curl").write_text(
            '#!/usr/bin/env bash\n'
            'url="${@: -1}"; output="${@: -2:1}"\n'
            'printf "%s\\n" "$url" >> "$CURL_TEST_LOG"\n'
            '[[ "$url" != *$\'\\r\'* ]] || exit 3\n'
            'printf "synthetic upstream recipe\\n" > "$output"\n')
        for executable in mock.iterdir():
            executable.chmod(0o755)
        environment = dict(os.environ, MSYSTEM="UCRT64", MINGW_PREFIX="/ucrt64",
                           NICONEON_WINDOWS_QT_WORK_DIR=str(self.root / "build-cache"),
                           CURL_TEST_LOG=str(log), PATH=str(mock) + os.pathsep + os.environ["PATH"])
        result = subprocess.run(["bash", str(builder)], env=environment,
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("PKGBUILD: OK", result.stdout)
        self.assertIn("qtbase-everywhere-src-6.12.0.tar.xz: FAILED", result.stdout)
        urls = log.read_bytes().splitlines()
        self.assertEqual(len(urls), 2)
        self.assertNotIn(b"\r", log.read_bytes())
        self.assertTrue(urls[0].endswith(b"/PKGBUILD"))
        self.assertTrue(urls[1].endswith(b"/qtbase-everywhere-src-6.12.0.tar.xz"))

    def test_valid_bundle(self):
        self.run_copier(True)
        self.assertEqual((self.destination / "windows-qt-source/PKGBUILD").read_bytes(),
                         (self.source / "PKGBUILD").read_bytes())

    def test_missing_required_file(self):
        (self.source / "build.log").unlink()
        self.refresh()
        self.run_copier()

    def test_corrupt_evidence(self):
        (self.source / "PKGBUILD").write_text("corrupt")
        self.run_copier()

    def test_missing_evidence_entry(self):
        p = self.source / "evidence.sha256"
        p.write_text("\n".join(p.read_text().splitlines()[1:]) + "\n")
        self.run_copier()

    def test_source_hash_even_with_refreshed_evidence(self):
        (self.source / "qtbase-everywhere-src-6.12.0.tar.xz").write_text("corrupt")
        self.refresh()
        self.run_copier()

    def test_recipe_hash_even_with_refreshed_evidence(self):
        (self.source / "recipe/PKGBUILD").write_text("corrupt")
        self.refresh()
        self.run_copier()

    def test_changed_source_pins(self):
        (self.source / "qt-sources.sha256").write_text("0" * 64 + "  other.tar.xz\n")
        self.refresh()
        self.run_copier()

    def test_missing_runtime(self):
        (self.prefix / "bin/Qt6Core.dll").unlink()
        self.run_copier()

    def test_corrupt_runtime(self):
        (self.prefix / "bin/Qt6Network.dll").write_bytes(b"changed")
        self.run_copier()

    def test_missing_required_runtime_entry(self):
        (self.source / "runtime.sha256").write_text(self.manifest(self.prefix, ["bin/Qt6Core.dll"]))
        self.refresh()
        self.run_copier()

    def test_runtime_manifest_traversal(self):
        p = self.source / "runtime.sha256"
        p.write_text(p.read_text() + "0" * 64 + "  ../outside.dll\n")
        self.refresh()
        self.run_copier()

    def test_duplicate_runtime_entry(self):
        p = self.source / "runtime.sha256"
        p.write_text(p.read_text() * 2)
        self.refresh()
        self.run_copier()

    def test_evidence_manifest_traversal(self):
        p = self.source / "evidence.sha256"
        p.write_text(p.read_text() + "0" * 64 + "  ../outside\n")
        self.run_copier()

    def test_missing_licenses(self):
        shutil.rmtree(self.source / "LICENSES")
        self.refresh()
        self.run_copier()


if __name__ == "__main__":
    unittest.main()
