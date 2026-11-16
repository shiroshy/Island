# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0
param([string]$OutputDirectory=(Join-Path $PSScriptRoot 'dist'),[string]$CompilerRoot='',[string]$WinrtRoot='',[string]$CacheRoot='')
$ErrorActionPreference='Stop'
$version=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'VERSION')).Trim()
if($version -ne '0.20.3'){throw 'Public version is pinned to 0.20.3'}
& "$PSScriptRoot\installer\build.ps1" -Version $version -SettingsMode Internal -CompilerRoot $CompilerRoot -WinrtRoot $WinrtRoot -CacheRoot $CacheRoot
if(!$?){throw 'Installer build failed'}
$name="Island-Setup-$version-Win10-11-x64-InternalSettings.exe"
$source=Join-Path "$PSScriptRoot\public-dist" $name
if((Get-Item -LiteralPath $source).VersionInfo.ProductVersion -ne $version){throw 'Installer version mismatch'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$destination=Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) $name
if([IO.Path]::GetFullPath($source) -ne $destination){Copy-Item -LiteralPath $source -Destination $destination -Force}
Write-Output $destination
