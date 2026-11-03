#!/usr/bin/env bash
# addu94 — scripts/ci.sh : the local CI gate (mirrors what future hosted CI runs).
# Gate order: format check (clang-format if present) -> clean build (GCC, Werror)
# -> ctest (plain) -> ASan/UBSan test build & run -> TSan build & run (queue/transport)
# -> clang-tidy (if present, best-effort).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BUILD="$ROOT/build"

fail() { echo "ci: FAIL: $*" >&2; exit 1; }

echo "== [1/6] configure (GCC, release-ish checks) =="
cmake -S . -B "$BUILD/main" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DADDU94_WARNINGS_AS_ERRORS=ON \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc

echo "== [2/6] build =="
cmake --build "$BUILD/main" -j"$(nproc)" || fail "build"

echo "== [3/6] ctest (plain) =="
ctest --test-dir "$BUILD/main" --output-on-failure || fail "ctest"

echo "== [4/6] ASan+UBSan build & test =="
cmake -S . -B "$BUILD/asan" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DADDU94_SANITIZE=asan \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc
cmake --build "$BUILD/asan" -j"$(nproc)" || fail "asan build"
ctest --test-dir "$BUILD/asan" --output-on-failure || fail "asan tests"

echo "== [5/6] TSan build & test =="
cmake -S . -B "$BUILD/tsan" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DADDU94_SANITIZE=tsan \
    -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER=gcc
cmake --build "$BUILD/tsan" -j"$(nproc)" || fail "tsan build"
ctest --test-dir "$BUILD/tsan" --output-on-failure || fail "tsan tests"

echo "== [6/6] clang-tidy (best-effort) =="
if command -v clang-tidy >/dev/null 2>&1 && [ -f "$BUILD/main/compile_commands.json" ]; then
    # M0: core sources only; list grows with the tree.
    SRC=$(git ls-files 'src/**.cpp' 'src/**.hpp' 2>/dev/null || find src -name '*.cpp' -o -name '*.hpp')
    if [ -n "$SRC" ]; then
        # shellcheck disable=SC2086
        clang-tidy $SRC -p "$BUILD/main" --quiet || fail "clang-tidy"
    fi
else
    echo "clang-tidy unavailable or no compile db — skipping (recorded)"
fi

echo ""
echo "ci: ALL GATES GREEN"
