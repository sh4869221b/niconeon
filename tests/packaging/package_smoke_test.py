#!/usr/bin/env python3
"""Check the release layout and AppRun contract without downloading release tools."""
from pathlib import Path
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]


class WindowsDependencyCollectorTests(unittest.TestCase):
    def collector(self):
        spec = importlib.util.spec_from_file_location(
            "collect_windows_dlls", ROOT / "scripts/release/collect_windows_dlls.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_sqlite_selection_omits_unsupported_database_plugins(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)
            drivers = bundle / "sqldrivers"
            drivers.mkdir()
            (drivers / "QSQLite.DLL").write_bytes(b"sqlite")
            for name in ["qsqlibase.dll", "qsqlmysql.dll", "qsqlpsql.dll", "qsqlodbc.dll"]:
                (drivers / name).write_bytes(b"unsupported")
            collector.retain_sqlite_driver(bundle)
            self.assertEqual([file.name for file in drivers.iterdir()], ["QSQLite.DLL"])
            self.assertEqual((drivers / "QSQLite.DLL").read_bytes(), b"sqlite")

    def test_sqlite_selection_fails_before_removing_anything_if_required_driver_is_missing(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            bundle = Path(directory)
            drivers = bundle / "sqldrivers"
            drivers.mkdir()
            other = drivers / "qsqlmysql.dll"
            other.write_bytes(b"unsupported")
            with self.assertRaisesRegex(RuntimeError, "Required Qt SQLite driver"):
                collector.retain_sqlite_driver(bundle)
            self.assertTrue(other.exists())

    def test_recursive_sdk_imports_and_system_allowances(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, sdk, system = (root / name for name in ["bundle", "sdk", "system"])
            for folder in [bundle, sdk, system]:
                folder.mkdir()
            (bundle / "niconeon.exe").write_bytes(b"fixture")
            (sdk / "first.dll").write_bytes(b"first")
            (sdk / "second.dll").write_bytes(b"second")
            (system / "KERNEL32.dll").write_bytes(b"system")
            imports = {"niconeon.exe": ["FIRST.dll", "KERNEL32.dll", "api-ms-win-core-test.dll"],
                       "first.dll": ["second.dll"], "second.dll": ["KERNEL32.dll"]}
            def inspect(command, **_):
                return SimpleNamespace(stdout="\n".join(
                    f"  DLL Name: {name}" for name in imports[Path(command[-1]).name.casefold()]))
            with patch.object(collector.subprocess, "run", side_effect=inspect):
                collector.collect(bundle, sdk, system, root / "report.txt", "objdump")
            self.assertEqual((bundle / "first.dll").read_bytes(), b"first")
            self.assertEqual((bundle / "second.dll").read_bytes(), b"second")
            self.assertNotIn("MISSING", (root / "report.txt").read_text())

    def test_missing_import_is_a_hard_failure_with_report(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, sdk, system = (root / name for name in ["bundle", "sdk", "system"])
            for folder in [bundle, sdk, system]:
                folder.mkdir()
            (bundle / "niconeon.exe").write_bytes(b"fixture")
            with patch.object(collector.subprocess, "run", return_value=SimpleNamespace(
                    stdout="  DLL Name: missing.dll\n")):
                with self.assertRaisesRegex(RuntimeError, "missing.dll"):
                    collector.collect(bundle, sdk, system, root / "report.txt", "objdump")
            self.assertIn("MISSING", (root / "report.txt").read_text())


@unittest.skipIf(os.name == "nt", "Bash/AppRun packaging layout is a Linux test")
class PackagingSmoke(unittest.TestCase):
    def test_linux_bundle_is_one_executable_and_licenses(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-package-test-") as directory:
            root = Path(directory)
            (root / "scripts/release").mkdir(parents=True)
            for name in ["package_linux.sh", "verify_binary_artifact_licenses.sh"]:
                shutil.copy2(ROOT / "scripts/release" / name, root / "scripts/release" / name)
            for name in ["LICENSE", "COPYING", "SOURCE_CODE.md", "THIRD_PARTY_NOTICES.txt"]:
                shutil.copy2(ROOT / name, root / name)
            build = root / "build/release"
            build.mkdir(parents=True)
            (build / "niconeon").write_text("#!/bin/sh\nexit 0\n")
            (build / "niconeon").chmod(0o755)
            environment = os.environ.copy()
            environment.pop("NICONEON_BUILD_DIR", None)
            environment.pop("NICONEON_RELEASE_BASENAME", None)
            subprocess.run(["bash", str(root / "scripts/release/package_linux.sh"), "test"],
                           env=environment, check=True)
            artifact = root / "dist/niconeon-test-linux-x86_64-binaries.zip"
            with zipfile.ZipFile(artifact) as archive:
                names = {Path(name).name for name in archive.namelist() if not name.endswith("/")}
                self.assertEqual(names, {"niconeon", "LICENSE", "COPYING", "SOURCE_CODE.md",
                                         "THIRD_PARTY_NOTICES.txt"})
            subprocess.run(["bash", str(root / "scripts/release/verify_binary_artifact_licenses.sh"),
                            str(artifact)], check=True)

    def test_apprun_preserves_arguments_and_uses_local_loader_paths(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-apprun-test-") as directory:
            root = Path(directory) / "AppDir with spaces"
            (root / "usr/bin").mkdir(parents=True)
            shutil.copy2(ROOT / "packaging/appimage/AppRun", root / "AppRun")
            application = root / "usr/bin/niconeon"
            application.write_text("#!/bin/sh\nprintf '%s\\n' \"$1\" \"$2\" \"$LD_LIBRARY_PATH\" \"$QT_PLUGIN_PATH\"\n")
            application.chmod(0o755)
            result = subprocess.run(["bash", str(root / "AppRun"), "video with spaces.mp4", "日本語"],
                                    check=True, text=True, capture_output=True)
            lines = result.stdout.splitlines()
            self.assertEqual(lines[:2], ["video with spaces.mp4", "日本語"])
            self.assertTrue(lines[2].startswith(str(root / "usr/lib") + ":"))
            self.assertTrue(lines[3].startswith(str(root / "usr/plugins") + ":"))


if __name__ == "__main__":
    unittest.main()
