# Raspberry Pi OS 64-bit (aarch64) 向けクロスビルド用ツールチェイン
#
#   cmake --preset linux-aarch64-cross
#
# コンパイラのプレフィックスは RSBA_CROSS_PREFIX で変更できる。
#   Debian/Ubuntu (gcc-aarch64-linux-gnu): aarch64-linux-gnu-（既定）
#   Windows 上の Arm GNU Toolchain:        aarch64-none-linux-gnu-
# Raspberry Pi のライブラリを参照する場合は環境変数 RSBA_SYSROOT に sysroot を指定する。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED RSBA_CROSS_PREFIX)
    if(DEFINED ENV{RSBA_CROSS_PREFIX})
        set(RSBA_CROSS_PREFIX "$ENV{RSBA_CROSS_PREFIX}")
    else()
        set(RSBA_CROSS_PREFIX "aarch64-linux-gnu-")
    endif()
endif()
set(RSBA_CROSS_PREFIX "${RSBA_CROSS_PREFIX}" CACHE STRING "Cross compiler prefix")

set(CMAKE_C_COMPILER "${RSBA_CROSS_PREFIX}gcc")

if(DEFINED ENV{RSBA_SYSROOT})
    set(CMAKE_SYSROOT "$ENV{RSBA_SYSROOT}")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
