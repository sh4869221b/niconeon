#!/usr/bin/env python3
"""Transfer only the supported libmpv SDK files, never the pacman-managed prefix."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import tarfile
import zipfile

DEPS = Path(__file__).resolve().parent
SOURCE = "share/niconeon/libmpv-source/"
ARCHIVE = "mpv-v0.42.0.tar.gz"
UPSTREAM_SHA256 = "bcfebbfa2b65a6d88dc97ea18117831072373e38b5785a2afb874ed4df5070dd"
DLL = "bin/libmpv-2.dll"
HEADERS = tuple("include/mpv/" + name for name in
                ("client.h", "render.h", "render_d3d11.h", "render_gl.h", "stream_cb.h"))
RECIPE = ("build_mpv.sh", "mpv-static-libplacebo.patch")
FILES = (DLL, "lib/libmpv.dll.a", "lib/pkgconfig/mpv.pc", *HEADERS,
         *(SOURCE + name for name in (ARCHIVE, *RECIPE, "Copyright", "README.txt", "library.sha256")))
MANIFEST = "manifest.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def verify_source(files):
    upstream = files[SOURCE + ARCHIVE]
    if digest(upstream) != UPSTREAM_SHA256:
        raise ValueError("wrong upstream libmpv archive")
    for name in RECIPE:
        if files[SOURCE + name] != (DEPS / name).read_bytes():
            raise ValueError("retained recipe differs from checkout: " + name)
    with tarfile.open(fileobj=io.BytesIO(upstream), mode="r:gz") as archive:
        for name in (*HEADERS, "Copyright"):
            expected = archive.extractfile("mpv-0.42.0/" + name).read()
            if files[SOURCE + name if name == "Copyright" else name] != expected:
                raise ValueError("file differs from verified upstream source: " + name)
    if files[SOURCE + "library.sha256"].split() != [digest(files[DLL]).encode(), b"libmpv-2.dll"]:
        raise ValueError("libmpv DLL does not match its corresponding-source record")


def manifest(key, files):
    return {"key": key, "files": {name: digest(data) for name, data in files.items()}}


def pack(prefix, bundle, key, built_dll):
    files = {name: (prefix / name).read_bytes() for name in FILES}
    if files[DLL] != built_dll.read_bytes():
        raise ValueError("installed DLL is not the fresh Meson build output")
    verify_source(files)
    bundle.parent.mkdir(parents=True, exist_ok=True)
    # actions/cache compresses the outer archive. Do not compress twice.
    with zipfile.ZipFile(bundle, "w") as archive:
        for name, data in files.items():
            archive.writestr(name, data)
        archive.writestr(MANIFEST, json.dumps(manifest(key, files)))


def restore(prefix, bundle, key):
    with zipfile.ZipFile(bundle) as archive:
        if sorted(archive.namelist()) != sorted((*FILES, MANIFEST)):
            raise ValueError("unexpected or missing libmpv cache members")
        files = {name: archive.read(name) for name in FILES}
        if json.loads(archive.read(MANIFEST)) != manifest(key, files):
            raise ValueError("libmpv cache identity or file hashes differ")
    verify_source(files)
    # Fully validate before changing any installed file. No archive-supplied paths.
    for name, data in files.items():
        path = prefix / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("pack", "restore"))
    parser.add_argument("prefix", type=Path)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("key")
    parser.add_argument("--built-dll", type=Path)
    args = parser.parse_args()
    if args.mode == "pack":
        if args.built_dll is None:
            parser.error("pack requires --built-dll")
        pack(args.prefix, args.bundle, args.key, args.built_dll)
    else:
        restore(args.prefix, args.bundle, args.key)
