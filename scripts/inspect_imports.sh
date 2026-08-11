#!/usr/bin/env bash
# Structural and dependency checks for the one-ELF BFpilot release.
set -euo pipefail

find_tool() {
  local name="$1"
  if [[ -n "${LLVM_BINDIR:-}" ]]; then
    [[ -x "${LLVM_BINDIR}/${name}" ]] && { printf '%s\n' "${LLVM_BINDIR}/${name}"; return; }
    [[ -x "${LLVM_BINDIR}/${name}.exe" ]] && { printf '%s\n' "${LLVM_BINDIR}/${name}.exe"; return; }
  fi
  command -v "${name}" 2>/dev/null || command -v "${name}.exe" 2>/dev/null || true
}

READELF="$(find_tool llvm-readelf)"
[[ -n "${READELF}" ]] || READELF="$(find_tool readelf)"
READOBJ="$(find_tool llvm-readobj)"
STRINGS="$(find_tool llvm-strings)"
[[ -n "${STRINGS}" ]] || STRINGS="$(find_tool strings)"

[[ -n "${READELF}" ]] || { echo "inspect-imports: readelf is required" >&2; exit 1; }
[[ -n "${STRINGS}" ]] || { echo "inspect-imports: strings is required" >&2; exit 1; }

file="${1:-bfpilot.elf}"
[[ $# -le 1 ]] || { echo "inspect-imports: expected one release ELF" >&2; exit 1; }
[[ -f "${file}" ]] || { echo "inspect-imports: missing ${file}" >&2; exit 1; }

for obsolete in \
  bfpilot-launcher-installer.elf \
  bfpilot-pkg-installer.elf \
  bfpilot-archive-worker.elf \
  build/internal/bfpilot-appinst-helper.elf; do
  if [[ -f "${obsolete}" ]]; then
    echo "inspect-imports: obsolete helper artifact still exists: ${obsolete}" >&2
    exit 1
  fi
done

header="$("${READELF}" -h "${file}")"
for expected in \
  'Class:.*ELF64' \
  'Data:.*little endian' \
  'OS/ABI:.*FreeBSD' \
  'Type:.*DYN' \
  'Machine:.*(X86-64|x86-64)'; do
  printf '%s\n' "${header}" | grep -E "${expected}" >/dev/null || {
    echo "inspect-imports: invalid PS5 ELF header (${expected})" >&2
    exit 1
  }
done

needed_libraries() {
  if [[ -n "${READOBJ}" ]]; then
    "${READOBJ}" --needed-libs "$1" 2>/dev/null |
      grep -E '^[[:space:]]+[^[:space:]]+\.sprx$' | sed -E 's/^[[:space:]]+//'
  else
    "${READELF}" -d "$1" 2>/dev/null |
      sed -nE 's/.*Shared library: \[([^]]+)\].*/\1/p'
  fi
}

needed="$(needed_libraries "${file}" || true)"
for required in libSceIpmi.sprx libSceAppInstUtil.sprx; do
  printf '%s\n' "${needed}" | grep -Fx "${required}" >/dev/null || {
    echo "inspect-imports: one-ELF installer is missing ${required}" >&2
    printf '%s\n' "${needed}" >&2
    exit 1
  }
done
if printf '%s\n' "${needed}" |
   grep -E 'libSceSystemService\.sprx|libSceUserService\.sprx|libkernel_sys\.sprx' >/dev/null; then
  echo "inspect-imports: unnecessary privileged dependency found" >&2
  printf '%s\n' "${needed}" >&2
  exit 1
fi

require_string() {
  "${STRINGS}" "${file}" 2>/dev/null | grep -aF "$1" >/dev/null || {
    echo "inspect-imports: missing all-in-one marker: $1" >&2
    exit 1
  }
}

require_string 'sceAppInstUtilInitialize'
require_string 'sceAppInstUtilInstallByPackage'
require_string 'sceAppInstUtilAppInstallAll'
require_string 'direct_tile+embedded_pkg_worker'
require_string 'BFPL00001'
require_string '65536'
require_string '/api/fs/pkg/install'
require_string '/api/fs/archive/prepare'
require_string '/api/fs/download/prepare'

abi_source="src/pkg_worker_main.c"
for shape in \
  'languages[30][8]' \
  'playgo_scenario_ids[64][3]' \
  'content_ids[64][48]' \
  'unknown[6480]' \
  'sizeof(appinst_playgo_info_t) == 9984'; do
  grep -F "${shape}" "${abi_source}" >/dev/null || {
    echo "inspect-imports: AppInst ABI guard missing: ${shape}" >&2
    exit 1
  }
done

if grep -R -E 'appinst_dispatch|appinst_helper|internal_helper_blob' \
     Makefile src >/dev/null; then
  echo "inspect-imports: obsolete AppInst helper architecture remains" >&2
  exit 1
fi

echo "== ${file} =="
echo "file size: $(wc -c < "${file}" | tr -d '[:space:]') bytes"
echo "needed libraries:"
printf '%s\n' "${needed}"
grep -F 'PKG_WORKER_BIN := build/internal/' Makefile >/dev/null || {
  echo "inspect-imports: private embedded PKG worker is not wired" >&2
  exit 1
}
grep -F 'INCASSET(bfpilot_pkg_worker_elf' src/pkg_worker_client.c >/dev/null || {
  echo "inspect-imports: private PKG worker is not embedded" >&2
  exit 1
}

echo "inspect-imports: one distributed ELF with direct tile AppInst and embedded PKG worker passed"
