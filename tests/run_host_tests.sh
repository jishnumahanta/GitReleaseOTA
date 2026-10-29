#!/usr/bin/env bash
# Builds and runs the host-side unit tests for src/gro_core.cpp.
# Needs: a C++11 compiler, mbedTLS headers+libs, and the openssl CLI.
#   macOS:  brew install mbedtls
#   Debian: apt install libmbedtls-dev
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

CXX="${CXX:-c++}"
PREFIX="${MBEDTLS_PREFIX:-}"
if [ -z "$PREFIX" ] && command -v brew >/dev/null 2>&1; then PREFIX="$(brew --prefix mbedtls 2>/dev/null || true)"; fi
INC=(); LIBDIR=()
if [ -n "$PREFIX" ]; then INC=(-I"$PREFIX/include"); LIBDIR=(-L"$PREFIX/lib" "-Wl,-rpath,$PREFIX/lib"); fi

# mbedTLS 4.x ships crypto in libtfpsacrypto; 2.x/3.x in libmbedcrypto.
LIBS=(-lmbedcrypto)
if [ -n "$PREFIX" ] && ls "$PREFIX"/lib/libtfpsacrypto* >/dev/null 2>&1; then LIBS=(-ltfpsacrypto); fi

echo "== building tests"
"$CXX" -std=c++11 -Wall -Wextra -Werror "${INC[@]}" "$ROOT/tests/host/test_core.cpp" "$ROOT/src/gro_core.cpp" \
  "${LIBDIR[@]}" "${LIBS[@]}" -o "$WORK/test_core"

echo "== generating signing material with: $(openssl version)"
openssl ecparam -name prime256v1 -genkey -noout -out "$WORK/k1.pem" 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out "$WORK/k2.pem" 2>/dev/null
openssl ec -in "$WORK/k1.pem" -pubout -out "$WORK/k1.pub" 2>/dev/null
openssl ec -in "$WORK/k2.pem" -pubout -out "$WORK/k2.pub" 2>/dev/null

export T_VER="1.2.3"
export T_SHA="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
printf '%s' "$T_VER:$T_SHA" | openssl dgst -sha256 -sign "$WORK/k1.pem" | base64 | tr -d '\n' > "$WORK/sig.b64"
export T_PUB="$WORK/k1.pub" T_PUB_OTHER="$WORK/k2.pub" T_SIG="$WORK/sig.b64"

echo "== running"
"$WORK/test_core"
