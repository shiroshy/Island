# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0
param([Parameter(Mandatory=$true)][string]$CompilerRoot, [string]$OutputRoot = (Join-Path $PSScriptRoot '../.build/regressions'))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$test = Join-Path $OutputRoot 'tray-layout-test.exe'
& (Join-Path $CompilerRoot 'bin/clang++.exe') (Join-Path $PSScriptRoot 'tray_layout.cpp') -o $test -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -municode -lcomctl32 -luser32 -lgdi32
if ($LASTEXITCODE -ne 0) { throw 'Tray regression compilation failed.' }
& $test (Join-Path $OutputRoot 'tray-state')
if ($LASTEXITCODE -ne 0) { throw 'Tray regression failed.' }
& (Join-Path $PSScriptRoot 'wallpaper.ps1') -CompilerRoot $CompilerRoot
if ($LASTEXITCODE -ne 0) { throw 'Wallpaper regression failed.' }
