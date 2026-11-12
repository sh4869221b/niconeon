#!/usr/bin/env python3
"""Check the release layout and AppRun contract without downloading release tools."""
from pathlib import Path
import hashlib
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import tarfile
import unittest
from unittest.mock import patch
import zipfile
from types import SimpleNamespace

from ffmpeg_source_test import FFmpegSourceTests
from windows_qt_source_test import WindowsQtSourceTests


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipIf(sys.platform != "linux", "Qt SDK source packaging uses Linux shell tools")
class QtSourcePackagingTests(unittest.TestCase):
    def fixture(self, root, source_override=False):
        """Exercise the real copier with tiny, explicitly synthetic pinned inputs."""
        script = root / "scripts/release/copy_qt_source.sh"
        script.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/release/copy_qt_source.sh", script)
        prefix = root / "SDK with spaces"
        source = (root / "source override with spaces" if source_override
                  else prefix / "share/niconeon/qt-source")
        source.mkdir(parents=True)
        # Only the fixture's adjacent trusted manifest changes; do not replace
        # checksum tools, disable checks, download Qt, or edit production pins.
        source_manifest = []
        for entry in (ROOT / "scripts/deps/qt-sources.sha256").read_text().splitlines():
            _, name = entry.split()
            payload = f"Synthetic source/patch fixture for {name}\n".encode()
            (source / name).write_bytes(payload)
            source_manifest.append(f"{hashlib.sha256(payload).hexdigest()}  {name}\n")
        manifest = root / "scripts/deps/qt-sources.sha256"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("".join(source_manifest))
        shutil.copy2(manifest, source / manifest.name)
        for name in ["build_qt.sh", "README.txt"]:
            (source / name).write_text("Synthetic packaging fixture, not a Qt build.\n")
        for module in ["qtbase", "qtshadertools", "qtsvg", "qtdeclarative"]:
            for name in [f"notices/{module}/LICENSES/GPL-3.0-only.txt",
                         f"configuration/{module}-CMakeCache.txt"]:
                file = source / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(f"Synthetic attribution/configuration for {module}\n")
        runtime_paths = [f"lib/libQt6{module}.so.6.12.0"
                         for module in ["Core", "Gui", "Network", "Quick", "Qml", "Sql"]]
        runtime_paths += ["plugins/platforms/libqxcb.so", "plugins/sqldrivers/libqsqlite.so",
                          "qml/QtQuick/Controls/libqtquickcontrols2plugin.so"]
        runtime_manifest = []
        for name in runtime_paths:
            file = prefix / name
            file.parent.mkdir(parents=True, exist_ok=True)
            payload = f"Synthetic runtime fixture for {name}\n".encode()
            file.write_bytes(payload)
            runtime_manifest.append(f"{hashlib.sha256(payload).hexdigest()}  {name}\n")
        (source / "runtime.sha256").write_text("".join(runtime_manifest))
        environment = os.environ.copy()
        environment.pop("NICONEON_QT_SOURCE_DIR", None)
        if source_override:
            environment["NICONEON_QT_SOURCE_DIR"] = str(source)
        output = root / "licenses with spaces"
        command = ["bash", str(script), str(output), str(prefix)]
        return source, prefix, output, command, environment

    def test_verified_qt_sources_patches_notices_and_runtime_manifest_are_copied(self):
        for override in [False, True]:
            with self.subTest(source_override=override), tempfile.TemporaryDirectory() as directory:
                source, _, output, command, environment = self.fixture(Path(directory), override)
                result = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = {file.relative_to(source): file.read_bytes()
                            for file in source.rglob("*") if file.is_file()}
                copied = output / "qt-source"
                actual = {file.relative_to(copied): file.read_bytes()
                          for file in copied.rglob("*") if file.is_file()}
                self.assertEqual(actual, expected)
                self.assertFalse(any(file.suffix == ".so" for file in copied.rglob("*")))

    def test_qt_source_or_runtime_corruption_is_rejected_before_copying(self):
        cases = ["source", "patch", "source-manifest", "core-runtime", "network-runtime",
                 "qml-plugin", "missing-license", "missing-configuration", "missing-runtime-entry"]
        for corrupt in cases:
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                source, prefix, output, command, environment = self.fixture(Path(directory))
                if corrupt == "source":
                    next(source.glob("qtbase-*.tar.xz")).write_bytes(b"corrupt source")
                elif corrupt == "patch":
                    next(source.glob("CVE-*.diff")).write_bytes(b"corrupt security patch")
                elif corrupt == "source-manifest":
                    with (source / "qt-sources.sha256").open("a") as manifest:
                        manifest.write("0" * 64 + "  unapproved-source.tar.xz\n")
                elif corrupt in ["core-runtime", "network-runtime"]:
                    module = "Core" if corrupt == "core-runtime" else "Network"
                    (prefix / f"lib/libQt6{module}.so.6.12.0").write_bytes(b"wrong runtime")
                elif corrupt == "qml-plugin":
                    (prefix / "qml/QtQuick/Controls/libqtquickcontrols2plugin.so").write_bytes(b"wrong plugin")
                elif corrupt == "missing-license":
                    (source / "notices/qtdeclarative/LICENSES/GPL-3.0-only.txt").unlink()
                elif corrupt == "missing-configuration":
                    (source / "configuration/qtbase-CMakeCache.txt").unlink()
                else:
                    manifest = source / "runtime.sha256"
                    manifest.write_text("".join(line for line in manifest.read_text().splitlines(True)
                                                if "libQt6Core.so" not in line))
                result = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertFalse(output.exists(), "failed verification must not produce a source bundle")

    def test_qt_runtime_manifest_traversal_is_rejected_even_with_a_matching_hash(self):
        for path in ["lib/../../outside.so", "plugins/../outside.so", "qml/../outside.so"]:
            with self.subTest(path=path), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, prefix, output, command, environment = self.fixture(root)
                outside = prefix / path
                outside.write_bytes(b"readable file outside the declared runtime subdirectory")
                digest = hashlib.sha256(outside.read_bytes()).hexdigest()
                # Append to a complete valid manifest, so required-entry checks
                # cannot accidentally be the reason traversal is rejected.
                with (source / "runtime.sha256").open("a") as manifest:
                    manifest.write(f"{digest}  {path}\n")
                result = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("invalid Qt runtime manifest", result.stderr)
                self.assertFalse(output.exists())


