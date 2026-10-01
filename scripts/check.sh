#!/usr/bin/env bash
# Syntax-check src/sierra/*.cpp against the Sierra Chart headers.
# Usage: scripts/check.sh [--bootstrap] [file.cpp]
#   --bootstrap  download portable llvm-mingw (clang) into tools/ when no compiler exists.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INC="$ROOT/third_party/sierra"
SRC="$ROOT/src/sierra"
BOOT=0; FILE=""
for a in "$@"; do
  case "$a" in
    --bootstrap) BOOT=1 ;;
    *) FILE="$a" ;;
  esac
done

if [ ! -f "$INC/sierrachart.h" ]; then
  echo "ERROR: $INC/sierrachart.h missing. Copy C:\\SierraChart\\ACS_Source\\* into third_party/sierra/." >&2
  exit 1
fi

find_clang() {
  local c
  for c in "$ROOT"/tools/llvm-mingw*/bin/clang++.exe "$ROOT"/tools/llvm-mingw*/bin/clang++; do
    [ -x "$c" ] && { echo "$c"; return; }
  done
  command -v clang++ 2>/dev/null && return
  return 1
}

bootstrap() {
  mkdir -p "$ROOT/tools"
  echo "Downloading portable llvm-mingw into tools/ ..."
  local url
  url=$(curl -s https://api.github.com/repos/mstorsjo/llvm-mingw/releases/latest | grep -o 'https://[^"]*ucrt-x86_64.zip' | head -1)
  [ -n "$url" ] || { echo "Could not resolve llvm-mingw download URL" >&2; exit 1; }
  curl -L -o "$ROOT/tools/llvm-mingw.zip" "$url"
  if command -v unzip >/dev/null 2>&1; then
    unzip -q -o "$ROOT/tools/llvm-mingw.zip" -d "$ROOT/tools"
  else
    powershell -NoProfile -Command "Expand-Archive -Path '$ROOT/tools/llvm-mingw.zip' -DestinationPath '$ROOT/tools' -Force"
  fi
  rm -f "$ROOT/tools/llvm-mingw.zip"
}

CXX="$(find_clang || true)"
if [ -z "$CXX" ] && [ "$BOOT" = 1 ]; then bootstrap; CXX="$(find_clang || true)"; fi
if [ -z "$CXX" ]; then CXX="$(command -v g++ || true)"; fi
if [ -z "$CXX" ]; then
  echo "ERROR: no C++ compiler. Run: scripts/check.sh --bootstrap" >&2
  exit 1
fi

if [ -n "$FILE" ]; then FILES=("$FILE"); else FILES=("$SRC"/*.cpp); fi
status=0
for f in "${FILES[@]}"; do
  echo "Checking $(basename "$f") ..."
  "$CXX" -fsyntax-only -std=c++17 -D_CRT_SECURE_NO_WARNINGS \
    -Wall -Wextra -Wno-unused-parameter -Wno-unused-value -Wno-unused-function \
    -Wno-missing-field-initializers -Wno-sign-compare -Wno-unused-variable \
    -Wno-unused-but-set-variable -Wno-deprecated-declarations \
    -isystem "$INC" "$f" || status=1
done
if [ $status -ne 0 ]; then echo "SYNTAX CHECK FAILED"; exit 1; fi
echo "SYNTAX CHECK OK"
