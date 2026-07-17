#!/usr/bin/env bash
# Compatibility checks for BFpilot ELFs.
# v0.4.1+: main bfpilot.elf embeds Media-tile AppInst (Payload Manager model).
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

# Primary ELF must include Media-tile AppInst (single-ELF model).
elf_has_string() {
  local file="$1"
  local needle="$2"
  # -a: treat as text on Windows/msys; tolerate null-containing streams
  if "${STRINGS}" "${file}" 2>/dev/null | grep -aF "${needle}" >/dev/null 2>&1; then
    return 0
  fi
  # Fallback: raw bytes (PowerShell/host often more reliable than llvm-strings path)
  if command -v python3 >/dev/null 2>&1; then
    python3 - "${file}" "${needle}" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
sys.exit(0 if sys.argv[2].encode() in data else 1)
PY
    return $?
  fi
  return 1
}

if [[ -f bfpilot.elf ]]; then
  main_needed="$(needed_libraries bfpilot.elf || true)"
  for required in libSceAppInstUtil.sprx libSceUserService.sprx; do
    if ! printf '%s\n' "${main_needed}" | grep -q "${required}"; then
      echo
      echo "inspect-imports: bfpilot.elf missing required ${required} (Media tile)" >&2
      printf '%s\n' "${main_needed}" >&2
      exit 1
    fi
  done
  if ! elf_has_string bfpilot.elf 'BFPL00001'; then
    echo "inspect-imports: bfpilot.elf missing BFPL00001 title id" >&2
    exit 1
  fi
  if ! elf_has_string bfpilot.elf 'applicationCategoryType'; then
    echo "inspect-imports: bfpilot.elf missing embedded param applicationCategoryType" >&2
    exit 1
  fi
  echo "inspect-imports: bfpilot.elf Media tile AppInst imports OK"
fi

# Optional recovery installer still needs full privileged set.
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
  echo "inspect-imports: launcher-installer AppInst imports OK"
fi

# Archive worker must NOT pull AppInst (file manager isolation for worker).
if [[ -f bfpilot-archive-worker.elf ]]; then
  worker_needed="$(needed_libraries bfpilot-archive-worker.elf || true)"
  if printf '%s\n' "${worker_needed}" | grep -E 'libSceAppInstUtil\.sprx' >/dev/null; then
    echo "inspect-imports: archive-worker must not link AppInstUtil" >&2
    exit 1
  fi
fi

echo
echo "inspect-imports: compatibility checks passed"
