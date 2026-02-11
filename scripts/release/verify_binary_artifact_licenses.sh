#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <artifact-path>" >&2
  exit 1
fi

artifact="$1"
if [[ ! -f "${artifact}" ]]; then
  echo "artifact not found: ${artifact}" >&2
  exit 1
fi

required=(
  "LICENSE"
  "COPYING"
  "SOURCE_CODE.md"
  "THIRD_PARTY_NOTICES.txt"
)

require_in_zip() {
  local archive="$1"
  local listing=""
  local name=""

  if ! command -v unzip >/dev/null 2>&1; then
    echo "unzip is required to inspect zip artifacts" >&2
    exit 1
  fi

  listing="$(unzip -Z1 "${archive}")"
  for name in "${required[@]}"; do
    if ! grep -E "/${name}$" >/dev/null <<<"${listing}"; then
      echo "missing ${name} in archive: ${archive}" >&2
      exit 1
    fi
  done
}

require_in_appimage() {
  local image="$1"
  local workdir=""
  local name=""
  local base=""

  workdir="$(mktemp -d "${TMPDIR:-/tmp}/niconeon-appimage-check-XXXXXX")"

  (
    cd "${workdir}"
    APPIMAGE_EXTRACT_AND_RUN=1 "${image}" --appimage-extract >/dev/null
  )

  base="${workdir}/squashfs-root/usr/share/licenses/niconeon"
  for name in "${required[@]}"; do
    if [[ ! -f "${base}/${name}" ]]; then
      echo "missing ${name} in AppImage: ${image}" >&2
      rm -rf "${workdir}"
      exit 1
    fi
  done

  rm -rf "${workdir}"
}

case "${artifact}" in
  *.zip)
    require_in_zip "${artifact}"
    ;;
  *.AppImage)
    require_in_appimage "${artifact}"
    ;;
  *)
    echo "unsupported artifact type: ${artifact}" >&2
    exit 1
    ;;
esac

echo "license files verified: ${artifact}"
