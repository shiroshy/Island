# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0

param([Parameter(Mandatory=$true)][string]$CompilerRoot)
$ErrorActionPreference='Stop'
$testBinary=Join-Path $env:TEMP ('OwnIslandWallpaperTests-'+$PID+'.exe')
& (Join-Path $CompilerRoot 'bin\clang++.exe') (Join-Path $PSScriptRoot 'wallpaper_playback.cpp') -o $testBinary -std=c++20 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -O2 -static -municode -lole32 -loleaut32 -luuid -lwindowscodecs -lshell32 -luser32 -ld3d11 -lmfplat -lmfreadwrite -lmfuuid
if($LASTEXITCODE -ne 0){throw 'Wallpaper test compilation failed'}
try {& $testBinary;if($LASTEXITCODE -ne 0){throw 'Wallpaper playback regression failed'}} finally {Remove-Item -LiteralPath $testBinary -ErrorAction SilentlyContinue}
