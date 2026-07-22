#!/usr/bin/env bash
# Compatibility checks for BFpilot ELFs.
# Main bfpilot.elf must NOT link AppInst (breaks loaders). Tile install is the
# separate bfpilot-launcher-installer.elf only (Media category 65536).
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
if ! mkdir -p "${TMP_DIR}" >/dev/null 2>&1; then
  TMP_DIR="build/tmp"
  mkdir -p "${TMP_DIR}"
fi

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


check_ps5_elf_header() {
  local file="$1"
  local header
  if [[ -z "${READELF}" ]]; then
    echo "inspect-imports: readelf or llvm-readelf is required for ELF validation" >&2
    return 1
  fi
  header="$("${READELF}" -h "${file}" 2>/dev/null)"
  for expected in \
    'Class:.*ELF64' \
    'Data:.*little endian' \
    'OS/ABI:.*FreeBSD' \
    'Type:.*DYN' \
    'Machine:.*(X86-64|x86-64)'; do
    if ! printf '%s\n' "${header}" | grep -E "${expected}" >/dev/null; then
      echo "inspect-imports: ${file} has an invalid PS5 ELF header (${expected})" >&2
      return 1
    fi
  done
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

  check_ps5_elf_header "${file}"

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
  if "${STRINGS}" bfpilot.elf 2>/dev/null |
     grep -aE 'libSceAppInstUtil\.sprx|sceAppInstUtilInitialize' >"${TMP_DIR}/bfpilot-forbidden-imports.txt" 2>/dev/null; then
    echo
    echo "inspect-imports: forbidden launcher/AppInst content in bfpilot.elf" >&2
    cat "${TMP_DIR}/bfpilot-forbidden-imports.txt" >&2
    exit 1
  fi
  # Scrub may rewrite sprx name; still reject live AppInst symbols
  if command -v python3 >/dev/null 2>&1; then
    if python3 - <<'PY'
import pathlib
data = pathlib.Path("bfpilot.elf").read_bytes()
# NEEDED-style / live install entry points must not remain
needles = [b"sceAppInstUtilInitialize", b"sceAppInstUtilAppInstallAll"]
for n in needles:
    if n in data:
        raise SystemExit(1)
raise SystemExit(0)
PY
    then
      :
    else
      echo "inspect-imports: forbidden AppInst symbols in bfpilot.elf" >&2
      exit 1
    fi
  fi
  echo "inspect-imports: bfpilot.elf free of AppInst (file manager only) OK"
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
  if ! "${STRINGS}" bfpilot-launcher-installer.elf 2>/dev/null | grep -aF 'BFPL00001' >/dev/null; then
    echo "inspect-imports: installer missing BFPL00001" >&2
    exit 1
  fi
  if ! "${STRINGS}" bfpilot-launcher-installer.elf 2>/dev/null | grep -aF '65536' >/dev/null; then
    echo "inspect-imports: installer missing Media category 65536" >&2
    exit 1
  fi
  echo "inspect-imports: launcher-installer Media AppInst OK"
fi

echo
echo "inspect-imports: compatibility checks passed"
