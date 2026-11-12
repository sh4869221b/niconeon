#!/usr/bin/env bash
# Must run before windeployqt adjusts deployment paths in copied QtCore DLLs.
set -euo pipefail
[[ $# -eq 2 ]] || { echo "usage: $0 <license-directory> <qt-prefix>" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
source_dir="$2/share/niconeon/windows-qt-source"
/usr/bin/python3 - "$source_dir" "$2" "$here/../deps" <<'PY'
import hashlib, pathlib, re, sys
source, prefix, deps = map(pathlib.Path, sys.argv[1:])
def verify(manifest, root):
    seen = set()
    for line in manifest.read_text().splitlines():
        if not line.strip():
            continue
        m = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
        if not m:
            raise SystemExit('malformed Qt manifest')
        name = m[2]
        path = pathlib.PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or '\\' in name or name in seen:
            raise SystemExit('unsafe/duplicate Qt manifest path')
        target = root / path
        if not target.resolve().is_relative_to(root.resolve()):
            raise SystemExit('Qt manifest path escapes its root')
        if not target.is_file() or hashlib.sha256(target.read_bytes()).hexdigest() != m[1]:
            raise SystemExit('Qt evidence/runtime mismatch: ' + str(target))
        seen.add(name)
    if not seen:
        raise SystemExit('empty Qt manifest')
    return seen
for name in ('PKGBUILD', 'PKGBUILD.upstream', 'README.txt', 'build_windows_qt.sh',
             'build.log', 'CMakeCache.txt', 'package-inventory.txt', 'packages.sha256',
             'runtime.sha256', 'qt-sources.sha256', 'windows-qt-recipe.sha256'):
    if not (source / name).is_file():
        raise SystemExit('missing Qt evidence: ' + name)
evidence = verify(source / 'evidence.sha256', source)
actual = {'./' + p.relative_to(source).as_posix() for p in source.rglob('*')
          if p.is_file() and p.name != 'evidence.sha256'}
if evidence != actual:
    raise SystemExit('incomplete Qt evidence manifest')
verify(deps / 'windows-qt-recipe.sha256', source / 'recipe')
expected = '\n'.join(x for x in (deps / 'qt-sources.sha256').read_text().splitlines()
                     if '  qtbase-everywhere' in x or '  CVE-' in x) + '\n'
if (source / 'qt-sources.sha256').read_text() != expected:
    raise SystemExit('Qt source pins mismatch')
verify(source / 'qt-sources.sha256', source)
names = verify(source / 'runtime.sha256', prefix)
if not {'bin/Qt6Core.dll', 'bin/Qt6Network.dll'} <= names or any(not n.endswith('.dll') for n in names):
    raise SystemExit('incomplete QtBase DLL manifest')
if not list((source / 'LICENSES').glob('*')):
    raise SystemExit('missing Qt licenses')
PY
mkdir -p "$1/windows-qt-source"
cp -R "$source_dir/." "$1/windows-qt-source/"
