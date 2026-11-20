#!/usr/bin/env python3
"""Keep the independent media bootstrap on the Linux SDK toolchain."""
from pathlib import Path
import re
import unittest


DEPS = Path(__file__).resolve().parents[2] / "scripts/deps"
FETCH = re.compile(r"^fetch (\S+) \\\n +(?P<url>https://\S+) \\\n +(?P<sha>[0-9a-f]{64})$", re.MULTILINE)


class LinuxMediaToolTests(unittest.TestCase):
    def test_media_tool_archives_match_full_sdk_without_llvm(self):
        def pins(name):
            entries = FETCH.findall((DEPS / name).read_text())
            self.assertEqual(len(entries), len({path for path, _, _ in entries}))
            return entries

        media = pins("install_media_build_tools.sh")
        full = pins("install_build_tools.sh")
        self.assertEqual([path.split("-", 1)[0] for path, _, _ in media],
                         ["cmake", "ninja", "meson"])
        self.assertEqual(media, full[:len(media)])


if __name__ == "__main__":
    unittest.main()
