# Windows 10 用ビルドスクリプト (Visual Studio 2022 / MSVC x64)
#   .\scripts\build.ps1           ビルド＋テスト
#   .\scripts\build.ps1 -NoTest   ビルドのみ
param([switch]$NoTest)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

cmake --preset windows-msvc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build --preset windows-msvc
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if (-not $NoTest) {
    ctest --preset windows-msvc
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
