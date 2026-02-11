#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
base="niconeon-${version}"
out_dir="${repo_root}/dist"
out_zip="${out_dir}/${base}-source.zip"

mkdir -p "${out_dir}"
git -C "${repo_root}" archive --format=zip --prefix="${base}/" -o "${out_zip}" HEAD

echo "created: ${out_zip}"
