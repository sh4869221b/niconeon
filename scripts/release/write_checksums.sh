#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <version>" >&2
  exit 1
fi

version="$1"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
out_dir="${repo_root}/dist"

mkdir -p "${out_dir}"
shopt -s nullglob
files=("${out_dir}/niconeon-${version}-"*)
shopt -u nullglob

if [[ ${#files[@]} -eq 0 ]]; then
  echo "no release artifacts found for version ${version}" >&2
  exit 1
fi

sum_file="${out_dir}/niconeon-${version}-sha256sums.txt"
: > "${sum_file}"

for file in "${files[@]}"; do
  if [[ "${file}" == "${sum_file}" ]]; then
    continue
  fi
  sha256sum "${file}" >> "${sum_file}"
done

echo "created: ${sum_file}"
