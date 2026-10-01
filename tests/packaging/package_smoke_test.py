#!/usr/bin/env python3
"""Check the release layout and AppRun contract without downloading release tools."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[2]


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
