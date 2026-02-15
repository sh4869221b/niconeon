#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <release-tag> <default-branch>" >&2
  exit 1
fi

release_tag="$1"
default_branch="$2"
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"

cd "${repo_root}"

git fetch --no-tags origin "${default_branch}"
git switch -C "${default_branch}" "origin/${default_branch}"

"${repo_root}/scripts/release/generate_full_changelog.sh" "${repo_root}/CHANGELOG.md"

if [[ -z "$(git status --porcelain -- CHANGELOG.md)" ]]; then
  echo "CHANGELOG.md is already up to date."
  exit 0
fi

git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"

git add CHANGELOG.md
git commit -m "docs(changelog): update for ${release_tag}"
git push origin "HEAD:${default_branch}"
