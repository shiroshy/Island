param([string]$Version='0.20.3',[switch]$ReusePayload,[ValidateSet('Internal')][string]$SettingsMode='Internal',[string]$CompilerRoot='',[string]$WinrtRoot='',[string]$CacheRoot='')
$ErrorActionPreference='Stop'
if($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$'){throw 'Invalid version'}
if($Version -ne '0.20.3'){throw 'Public version is pinned to 0.20.3 until explicitly changed'}
$root=Split-Path $PSScriptRoot
$payload=Join-Path $PSScriptRoot 'payload'
$work=Join-Path $PSScriptRoot 'work'
if($SettingsMode -eq 'Internal'){$payload=Join-Path $PSScriptRoot 'payload-internal';$work=Join-Path $PSScriptRoot 'work-internal'}
if(!$CacheRoot){$CacheRoot=Join-Path $root 'work\build'}
if(!$CompilerRoot){
 $sharedCompiler=Join-Path ([IO.Path]::GetFullPath((Join-Path $root '..\..'))) 'work\toolchain\llvm-mingw-20261020-ucrt-x86_64'
 $CompilerRoot=if(Test-Path -LiteralPath (Join-Path $sharedCompiler 'bin\clang++.exe')){$sharedCompiler}else{Join-Path $CacheRoot 'llvm-mingw-20261020-ucrt-x86_64'}
}
$bin=Join-Path $CompilerRoot 'bin'
New-Item -ItemType Directory -Force -Path $payload,$work,(Join-Path $root 'public-dist') | Out-Null
$appBuilt=$false
if(!$ReusePayload){
 if(!(Test-Path -LiteralPath (Join-Path $CompilerRoot 'bin\clang++.exe'))){
  & (Join-Path $root 'build.ps1') -OutputPath (Join-Path $payload 'Island.exe') -SettingsMode $SettingsMode -WinrtRoot $WinrtRoot -CacheRoot $CacheRoot
  if(!$?){throw 'Toolchain bootstrap failed'}
  $appBuilt=$true
 }
 if(!$appBuilt){
 & (Join-Path $root 'build.ps1') -OutputPath (Join-Path $payload 'Island.exe') -SettingsMode $SettingsMode -CompilerRoot $CompilerRoot -WinrtRoot $WinrtRoot -CacheRoot $CacheRoot
 if(!$?){throw 'Application build failed'}
 }
 & (Join-Path $root 'early-start\build.ps1') -OutputRoot $payload -CompilerRoot $CompilerRoot
 if(!$?){throw 'Service build failed'}
 if($SettingsMode -eq 'Native'){
 & "$bin\clang++.exe" "$root\experimental-settings\island_settings_v2.cpp" -shared -o "$payload\Island.SettingsHandler.dll" -std=c++20 -DUNICODE -D_UNICODE -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -I "$root\work\build\generated" -O2 -fms-extensions -Wno-unknown-pragmas -static -s -lruntimeobject -lole32 -loleaut32 -luuid -luser32 -ladvapi32 -ldwmapi -ltaskschd -lshell32
 if($LASTEXITCODE){throw 'Settings build failed'}
 }
}
if($SettingsMode -eq 'Native'){
& "$bin\..\python\bin\python.exe" "$PSScriptRoot\make_patch.py"
if($LASTEXITCODE){throw 'Resource patch build failed'}
}
$names=@('Island.exe','Island.Desktop.exe','Island.Files.exe','Island.Start.exe','Island.Preload.exe','Island.Preload.Install.exe','Island.SettingsHandler.dll')
if($SettingsMode -eq 'Internal'){$names=$names | Where-Object {$_ -ne 'Island.SettingsHandler.dll'}}
$receipt=Join-Path $payload 'explorer-payload.json'
if(!$ReusePayload){
 $hashes=@{};foreach($name in $names){$hashes[$name]=(Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash}
 @{ShellMode='explorer';SourceRoot=(Join-Path $root 'src');Hashes=$hashes} | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $receipt -Encoding UTF8
}
if(!(Test-Path -LiteralPath $receipt)){throw 'Explorer-only payload receipt missing; rebuild without -ReusePayload'}
$verified=Get-Content -LiteralPath $receipt -Raw | ConvertFrom-Json
if($verified.ShellMode -ne 'explorer' -or $verified.SourceRoot -ne (Join-Path $root 'src')){throw 'Only the main Explorer distribution can be packaged'}
foreach($name in $names){if($verified.Hashes.$name -ne (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash){throw "Unverified payload: $name; rebuild without -ReusePayload"}}
foreach($name in @('Uninstall-Island.cmd','Remove-Island-Service.cmd')){
 Copy-Item -LiteralPath (Join-Path "$PSScriptRoot\files" $name) -Destination (Join-Path $payload $name) -Force
 $names+=$name
}
foreach($name in @('LICENSE','THIRD_PARTY_NOTICES')){
 Copy-Item -LiteralPath (Join-Path $root $name) -Destination (Join-Path $payload $name) -Force
 $names+=$name
}
$header=@('#pragma once',"constexpr wchar_t PackageVersion[]=L`"$Version`";",'constexpr wchar_t OriginalPriSHA[]=L"CAB5835106D2225BAE3C6C3A0BC70F0183CDDB50BEE66A9C0CEEB35A6CCBBDDD";','constexpr wchar_t PatchedPriSHA[]=L"6E71EBF0969B95E0B6DF93AF57AA8736964C20AD50C64F4387F92F66CD85519A";')
if($SettingsMode -eq 'Internal'){$header=$header[0..1]}else{
$systemHash=(Get-FileHash -LiteralPath "$env:WINDIR\ImmersiveControlPanel\SystemSettings.exe" -Algorithm SHA256).Hash
$header+='constexpr wchar_t SystemSettingsSHA[]=L"'+$systemHash+'";'
$header+='constexpr int SettingsPatchResource=1100;'
}
$header+='struct PackageFile {int resource;const wchar_t* name;const wchar_t* digest;size_t size;};'
$header+='constexpr PackageFile PackageFiles[]={'
$rc=@('#include <windows.h>')
$index=1000
foreach($name in $names){
 $path=Join-Path $payload $name
 $digest=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
 $length=(Get-Item -LiteralPath $path).Length
 $header+="{$index,L`"$name`",L`"$digest`",$length},"
 $rc+="$index RCDATA `"$($path.Replace('\','/'))`""
 $index++
}
$header+='};'
$header | Set-Content -LiteralPath "$payload\package_manifest.h" -Encoding UTF8
if($SettingsMode -eq 'Native'){$rc+='1100 RCDATA "'+"$payload/settings.patch".Replace('\','/')+'"'}
$rc+='1 ICON "'+"$root/src/Island.ico".Replace('\','/')+'"'
$rc+='1 RT_MANIFEST "'+"$root/src/Island.manifest".Replace('\','/')+'"'
$rc+=@'
1 VERSIONINFO
FILEVERSION 0,20,3,0
PRODUCTVERSION 0,20,3,0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "FileDescription", "Island Setup\0"
   VALUE "ProductName", "Island\0"
   VALUE "FileVersion", "0.20.3\0"
   VALUE "ProductVersion", "0.20.3\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x0409, 1200
 END
END
'@
$rc | Set-Content -LiteralPath "$work\package.rc" -Encoding UTF8
& "$bin\llvm-windres.exe" "$work\package.rc" -O coff -o "$work\package.o"
if($LASTEXITCODE){throw 'Resource compilation failed'}
$output=Join-Path $root "public-dist\Island-Setup-$Version-Win10-x64.exe"
$modeFlags=@()
if($SettingsMode -eq 'Internal'){$modeFlags+='-DISLAND_INTERNAL_SETTINGS';$output=Join-Path $root "public-dist\Island-Setup-$Version-Win10-11-x64-InternalSettings.exe"}
$pathMappings=@(('-ffile-prefix-map='+[IO.Path]::GetFullPath($root)+'=.'), ('-ffile-prefix-map='+[IO.Path]::GetFullPath($CompilerRoot)+'=external/llvm-mingw'))
& "$bin\clang++.exe" "$PSScriptRoot\src\setup.cpp" "$work\package.o" -I $payload @modeFlags @pathMappings -o $output -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -static -s -municode -mwindows -ladvapi32 -lbcrypt -luserenv -lwtsapi32 -ltaskschd -lole32 -loleaut32 -luuid -lshell32 -luser32 -lgdi32 -ldwmapi
if($LASTEXITCODE){throw 'Setup compilation failed'}
Get-FileHash -LiteralPath $output -Algorithm SHA256
