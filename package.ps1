# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0
param([string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist'))
$ErrorActionPreference = 'Stop'
$version = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'VERSION')).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid version.' }
$names = @('Island.exe','Island.Start.exe','Island.Desktop.exe','Island.Files.exe','README.md','LICENSE','THIRD_PARTY_NOTICES','Stop-Island.cmd')
$files = foreach ($name in $names) {
    $path = Join-Path $OutputDirectory $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $name" }
    if ($name.EndsWith('.exe') -and (Get-Item -LiteralPath $path).VersionInfo.FileVersion -ne $version) { throw "Unexpected binary version: $name" }
    $path
}
$archive = Join-Path $OutputDirectory "Island-$version-win-x64.zip"
if (Test-Path -LiteralPath $archive) { throw "Archive already exists: $archive" }
Compress-Archive -LiteralPath $files -DestinationPath $archive -CompressionLevel Optimal
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($archive + '.sha256'), ($hash + '  ' + [IO.Path]::GetFileName($archive) + "`n"), (New-Object Text.UTF8Encoding($false)))
Write-Output $archive
