# Copyright 2026 Shizuka
# SPDX-License-Identifier: Apache-2.0
param(
    [string]$CompilerRoot,
    [string]$OutputPath,
    [string]$WinrtRoot,
    [string]$CacheRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '.build'))
)
$ErrorActionPreference = 'Stop'
if (-not $OutputPath) { $OutputPath = Join-Path $PSScriptRoot 'dist\Island.exe' }
New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null
function Get-Archive([string]$Url, [string]$Name, [string]$Destination) {
    $archive = Join-Path $CacheRoot $Name
    if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $Url -OutFile $archive }
    Expand-Archive -LiteralPath $archive -DestinationPath $Destination -Force
}
if (-not $CompilerRoot) {
    $CompilerRoot = Join-Path $CacheRoot 'llvm-mingw-20261020-ucrt-x86_64'
    if (-not (Test-Path -LiteralPath (Join-Path $CompilerRoot 'bin\clang++.exe'))) {
        Get-Archive 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip' 'compiler.zip' $CacheRoot
    }
}
if (-not $WinrtRoot) {
    $WinrtRoot = Join-Path $CacheRoot 'generated'
    if (-not (Test-Path -LiteralPath (Join-Path $WinrtRoot 'winrt\Windows.Media.Control.h'))) {
        $contracts = Join-Path $CacheRoot 'contracts'
        $cppwinrt = Join-Path $CacheRoot 'cppwinrt'
        Get-Archive 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.contracts/10.0.19041.1/microsoft.windows.sdk.contracts.10.0.19041.1.nupkg' 'contracts.zip' $contracts
        Get-Archive 'https://api.nuget.org/v3-flatcontainer/microsoft.windows.cppwinrt/2.0.240405.15/microsoft.windows.cppwinrt.2.0.240405.15.nupkg' 'cppwinrt.zip' $cppwinrt
        & (Join-Path $cppwinrt 'bin\cppwinrt.exe') -input (Join-Path $contracts 'ref\netstandard2.0') -output $WinrtRoot
        if ($LASTEXITCODE -ne 0) { throw 'C++/WinRT generation failed.' }
    }
}
$outputDirectory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath))
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$src = Join-Path $PSScriptRoot 'src'
$resource = Join-Path $CacheRoot 'resources.o'
& (Join-Path $CompilerRoot 'bin\llvm-windres.exe') -I $src (Join-Path $src 'Island.rc') $resource
if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }
$compileFlags = @('-std=c++20', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-I', $WinrtRoot, '-O2', '-fms-extensions', '-Wno-unknown-pragmas')
$pathMappings = @(
    ('-ffile-prefix-map=' + [IO.Path]::GetFullPath($PSScriptRoot) + '=.'),
    ('-ffile-prefix-map=' + [IO.Path]::GetFullPath($WinrtRoot) + '=external/cppwinrt'),
    ('-ffile-prefix-map=' + [IO.Path]::GetFullPath($CompilerRoot) + '=external/llvm-mingw')
)
$compileFlags += $pathMappings
$sources = @('app.cpp','platform.cpp','graphics.cpp','picker.cpp','audio.cpp','menu.cpp','tasks.cpp','shell.cpp','overlay.cpp','search.cpp','desktop.cpp','wallpaper.cpp','network.cpp','widget_image.cpp','native_desktop.cpp','runtime.cpp','tray.cpp','file_browser.cpp','file_ops.cpp','components.cpp')
$objectRoot = Join-Path $CacheRoot 'objects'
New-Item -ItemType Directory -Path $objectRoot -Force | Out-Null
$compiler = Join-Path $CompilerRoot 'bin\clang++.exe'
$headerHashes = Get-ChildItem -LiteralPath $src -Filter '*.h' | Sort-Object Name | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
$fingerprint = ($headerHashes -join '') + ($compileFlags -join '|') + $compiler + (Get-Item -LiteralPath $compiler).LastWriteTimeUtc.Ticks
$objects = foreach ($name in $sources) {
    $source = Join-Path $src $name
    $key = $fingerprint + (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($key)))).Replace('-','').Substring(0,24) } finally { $sha.Dispose() }
    $object = Join-Path $objectRoot ($name + '-' + $hash + '.o')
    if (-not (Test-Path -LiteralPath $object)) {
        & $compiler @compileFlags '-c' $source '-o' $object
        if ($LASTEXITCODE -ne 0) { throw ('Compilation failed: ' + $name) }
    }
    $object
}
$arguments = @($objects) + @($resource, '-o', $OutputPath, '-mwindows', '-municode', '-static', '-s')
$arguments += @('-ld2d1','-ldwrite','-ld3d11','-ldxgi','-ldcomp','-ldwmapi','-lwindowscodecs','-lole32','-loleaut32','-luuid','-lshell32','-lshlwapi','-lcomdlg32','-ladvapi32','-luser32','-lgdi32','-lpowrprof','-lshcore','-lruntimeobject','-lkernel32','-lwinhttp','-liphlpapi','-lwtsapi32','-lmfplat','-lmfreadwrite','-lmfuuid')
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Link failed. Close the target executable before replacing it.' }
foreach($component in @('Island.Desktop.exe','Island.Files.exe')) {
    Copy-Item -LiteralPath $OutputPath -Destination (Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath))) $component) -Force
}

$bootstrap = Join-Path ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($OutputPath))) 'Island.Start.exe'
& $compiler (Join-Path $src 'bootstrap.cpp') (Join-Path $src 'native_desktop.cpp') $resource '-o' $bootstrap '-std=c++20' '-DUNICODE' '-D_UNICODE' '-D_WIN32_WINNT=0x0A00' '-Os' '-static' '-s' '-mwindows' '-municode' '-lshell32' '-luser32' '-ladvapi32' @pathMappings
if ($LASTEXITCODE -ne 0) { throw 'Bootstrap compilation failed.' }

foreach($name in @('README.md','LICENSE','THIRD_PARTY_NOTICES','Stop-Island.cmd')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $outputDirectory $name) -Force
}
Write-Output ('Built Island in ' + $outputDirectory)
