#!/usr/bin/env bash
set -euo pipefail

find_tool() {
  local name="$1"
  if [[ -n "${LLVM_BINDIR:-}" ]]; then
    if [[ -x "${LLVM_BINDIR}/${name}" ]]; then
      printf '%s\n' "${LLVM_BINDIR}/${name}"
      return 0
    fi
    if [[ -x "${LLVM_BINDIR}/${name}.exe" ]]; then
      printf '%s\n' "${LLVM_BINDIR}/${name}.exe"
      return 0
    fi
  fi
  if command -v "${name}" >/dev/null 2>&1; then
    command -v "${name}"
    return 0
  fi
  if command -v "${name}.exe" >/dev/null 2>&1; then
    command -v "${name}.exe"
    return 0
  fi
  return 1
}

READELF="$(find_tool llvm-readelf || find_tool readelf || true)"
READOBJ="$(find_tool llvm-readobj || true)"
NM="$(find_tool llvm-nm || find_tool nm || true)"
STRINGS="$(find_tool llvm-strings || find_tool strings || true)"
TMP_DIR="${TMPDIR:-/tmp}"

if [[ -z "${STRINGS}" ]]; then
  echo "inspect-imports: strings or llvm-strings is required" >&2
  exit 1
fi

if [[ $# -eq 0 ]]; then
  echo "inspect-imports: no ELF files provided" >&2
  exit 1
fi

needed_libraries() {
  local file="$1"
  if [[ -n "${READOBJ}" ]]; then
    "${READOBJ}" --needed-libs "${file}" 2>/dev/null |
      grep -E '^[[:space:]]+[^[:space:]]+\.sprx$' |
      sed -E 's/^[[:space:]]+//'
  elif [[ -n "${READELF}" ]]; then
    "${READELF}" -d "${file}" 2>/dev/null |
      sed -nE 's/.*Shared library: \[([^]]+)\].*/\1/p'
  fi
}


check_no_direct_appinst_import() {
  local file="$1"
  local needed
  needed="$(needed_libraries "${file}" || true)"
  if printf '%s\n' "${needed}" |
     grep -E 'libSceAppInstUtil\.sprx|libSceSystemService\.sprx|libSceUserService\.sprx|libkernel_sys\.sprx' >/dev/null; then
    echo
    echo "inspect-imports: unsafe direct launcher import in ${file}" >&2
    printf '%s\n' "${needed}" >&2
    return 1
  fi
}


for file in "$@"; do
  echo
  echo "== ${file} =="
  if [[ ! -f "${file}" ]]; then
    echo "missing"
    continue
  fi

  size="$(wc -c < "${file}" | tr -d '[:space:]')"
  echo "file size: ${size} bytes"

  echo "-- dynamic libraries/imports --"
  if [[ -n "${READOBJ}" ]]; then
    "${READOBJ}" --needed-libs "${file}" || true
  elif [[ -n "${READELF}" ]]; then
    "${READELF}" -d "${file}" | grep -E 'NEEDED|SONAME' || true
  else
    echo "no llvm-readobj/readelf available"
  fi

  echo "-- undefined symbols --"
  if [[ -n "${READELF}" ]]; then
    "${READELF}" --dyn-syms "${file}" | grep -E 'UND|Undefined' || true
  elif [[ -n "${NM}" ]]; then
    "${NM}" -u "${file}" || true
  else
    echo "no llvm-readelf/readelf/nm available"
  fi

  echo "-- SCE-related strings/symbol hints --"
  "${STRINGS}" "${file}" | grep -Ei '(^|[^A-Za-z0-9_])(sce|Sce|AppInst|SystemService|UserService|BFPL00001|app_installer)' || true
done

for file in "$@"; do
  case "${file}" in
    bfpilot-launcher-installer.elf|./bfpilot-launcher-installer.elf)
      continue
      ;;
  esac
  if [[ -f "${file}" ]]; then
    check_no_direct_appinst_import "${file}"
  fi
done

if [[ -f bfpilot.elf ]]; then
  if "${STRINGS}" bfpilot.elf |
     grep -E 'SceAppInstUtil|sceAppInst|app_installer|BFPL00001' >"${TMP_DIR}/bfpilot-forbidden-imports.txt"; then
    echo
    echo "inspect-imports: forbidden launcher/AppInst content in bfpilot.elf" >&2
    cat "${TMP_DIR}/bfpilot-forbidden-imports.txt" >&2
    exit 1
  fi
fi

if [[ -f bfpilot-launcher-installer.elf ]]; then
  installer_needed="$(needed_libraries bfpilot-launcher-installer.elf || true)"
  for required in libkernel_sys.sprx libSceSystemService.sprx \
                  libSceUserService.sprx libSceAppInstUtil.sprx; do
    if ! printf '%s\n' "${installer_needed}" | grep -q "${required}"; then
      echo
      echo "inspect-imports: bfpilot-launcher-installer.elf missing ${required}" >&2
      exit 1
    fi
  done
fi

echo
echo "inspect-imports: compatibility checks passed"
