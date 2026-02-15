#!/usr/bin/env bash
set -euo pipefail

output_path="${1:-CHANGELOG.md}"

mapfile -t tags < <(git tag --sort=-version:refname --list 'v*')

if [[ ${#tags[@]} -eq 0 ]]; then
  echo "error: no v* tags found" >&2
  exit 1
fi

mkdir -p "$(dirname "${output_path}")"

{
  echo "# Changelog"
  echo
  echo "_Generated from git commit subjects (merge commits excluded)._"
  echo

  for i in "${!tags[@]}"; do
    current_tag="${tags[$i]}"
    release_date="$(git log -1 --format=%cs "${current_tag}")"

    if (( i + 1 < ${#tags[@]} )); then
      previous_tag="${tags[$((i + 1))]}"
      range="${previous_tag}..${current_tag}"
    else
      range="${current_tag}"
    fi

    mapfile -t subjects < <(git log --no-merges --reverse --pretty=format:%s "${range}")

    echo "## ${current_tag} (${release_date})"
    echo
    echo "_Range: \`${range}\`_"
    echo

    if [[ ${#subjects[@]} -eq 0 ]]; then
      echo "- 変更なし"
    else
      for subject in "${subjects[@]}"; do
        echo "- ${subject}"
      done
    fi

    echo
  done
} > "${output_path}"
