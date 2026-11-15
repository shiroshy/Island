# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0

param([Parameter(Mandatory=$true)][string]$CompilerRoot,[Parameter(Mandatory=$true)][string]$OutputRoot,[string]$WinrtRoot=(Join-Path $PSScriptRoot '../.build/generated'))
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$compiler=Join-Path $CompilerRoot 'bin/clang++.exe'
& $compiler (Join-Path $PSScriptRoot 'wallpaper_engine_fixture.cpp') -o (Join-Path $OutputRoot 'wallpaper64.exe') -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -municode -luser32 -lgdi32 -lshell32
if($LASTEXITCODE -ne 0){throw 'Engine fixture build failed'}
$test=Join-Path $OutputRoot 'wallpaper-engine-test.exe'
& $compiler (Join-Path $PSScriptRoot 'wallpaper_engine.cpp') -o $test -I $WinrtRoot -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -municode -fms-extensions -Wno-unknown-pragmas -ld3d11 -ldxgi -ld2d1 -ldwrite -ldcomp -ldwmapi -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -luser32 -lgdi32 -lruntimeobject -lwindowscodecs
if($LASTEXITCODE -ne 0){throw 'Engine integration test build failed'}
& $test $OutputRoot
if($LASTEXITCODE -ne 0){throw 'Engine integration test failed'}
