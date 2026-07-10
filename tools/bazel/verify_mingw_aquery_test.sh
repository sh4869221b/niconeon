#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
validator="${script_dir}/verify_mingw_aquery.sh"

if printf '%s\n' 'Command Line: [C:\Program Files\Microsoft Visual Studio\VC\bin\cl.exe, /c, app.cpp]' | "${validator}"; then
  echo "MSVC CppCompile action was accepted" >&2
  exit 1
fi

if printf '%s\n' 'Command Line: [C:\LLVM\bin\clang-cl.exe, /c, app.cpp]' | "${validator}"; then
  echo "clang-cl CppCompile action was accepted" >&2
  exit 1
fi

if printf '%s\n' 'action without a compiler command line' | "${validator}"; then
  echo "aquery output without a MinGW compiler was accepted" >&2
  exit 1
fi

printf '%s\n' 'Command Line: [D:/a/_temp/msys64/mingw64/bin/gcc, -c, app.cpp]' | "${validator}"
