#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
exec python3 "${repo_root}/scripts/release/generate_third_party_notices.py" "$@"
