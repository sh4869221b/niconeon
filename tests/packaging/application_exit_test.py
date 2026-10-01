#!/usr/bin/env python3
"""Exercise the real executable's asynchronous shutdown, including Main.qml."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="niconeon-exit-test-") as directory:
    root = Path(directory)
    environment = os.environ.copy()
    for key in ["NICONEON_AUTO_VIDEO_PATH", "NICONEON_NICONICO_COOKIE", "NICONICO_COOKIE", "NICONEON_AUTO_PERF_LOG", "NICONEON_SYNTHETIC_COMMENTS"]:
        environment.pop(key, None)
    environment.update({
        "QT_QPA_PLATFORM": "offscreen",
        "QT_QUICK_BACKEND": "software",
        "NICONEON_AUTO_EXIT_MS": "150",
        "NICONEON_DANMAKU_WORKER": "off",
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_DATA_HOME": str(root / "data"),
        "XDG_CACHE_HOME": str(root / "cache"),
    })
    for mode in ("", "default", "bilinear", "invalid-test-value"):
        environment["NICONEON_MPV_SCALE"] = mode
        result = subprocess.run([sys.argv[1]], env=environment, timeout=15,
                                text=True, capture_output=True)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if result.returncode != 0:
            raise SystemExit(result.returncode)
        if mode == "bilinear":
            assert result.stderr.count("softer compatibility scaling enabled") == 1
            assert "was rejected" not in result.stderr
        elif mode == "invalid-test-value":
            assert result.stderr.count("unknown NICONEON_MPV_SCALE") == 1
            assert "compatibility scaling enabled" not in result.stderr
        else:
            assert "NICONEON_MPV_SCALE" not in result.stderr
        print(f"Full application entered and exited its QML event loop: scale={mode or '<empty>'}")