@unittest.skipIf(sys.platform != "linux" or not shutil.which("cc") or not shutil.which("readelf"),
                 "Media support packaging requires a Linux C compiler and readelf")
class MediaSupportSourcePackagingTests(unittest.TestCase):
    @staticmethod
    def hash_line(root, name):
        return f"{hashlib.sha256((root / name).read_bytes()).hexdigest()}  {name}\n"

    def fixture(self, root, source_override=False):
        """Use the real copier, synthetic pins and real minimal ELF libraries."""
        script = root / "scripts/release/copy_media_support_source.sh"
        script.parent.mkdir(parents=True)
        shutil.copy2(ROOT / "scripts/release/copy_media_support_source.sh", script)
        prefix = root / "media SDK with spaces"
        source = (root / "source override with spaces" if source_override
                  else prefix / "share/niconeon/media-support-source")
        source.mkdir(parents=True)
        manifest = root / "scripts/deps/media-support-sources.sha256"
        manifest.parent.mkdir(parents=True)
        source_manifest = []
        for line in (ROOT / "scripts/deps/media-support-sources.sha256").read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            _, name = line.split()
            (source / name).write_text(f"Synthetic media support source fixture: {name}\n")
            source_manifest.append(self.hash_line(source, name))
        # Change only the isolated fixture's adjacent trusted pins. Production
        # pins, checksum validation and ELF inspection stay untouched.
        manifest.write_text("".join(source_manifest))
        shutil.copy2(manifest, source / manifest.name)
        evidence = ["build_media_support.sh", "README.txt", "notices/libass/COPYING",
                    "notices/lcms2/LICENSE", "configuration/build.txt",
                    "configuration/libass/config.log", "configuration/libass/config.status",
                    "configuration/libass/config.h", "configuration/libass/Makefile",
                    "configuration/lcms2/intro-buildoptions.json",
                    "configuration/lcms2/intro-dependencies.json",
                    "configuration/lcms2/compile_commands.json",
                    "configuration/lcms2/meson-log.txt", "configuration/lcms2/testlog.txt"]
        for name in evidence:
            target = source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"Synthetic attribution/configuration fixture: {name}\n")
        (source / "evidence.sha256").write_text("".join(self.hash_line(source, name) for name in evidence))
        (prefix / "lib").mkdir(parents=True)
        stub = root / "fixture.c"
        stub.write_text("int fixture(void) { return 0; }\n")
        runtimes = ["lib/libass.so.9", "lib/liblcms2.so.2"]
        for name in runtimes:
            subprocess.run(["cc", "-shared", "-fPIC", f"-Wl,-soname,{Path(name).name}",
                            str(stub), "-o", str(prefix / name)], check=True, capture_output=True)
        (source / "runtime.sha256").write_text("".join(self.hash_line(prefix, name) for name in runtimes))
        environment = os.environ.copy()
        environment.pop("NICONEON_MEDIA_SUPPORT_SOURCE_DIR", None)
        if source_override:
            environment["NICONEON_MEDIA_SUPPORT_SOURCE_DIR"] = str(source)
        output = root / "licenses with spaces"
        return source, prefix, output, ["bash", str(script), str(output), str(prefix)], environment

    def assert_rejected(self, output, command, environment):
        result = subprocess.run(command, env=environment, text=True, capture_output=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertFalse(output.exists(), "failed verification must not produce a source bundle")
        return result

    def test_verified_media_sources_notices_configuration_and_runtime_hashes_are_copied(self):
        for override in [False, True]:
            with self.subTest(source_override=override), tempfile.TemporaryDirectory() as directory:
                source, _, output, command, environment = self.fixture(Path(directory), override)
                result = subprocess.run(command, env=environment, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                expected = {file.relative_to(source): file.read_bytes()
                            for file in source.rglob("*") if file.is_file()}
                copied = output / "media-support-source"
                actual = {file.relative_to(copied): file.read_bytes()
                          for file in copied.rglob("*") if file.is_file()}
                self.assertEqual(actual, expected)
                self.assertFalse(any(".so" in file.name for file in copied.rglob("*")))

    def test_media_corruption_and_missing_manifest_entries_fail_before_copying(self):
        cases = ["libass-source", "lcms-source", "source-manifest", "libass-runtime",
                 "lcms-runtime", "license", "configuration", "missing-license",
                 "missing-configuration", "missing-runtime-entry", "duplicate-runtime-entry",
                 "missing-evidence-entry", "duplicate-evidence-entry"]
        for corrupt in cases:
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                source, prefix, output, command, environment = self.fixture(Path(directory))
                if corrupt in ["libass-source", "lcms-source"]:
                    pattern = "libass-*.tar.xz" if corrupt == "libass-source" else "lcms2-*.tar.gz"
                    next(source.glob(pattern)).write_bytes(b"corrupt source")
                elif corrupt == "source-manifest":
                    with (source / "media-support-sources.sha256").open("a") as manifest:
                        manifest.write("0" * 64 + "  unapproved-source.tar.xz\n")
                elif corrupt in ["libass-runtime", "lcms-runtime"]:
                    name = "libass.so.9" if corrupt == "libass-runtime" else "liblcms2.so.2"
                    # Keep a readable ELF and valid SONAME, so the hash check
                    # (not merely readelf) must detect modified runtime bytes.
                    with (prefix / "lib" / name).open("ab") as runtime:
                        runtime.write(b"corrupt runtime bytes")
                elif corrupt in ["license", "configuration", "missing-license", "missing-configuration"]:
                    name = ("notices/lcms2/LICENSE" if "license" in corrupt
                            else "configuration/libass/config.log")
                    if corrupt.startswith("missing-"):
                        (source / name).unlink()
                    else:
                        (source / name).write_text("corrupt retained evidence")
                else:
                    manifest = source / ("runtime.sha256" if "runtime" in corrupt else "evidence.sha256")
                    lines = manifest.read_text().splitlines(True)
                    manifest.write_text("".join(lines[1:] if corrupt.startswith("missing-")
                                                else lines + [lines[0]]))
                self.assert_rejected(output, command, environment)

    def test_media_manifest_path_traversal_is_rejected_with_matching_hashes(self):
        cases = [("runtime.sha256", "lib/../../outside.so", "invalid media support runtime manifest"),
                 ("runtime.sha256", "lib/../outside.so", "invalid media support runtime manifest"),
                 ("evidence.sha256", "configuration/../../outside", "unsafe media support evidence path"),
                 ("evidence.sha256", "../outside", "unsafe media support evidence path"),
                 ("evidence.sha256", "absolute", "unsafe media support evidence path")]
        for name, path, message in cases:
            with self.subTest(manifest=name, path=path), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, prefix, output, command, environment = self.fixture(root)
                if path == "absolute":
                    path = str(root / "absolute outside")
                base = prefix if name == "runtime.sha256" else source
                (base / path).write_bytes(b"readable outside file with a matching hash")
                # Retain every valid entry, so traversal is rejected on its own.
                with (source / name).open("a") as manifest:
                    manifest.write(self.hash_line(base, path))
                result = self.assert_rejected(output, command, environment)
                self.assertIn(message, result.stderr)

    def test_media_wrong_abi_and_external_symlinks_are_rejected(self):
        for corrupt in ["wrong-soname", "runtime-symlink", "evidence-symlink"]:
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, prefix, output, command, environment = self.fixture(root)
                if corrupt == "wrong-soname":
                    subprocess.run(["cc", "-shared", "-fPIC", "-Wl,-soname,libass.so.10",
                                    str(root / "fixture.c"), "-o", str(prefix / "lib/libass.so.9")],
                                   check=True, capture_output=True)
                    (source / "runtime.sha256").write_text("".join(self.hash_line(prefix, name)
                        for name in ["lib/libass.so.9", "lib/liblcms2.so.2"]))
                    message = "unexpected media support runtime ABI"
                else:
                    original = (prefix / "lib/libass.so.9" if corrupt == "runtime-symlink"
                                else source / "notices/libass/COPYING")
                    outside = root / "external file"
                    outside.write_bytes(original.read_bytes())
                    original.unlink()
                    original.symlink_to(outside)
                    message = ("resolves outside its prefix" if corrupt == "runtime-symlink"
                               else "media support evidence escapes its bundle")
                result = self.assert_rejected(output, command, environment)
                self.assertIn(message, result.stderr)


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
    def test_appimage_tools_use_pinned_assets_and_require_verified_bytes(self):
        payload = b'#!/bin/sh\necho executed >> "$TOOL_EXECUTIONS"\n'
        digest = hashlib.sha256(payload).hexdigest()
        tools = ["appimagetool.AppImage", "runtime-x86_64", "linuxdeploy.AppImage", "linuxdeploy-plugin-qt.AppImage"]
        for case in ["download", "cached", "replace-corrupt-cache", "corrupt-download"]:
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                script = root / "scripts/release/package_linux_appimage.sh"
                script.parent.mkdir(parents=True)
                shutil.copy2(ROOT / "scripts/release/package_linux_appimage.sh", script)
                for name in ["build/release/niconeon", "packaging/appimage/niconeon.desktop",
                             "packaging/appimage/niconeon.png", "LICENSE", "COPYING",
                             "SOURCE_CODE.md", "THIRD_PARTY_NOTICES.txt"]:
                    target = root / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text("fixture")
                binaries = root / "bin"
                binaries.mkdir()
                curl = binaries / "curl"
                curl.write_text(f"#!{sys.executable}\n" + '''import os, pathlib, re, sys
args = sys.argv[1:]
assert "Accept: application/octet-stream" in args
assert re.fullmatch(r"https://api.github.com/repos/[^/]+/[^/]+/releases/assets/[0-9]+", args[-1])
with open(os.environ["FETCH_LOG"], "a") as log:
    log.write(args[-1] + "\\n")
pathlib.Path(args[args.index("-o") + 1]).write_bytes(pathlib.Path(os.environ["FETCH_PAYLOAD"]).read_bytes())
''')
                curl.chmod(0o755)
                # Stop immediately after tool verification, before any native
                # runtime inspection/deployment. No release tool is executed.
                ldd = binaries / "ldd"
                ldd.write_text('#!/bin/sh\necho reached > "$NATIVE_BOUNDARY"\nexit 77\n')
                ldd.chmod(0o755)
                download = root / "payload"
                download.write_bytes(b"corrupt" if case == "corrupt-download" else payload)
                cache = root / "dist/tools"
                cache.mkdir(parents=True)
                if case in ["cached", "replace-corrupt-cache"]:
                    for name in tools:
                        (cache / name).write_bytes(payload if case == "cached" else b"corrupt cache")
                environment = os.environ.copy()
                for name in ["NICONEON_BUILD_DIR", "NICONEON_RELEASE_BASENAME"]:
                    environment.pop(name, None)
                for prefix in ["APPIMAGETOOL", "APPIMAGE_RUNTIME", "LINUXDEPLOY", "LINUXDEPLOY_QT"]:
                    environment.pop(prefix + "_URL", None)
                    environment[prefix + "_SHA256"] = digest
                environment.update(PATH=str(binaries) + os.pathsep + environment["PATH"],
                                   FETCH_PAYLOAD=str(download), FETCH_LOG=str(root / "fetch.log"),
                                   TOOL_EXECUTIONS=str(root / "executions"), NATIVE_BOUNDARY=str(root / "native"))
                result = subprocess.run(["bash", str(script), "test"], env=environment,
                                        text=True, capture_output=True)
                self.assertFalse((root / "executions").exists())
                if case == "corrupt-download":
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse((root / "native").exists())
                    self.assertIn("checksum did NOT match", result.stderr)
                else:
                    self.assertEqual(result.returncode, 77, result.stderr)
                    self.assertTrue((root / "native").exists())
                    for name in tools:
                        self.assertEqual((cache / name).read_bytes(), payload)
                        self.assertTrue(os.access(cache / name, os.X_OK))
                fetched = (root / "fetch.log").read_text().splitlines() if (root / "fetch.log").exists() else []
                self.assertEqual(len(fetched), 0 if case == "cached" else 1 if case == "corrupt-download" else 4)

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

    def test_runtime_hash_rejects_unsupported_or_changed_library(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-runtime-test-") as directory:
            root = Path(directory)
            runtime = root / "libmpv.so.2"
            runtime.write_bytes(b"fixture-supported-runtime")
            digest = hashlib.sha256(runtime.read_bytes()).hexdigest()
            (root / "library.sha256").write_text(f"{digest}  libmpv.so.2\n")
            environment = os.environ.copy()
            environment["NICONEON_MPV_SOURCE_DIR"] = str(root)
            command = ["bash", str(ROOT / "scripts/release/verify_mpv_runtime.sh"), str(runtime)]
            self.assertEqual(subprocess.run(command, env=environment, capture_output=True).returncode, 0)
            runtime.write_bytes(b"fixture-unsupported-runtime")
            self.assertNotEqual(subprocess.run(command, env=environment, capture_output=True).returncode, 0)

    def test_mpv_build_retains_verified_source_and_static_dependency_patch(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-mpv-build-test-") as directory:
            root = Path(directory)
            work, prefix = root / "work", root / "install"
            work.mkdir()
            # Existing 0.41 builds must not be reused by the new release.
            (work / "build").mkdir()
            (work / "build/old-build").write_text("keep")
            fixture = root / "fixture/mpv-0.42.0"
            fixture.mkdir(parents=True)
            (fixture / "Copyright").write_text("fixture upstream license")
            (fixture / "meson.build").write_text("\nlibplacebo = dependency('libplacebo', version: '>=7.387.0',\n                default_options: ['default_library=static', 'demos=false'])\n\nlibass = dependency('libass', version: '>= 0.12.2')\n\n")
            archive = work / "mpv-v0.42.0.tar.gz"
            with tarfile.open(archive, "w:gz") as tar:
                tar.add(fixture, arcname="mpv-0.42.0")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            script = root / "build_mpv.sh"
            original = (ROOT / "scripts/deps/build_mpv.sh").read_text()
            expected = next(line.split("=", 1)[1] for line in original.splitlines()
                            if line.startswith("checksum="))
            script.write_text(original.replace(expected, digest))
            shutil.copy2(ROOT / "scripts/deps/mpv-static-libplacebo.patch", root / "mpv-static-libplacebo.patch")
            meson = root / "meson"
            meson.write_text(f"#!{sys.executable}\n" + """import os, pathlib, sys
work, prefix = pathlib.Path(os.environ["FIXTURE_WORK"]), pathlib.Path(os.environ["FIXTURE_PREFIX"])
args = sys.argv[1:]
if args[0] == "setup":
    assert args[1:3] == [str(work / "build-0.42.0"), str(work / "mpv-0.42.0")]
    assert "nodownload" in args and "-Dplain-gl=enabled" in args
elif args[0] in ["compile", "install"]:
    assert args[1:3] == ["-C", str(work / "build-0.42.0")]
if args[0] == "install":
    (prefix / "lib").mkdir(parents=True, exist_ok=True)
    (prefix / "lib/libmpv.so.2").write_bytes(b"fixture upstream runtime")
""")
            meson.chmod(0o755)
            (root / "nm").write_text("#!/bin/sh\nexit 0\n")
            (root / "nm").chmod(0o755)
            environment = dict(os.environ, NICONEON_MPV_WORK_DIR=str(work),
                               NICONEON_MESON=str(meson), FIXTURE_WORK=str(work), FIXTURE_PREFIX=str(prefix),
                               PATH=str(root) + os.pathsep + os.environ["PATH"])
            for _ in range(2):
                subprocess.run(["bash", str(script), str(prefix)], env=environment,
                               capture_output=True, check=True)
            bundle = prefix / "share/niconeon/libmpv-source"
            self.assertEqual({file.name for file in bundle.iterdir()},
                             {archive.name, "mpv-static-libplacebo.patch", "build_mpv.sh", "Copyright", "README.txt", "library.sha256"})
            self.assertEqual(hashlib.sha256((bundle / archive.name).read_bytes()).hexdigest(), digest)
            self.assertIn(hashlib.sha256(b"fixture upstream runtime").hexdigest(),
                          (bundle / "library.sha256").read_text())
            self.assertEqual((work / "build/old-build").read_text(), "keep")
            copy_script = root / "copy_mpv_source.sh"
            copy_script.write_text((ROOT / "scripts/release/copy_mpv_source.sh").read_text().replace(expected, digest))
            environment["NICONEON_MPV_SOURCE_DIR"] = str(bundle)
            subprocess.run(["bash", str(copy_script), str(root / "licenses")], env=environment, check=True)
            self.assertEqual((root / "licenses/libmpv-source" / archive.name).read_bytes(), archive.read_bytes())
            (root / "nm").write_text("#!/bin/sh\necho '                 U pl_version'\n")
            result = subprocess.run(["bash", str(script), str(prefix)], env=environment,
                                    capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("libplacebo symbols are not isolated", result.stderr)

    def test_static_libplacebo_source_and_link_input_checksums(self):
        for corrupt in [None, "source", "headers", "library", "manifest"]:
            with self.subTest(corrupt=corrupt), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "source with spaces"
                source.mkdir()
                source_payload = b"verified upstream source fixture"
                digest = hashlib.sha256(source_payload).hexdigest()
                archive = source / "libplacebo-v7.387.0.tar.gz"
                archive.write_bytes(source_payload)
                headers = source / "Vulkan-Headers-v1.4.368.tar.gz"
                headers.write_bytes(b"verified matching Vulkan headers")
                headers_digest = hashlib.sha256(headers.read_bytes()).hexdigest()
                library = root / "libplacebo.a"
                library.write_bytes(b"verified PIC static library fixture")
                library_digest = hashlib.sha256(library.read_bytes()).hexdigest()
                (source / "library.sha256").write_text(f"{library_digest}  libplacebo.a\n")
                for name in ["build_libplacebo.sh", "LICENSE", "README.txt", "notices/json.h",
                             "notices/fast-float-copyright", "notices/glad/gl.h",
                             "notices/Vulkan-Headers/LICENSE.md", "notices/Vulkan-Headers/LICENSES/Apache-2.0.txt",
                             "notices/Vulkan-Headers/LICENSES/MIT.txt"]:
                    file = source / name
                    file.parent.mkdir(parents=True, exist_ok=True)
                    file.write_text("fixture attribution")
                script = root / "copy.sh"
                script.write_text((ROOT / "scripts/release/copy_libplacebo_source.sh").read_text().replace(
                    "f9e62d3774a9c4cfd198ec3abaa88294aaade2e31c04d5ab02bcf4abc98b1df5", digest).replace(
                    "5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda", headers_digest))
                if corrupt == "source":
                    archive.write_bytes(b"corrupt source")
                elif corrupt == "headers":
                    headers.write_bytes(b"corrupt headers")
                elif corrupt == "library":
                    library.write_bytes(b"wrong library")
                elif corrupt == "manifest":
                    (source / "library.sha256").write_text(f"{library_digest}  libplacebo.so.387\n")
                output = root / "licenses"
                result = subprocess.run(["bash", str(script), str(output), str(library)],
                                        env=dict(os.environ, NICONEON_LIBPLACEBO_SOURCE_DIR=str(source)),
                                        capture_output=True, text=True)
                if corrupt:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual((output / "libplacebo-source" / archive.name).read_bytes(), source_payload)
                    self.assertTrue((output / "libplacebo-source/notices/glad/gl.h").is_file())

    def test_library_source_rejects_corrupt_archive(self):
        with tempfile.TemporaryDirectory(prefix="niconeon-source-test-") as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            for name in ["mpv-v0.42.0.tar.gz", "mpv-static-libplacebo.patch", "build_mpv.sh", "Copyright", "README.txt", "library.sha256"]:
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
