#!/usr/bin/env python3
"""Offline regression tests for the real FFmpeg source/runtime copier."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIBRARIES = (
    "libavcodec.so.63", "libavformat.so.63", "libavdevice.so.63",
    "libavfilter.so.12", "libavutil.so.61", "libswresample.so.7", "libswscale.so.10",
)
SONIC = "https://github.com/FFmpeg/FFmpeg/commit/ca1d253621d4643b7b9ba13ded6dfc68c329fda5"
RAWVIDEO = "https://github.com/FFmpeg/FFmpeg/blob/n9.0.2/libavcodec/raw_pix_fmt_tags.h#L249-L251"
PP = "https://github.com/FFmpeg/FFmpeg/commit/8c920c4c396163e3b9a0b428dd550d3c986236aa"
HLS = "https://github.com/FFmpeg/FFmpeg/commit/64fafd63f0b4ebf8dbbdbdc2296f21a03548b5fc"
ALLOWED = {
    "decoders": {"sonic": SONIC, **dict.fromkeys(("v308", "v408", "v410"), RAWVIDEO)},
    "encoders": {"sonic": SONIC, "sonicls": SONIC,
                 **dict.fromkeys(("v308", "v408", "v410"), RAWVIDEO)},
    "filters": {"pp": PP}, "protocols": {"Input:hls": HLS},
}
REPLACEMENTS = {
    "decoders": {"rawvideo"}, "encoders": {"rawvideo"}, "demuxers": {"hls"},
    "pixel_formats": {"vyu444", "uyva", "v30xle"},
}


@unittest.skipIf(sys.platform != "linux", "FFmpeg source packaging uses Linux shell tools")
class FFmpegSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ffmpeg-source-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.prefix = self.root / "media prefix with spaces"
        self.source = self.prefix / "share/niconeon/ffmpeg-source"
        self.source.mkdir(parents=True)
        (self.prefix / "lib").mkdir()
        self.destination = self.root / "license destination"
        self.script = self.root / "copy_ffmpeg_source.sh"
        script = (ROOT / "scripts/release/copy_ffmpeg_source.sh").read_text()
        # Only archive digest literals change in the test's private script copy.
        # Runtime validation, shell commands and feature policy stay untouched.
        archives = {
            "ffmpeg-9.0.2.tar.xz": "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e",
            "Vulkan-Headers-v1.4.368.tar.gz": "5c5c6f64fcce28a24cc934143a048707af036a858e4ed11d7f71fe016006ecda",
        }
        for name, original in archives.items():
            payload = ("Synthetic archive fixture: " + name + "\n").encode()
            (self.source / name).write_bytes(payload)
            self.assertEqual(script.count(original), 1)
            script = script.replace(original, hashlib.sha256(payload).hexdigest())
        self.script.write_text(script)
        for name in (
            "build_ffmpeg.sh", "README.txt", "baseline-buildconf.txt", "buildconf.txt",
            "check_features.py", "config.log", "config.mak", "config.h",
            "pkg-config-wrapper.sh", "notices/LICENSE.md", "notices/COPYING.GPLv2",
            "notices/COPYING.LGPLv2.1",
        ):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("Synthetic corresponding-source fixture\n")
        lines = []
        for name in LIBRARIES:
            payload = ("Synthetic runtime fixture: " + name).encode()
            (self.prefix / "lib" / name).write_bytes(payload)
            lines.append(f"{hashlib.sha256(payload).hexdigest()}  {name}\n")
        self.manifest = self.source / "libraries.sha256"
        self.manifest.write_text("".join(lines))
        current = {
            "decoders": ["h264", "rawvideo"], "encoders": ["rawvideo"],
            "demuxers": ["hls"], "muxers": ["mp4"], "filters": ["scale"],
            "protocols": ["Input:http"], "hwaccels": ["vulkan"],
            "pixel_formats": ["vyu444", "uyva", "v30xle"],
        }
        self.report = {
            kind: {"baseline": sorted(set(names) | ALLOWED.get(kind, {}).keys()),
                   "current": sorted(names)} for kind, names in current.items()
        }
        self.update_report()

    def update_report(self):
        for kind, item in self.report.items():
            missing = set(item["baseline"]) - set(item["current"])
            accepted = missing & ALLOWED.get(kind, {}).keys()
            item.update(
                missing=sorted(missing), allowed_removals=sorted(accepted),
                unexpected_missing=sorted(missing - accepted),
                upstream_removal_sources={name: ALLOWED[kind][name] for name in sorted(accepted)},
                missing_replacements=sorted(REPLACEMENTS.get(kind, set()) - set(item["current"])),
            )
        self.write_report()

    def write_report(self):
        (self.source / "features.json").write_text(json.dumps(self.report))

    def run_copier(self, success):
        env = os.environ.copy()
        env.pop("NICONEON_FFMPEG_SOURCE_DIR", None)
        # A bogus pkg-config prefix must not override the supplied media prefix.
        env["PKG_CONFIG_PATH"] = str(self.root / "unrelated pkgconfig")
        result = subprocess.run(
            ["bash", str(self.script), str(self.destination), str(self.prefix)],
            capture_output=True, text=True, env=env,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            copied = self.destination / "ffmpeg-source"
            for name in ("features.json", "libraries.sha256", "notices/LICENSE.md"):
                self.assertEqual((copied / name).read_bytes(), (self.source / name).read_bytes())
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertFalse((self.destination / "ffmpeg-source").exists())
        return result

    def test_good_bundle_accepts_exact_documented_removals(self):
        self.assertEqual(sum(len(i["allowed_removals"]) for i in self.report.values()), 11)
        self.run_copier(True)

    def test_rejects_source_corruption(self):
        (self.source / "ffmpeg-9.0.2.tar.xz").write_bytes(b"corrupt")
        self.run_copier(False)

    def test_rejects_vulkan_archive_corruption(self):
        (self.source / "Vulkan-Headers-v1.4.368.tar.gz").write_bytes(b"corrupt")
        self.run_copier(False)

    def test_rejects_runtime_corruption(self):
        (self.prefix / "lib" / LIBRARIES[0]).write_bytes(b"corrupt")
        self.run_copier(False)

    def test_rejects_wrong_manifest_name(self):
        self.manifest.write_text(self.manifest.read_text().replace(LIBRARIES[0], "libavcodec.so.999"))
        self.run_copier(False)

    def test_rejects_duplicate_manifest_entry(self):
        self.manifest.write_text(self.manifest.read_text() + self.manifest.read_text().splitlines(True)[0])
        self.run_copier(False)

    def test_rejects_traversal_manifest_entry(self):
        self.manifest.write_text(self.manifest.read_text().replace(LIBRARIES[0], "../lib/" + LIBRARIES[0]))
        self.run_copier(False)

    def test_rejects_incomplete_manifest(self):
        self.manifest.write_text("".join(self.manifest.read_text().splitlines(True)[1:]))
        self.run_copier(False)

    def test_rejects_unexpected_h264_loss(self):
        self.report["decoders"]["current"].remove("h264")
        self.update_report()
        self.run_copier(False)

    def test_rejects_fabricated_allowed_exception(self):
        item = self.report["decoders"]
        item["current"].remove("h264")
        self.update_report()
        item["allowed_removals"] = sorted(item["allowed_removals"] + ["h264"])
        item["unexpected_missing"] = []
        item["upstream_removal_sources"]["h264"] = SONIC
        self.write_report()
        self.run_copier(False)

    def test_rejects_false_provenance(self):
        self.report["filters"]["upstream_removal_sources"]["pp"] = SONIC
        self.write_report()
        self.run_copier(False)

    def test_rejects_missing_rawvideo_decoder(self):
        self.report["decoders"]["current"].remove("rawvideo")
        self.update_report()
        self.run_copier(False)

    def test_rejects_missing_rawvideo_encoder(self):
        self.report["encoders"]["current"].remove("rawvideo")
        self.update_report()
        self.run_copier(False)

    def test_rejects_missing_hls_replacement(self):
        self.report["demuxers"]["current"].remove("hls")
        self.update_report()
        self.run_copier(False)

    def test_rejects_missing_each_pixel_replacement(self):
        for name in ("vyu444", "uyva", "v30xle"):
            with self.subTest(pixel_format=name):
                item = self.report["pixel_formats"]
                item["current"].remove(name)
                self.update_report()
                self.run_copier(False)
                item["current"].append(name)
                self.update_report()


if __name__ == "__main__":
    unittest.main()
