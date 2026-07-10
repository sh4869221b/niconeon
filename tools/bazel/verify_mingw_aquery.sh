#!/usr/bin/env bash
set -euo pipefail

aquery_output="$(cat)"
msvc_pattern='(^|[/\\])(cl|clang-cl)(\.exe)?([^[:alnum:]_.+-]|$)'
mingw_pattern='(^|[/\\])mingw64[/\\]bin[/\\](gcc|g\+\+)(\.exe)?([^[:alnum:]_.+-]|$)'

if grep -Eiq "${msvc_pattern}" <<<"${aquery_output}"; then
  echo "MSVC-compatible compiler found in MinGW CppCompile actions" >&2
  exit 1
fi

mingw_compiler="$(grep -Eio "${mingw_pattern}" <<<"${aquery_output}" | head -n1 || true)"
if [[ -z "${mingw_compiler}" ]]; then
  echo "MinGW compiler was not found in CppCompile actions" >&2
  exit 1
fi

echo "Verified MinGW C++ compiler: ${mingw_compiler}"
