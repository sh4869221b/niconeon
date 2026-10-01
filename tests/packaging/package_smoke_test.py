#!/usr/bin/env python3
"""Check the release layout and AppRun contract without downloading release tools."""
from pathlib import Path
import hashlib
import importlib.util
import hashlib
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

    def test_mesa_fallback_keeps_hardware_loader_and_collects_its_own_closure(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, sdk, system = (root / name for name in ["bundle", "sdk", "system"])
            for folder in [bundle, sdk, system]:
                folder.mkdir()
            for name in ["opengl32.dll", "libgallium_wgl.dll", "libLLVM.dll"]:
                (sdk / name).write_bytes(name.encode())
            (bundle / "niconeon.exe").write_bytes(b"not part of fallback closure")
            (system / "opengl32.dll").write_bytes(b"system hardware loader")
            (system / "KERNEL32.dll").write_bytes(b"system")
            aliases = collector.deploy_mesa_fallback(bundle, sdk)
            self.assertEqual((bundle / "opengl32sw.dll").read_bytes(), b"opengl32.dll")
            self.assertFalse((bundle / "opengl32.dll").exists())
            imports = {"opengl32sw.dll": ["libgallium_wgl.dll", "KERNEL32.dll"],
                       "libgallium_wgl.dll": ["libLLVM.dll", "opengl32.dll"], "libllvm.dll": []}
            def inspect(command, **_):
                return SimpleNamespace(stdout="\n".join(
                    f"  DLL Name: {name}" for name in imports[Path(command[-1]).name.casefold()]))
            with patch.object(collector.subprocess, "run", side_effect=inspect):
                binaries = collector.collect(bundle, sdk, system, root / "report.txt", "objdump", roots=aliases)
            self.assertEqual({file.name for file in binaries},
                             {"opengl32sw.dll", "libgallium_wgl.dll", "libLLVM.dll"})
            self.assertFalse((bundle / "opengl32.dll").exists())
            self.assertEqual((system / "opengl32.dll").read_bytes(), b"system hardware loader")

    def test_mesa_fallback_requires_both_runtime_roots_before_copying(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            bundle, sdk = Path(directory) / "bundle", Path(directory) / "sdk"
            bundle.mkdir()
            sdk.mkdir()
            (sdk / "opengl32.dll").write_bytes(b"mesa")
            with self.assertRaisesRegex(RuntimeError, "libgallium_wgl.dll"):
                collector.deploy_mesa_fallback(bundle, sdk)
            self.assertFalse(list(bundle.iterdir()))

    def test_mesa_fallback_rejects_bundled_hardware_loader_override(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            bundle, sdk = Path(directory) / "bundle", Path(directory) / "sdk"
            bundle.mkdir()
            sdk.mkdir()
            (bundle / "OpenGL32.DLL").write_bytes(b"unexpected override")
            with self.assertRaisesRegex(RuntimeError, "override the system OpenGL"):
                collector.deploy_mesa_fallback(bundle, sdk)

    def test_mesa_notices_record_renamed_source_hash_package_and_nested_licenses(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, sdk = root / "bundle", root / "ucrt64/bin"
            bundle.mkdir()
            sdk.mkdir(parents=True)
            (sdk / "opengl32.dll").write_bytes(b"mesa")
            binary = bundle / "opengl32sw.dll"
            binary.write_bytes(b"mesa")
            license_file = sdk.parent / "share/licenses/mesa/exceptions/Linux-Syscall-Note"
            license_file.parent.mkdir(parents=True)
            license_file.write_text("original notice")
            package = "mingw-w64-ucrt-x86_64-mesa"
            responses = {"-Qqo": package, "-Qi": "Name : mesa\nVersion : fixture\nLicenses : MIT",
                         "-Qlq": str(license_file) + "\n" + str(sdk / "opengl32.dll")}
            with patch.object(collector.subprocess, "run", side_effect=lambda command, **_:
                              SimpleNamespace(stdout=responses[command[1]])):
                collector.capture_msys2_notices(bundle, sdk, {binary}, {binary: sdk / "opengl32.dll"})
            notices = bundle / "licenses/software-opengl"
            self.assertEqual((notices / "mesa/exceptions/Linux-Syscall-Note").read_text(), "original notice")
            self.assertIn("Version : fixture", (notices / f"{package}.txt").read_text())
            inventory = (notices / "inventory.tsv").read_text()
            self.assertIn(f"opengl32sw.dll\topengl32.dll\t{package}\t", inventory)
            self.assertIn(hashlib.sha256(b"mesa").hexdigest(), inventory)
            self.assertFalse((notices / "opengl32.dll").exists())

    def test_mesa_notices_fail_when_dependency_license_texts_are_missing(self):
        collector = self.collector()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle, sdk = root / "bundle", root / "ucrt64/bin"
            bundle.mkdir()
            sdk.mkdir(parents=True)
            binary = bundle / "libLLVM.dll"
            binary.write_bytes(b"llvm")
            (sdk / binary.name).write_bytes(b"llvm")
            responses = {"-Qqo": "mingw-w64-ucrt-x86_64-llvm-libs", "-Qi": "license metadata", "-Qlq": ""}
            with patch.object(collector.subprocess, "run", side_effect=lambda command, **_:
                              SimpleNamespace(stdout=responses[command[1]])):
                with self.assertRaisesRegex(RuntimeError, "No installed license texts"):
                    collector.capture_msys2_notices(bundle, sdk, {binary}, {})

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

    def test_runtime_hash_rejects_unpatched_or_changed_library(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-runtime-test-") as directory:
            root = Path(directory)
            runtime = root / "libmpv.so.2"
            runtime.write_bytes(b"fixture-patched-runtime")
            digest = hashlib.sha256(runtime.read_bytes()).hexdigest()
            (root / "library.sha256").write_text(f"{digest}  libmpv.so.2\n")
            environment = os.environ.copy()
            environment["NICONEON_MPV_SOURCE_DIR"] = str(root)
            command = ["bash", str(ROOT / "scripts/release/verify_mpv_runtime.sh"), str(runtime)]
            self.assertEqual(subprocess.run(command, env=environment, capture_output=True).returncode, 0)
            runtime.write_bytes(b"fixture-unpatched-runtime")
            self.assertNotEqual(subprocess.run(command, env=environment, capture_output=True).returncode, 0)

    def test_modified_library_source_rejects_corrupt_archive(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-source-test-") as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            for name in ["mpv-v0.41.0.tar.gz", "mpv-lut-padding.patch", "build_mpv.sh", "Copyright", "README.txt", "library.sha256"]:
                (source / name).write_text("deliberately invalid fixture")
            environment = os.environ.copy()
            environment["NICONEON_MPV_SOURCE_DIR"] = str(source)
            destination = root / "output"
            result = subprocess.run(["bash", str(ROOT / "scripts/release/copy_mpv_source.sh"), str(destination)],
                                    env=environment, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(destination.exists())

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
