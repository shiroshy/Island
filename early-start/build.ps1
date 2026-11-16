param([string]$OutputRoot=(Join-Path $PSScriptRoot 'bin'),[string]$CompilerRoot='')
$ErrorActionPreference='Stop'
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
if(!$CompilerRoot){$CompilerRoot=Join-Path $PSScriptRoot '..\work\build\llvm-mingw-20261020-ucrt-x86_64'}
$compiler=Join-Path $CompilerRoot 'bin\clang++.exe'
$pathMappings=@(('-ffile-prefix-map='+[IO.Path]::GetFullPath((Split-Path $PSScriptRoot))+'=.'), ('-ffile-prefix-map='+[IO.Path]::GetFullPath($CompilerRoot)+'=external/llvm-mingw'))
$service=Join-Path $OutputRoot 'Island.Preload.exe'
& $compiler (Join-Path $PSScriptRoot 'service.cpp') @pathMappings -o $service -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -s -municode -lwtsapi32 -luserenv -ltaskschd -lole32 -loleaut32 -lshell32 -luuid -ladvapi32 -luser32
if($LASTEXITCODE -ne 0){throw 'Service compilation failed'}
$test=Start-Process -FilePath $service -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
if($test.ExitCode -ne 0){throw ('Service token/path self-test failed: '+$test.ExitCode)}
$hash=(Get-FileHash -LiteralPath $service -Algorithm SHA256).Hash
('constexpr wchar_t ServiceSHA256[]=L"'+$hash+'";') | Set-Content -LiteralPath (Join-Path $OutputRoot 'service_hash.h') -Encoding UTF8
& $compiler (Join-Path $PSScriptRoot 'install.cpp') @pathMappings -I $OutputRoot -o (Join-Path $OutputRoot 'Island.Preload.Install.exe') -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -s -municode -mwindows -lshell32 -lole32 -luuid -ladvapi32 -luser32 -lbcrypt
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed'}
Write-Output 'Built service and UAC installer; token/path self-test passed.'
