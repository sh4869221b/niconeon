#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <current-tag> <output-path>" >&2
  exit 1
fi

current_tag="$1"
output_path="$2"

if ! git rev-parse -q --verify "refs/tags/${current_tag}" >/dev/null; then
  echo "error: tag not found: ${current_tag}" >&2
  exit 1
fi

previous_tag=""
if previous_tag="$(git describe --tags --abbrev=0 "${current_tag}^" --match 'v*' 2>/dev/null)"; then
  range="${previous_tag}..${current_tag}"
  range_label="${previous_tag}..${current_tag}"
else
  range="${current_tag}"
  range_label="(initial)..${current_tag}"
fi

mkdir -p "$(dirname "${output_path}")"

mapfile -t commit_subjects < <(git log --no-merges --reverse --pretty=format:%s "${range}")

{
  echo "## Commit History"
  echo
  echo "_Range: \`${range_label}\`_"
  echo
  if [[ ${#commit_subjects[@]} -eq 0 ]]; then
    echo "- 変更なし"
  else
    for subject in "${commit_subjects[@]}"; do
      echo "- ${subject}"
    done
  fi
  echo
  echo "## GitHub Auto Notes"
  echo
  echo "_以下は GitHub により自動生成されます。_"
} > "${output_path}"
