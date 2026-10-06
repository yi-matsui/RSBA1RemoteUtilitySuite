#!/bin/sh
# Linux 用ビルドスクリプト
#   ./scripts/build.sh          ネイティブビルド＋テスト（Raspberry Pi 上など）
#   ./scripts/build.sh cross    aarch64 クロスビルド（テストは実行しない）
set -eu

cd "$(dirname "$0")/.."

case "${1:-native}" in
native)
    cmake --preset linux
    cmake --build --preset linux
    ctest --preset linux
    ;;
cross)
    cmake --preset linux-aarch64-cross
    cmake --build --preset linux-aarch64-cross
    ;;
*)
    echo "usage: $0 [native|cross]" >&2
    exit 2
    ;;
esac
