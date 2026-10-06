cmake_minimum_required(VERSION 3.21)

project(rsba1_remote_utility_suite
    VERSION 0.1.0
    DESCRIPTION "RS-BA1 Remote Utility compatible suite for IC-9100"
    LANGUAGES C)

option(RSBA_BUILD_TESTS "Build unit tests" ON)
option(RSBA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(RSBA_REQUIRE_ALSA "Linux: fail configuration if ALSA (libasound2-dev) is missing" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 全ターゲット共通のコンパイル設定
add_library(rsba_build_options INTERFACE)
target_compile_definitions(rsba_build_options INTERFACE
    RSBA_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(rsba_build_options INTERFACE /W4 /utf-8)
    if(RSBA_WARNINGS_AS_ERRORS)
        target_compile_options(rsba_build_options INTERFACE /WX)
    endif()
else()
    target_compile_options(rsba_build_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
    if(RSBA_WARNINGS_AS_ERRORS)
        target_compile_options(rsba_build_options INTERFACE -Werror)
    endif()
endif()

if(WIN32)
    # Windows 10 以降を対象とする
    target_compile_definitions(rsba_build_options INTERFACE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
else()
    # -std=c11 (拡張なし) で POSIX / BSD API を有効化する
    target_compile_definitions(rsba_build_options INTERFACE
        _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()

add_subdirectory(src/common)
add_subdirectory(src/server)
add_subdirectory(src/client)

if(RSBA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
