param(
    [string]$SourceDirectory = '',
    [string]$OutputDirectory = '',
    [string]$Compiler = 'C:\msys64\mingw64\bin\g++.exe'
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$snapshot=Join-Path $root 'vendor-snapshots/phi-recorder'
$revision=(Get-Content -LiteralPath (Join-Path $snapshot 'BASE_COMMIT.txt') -Raw).Trim()
if(!$SourceDirectory){
    $SourceDirectory=Join-Path $snapshot 'source'
    if(!(Test-Path -LiteralPath (Join-Path $SourceDirectory 'src/main.cpp'))){throw 'Missing bundled native engine source'}
}
$SourceDirectory=(Resolve-Path -LiteralPath $SourceDirectory).Path
if(!(Test-Path -LiteralPath (Join-Path $SourceDirectory 'src/hitfx.h'))){throw 'The native source is missing PhiRender integration patches'}
if(!$OutputDirectory){$OutputDirectory=Join-Path $root 'target/release'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$sources=@(Get-ChildItem -LiteralPath (Join-Path $SourceDirectory 'src') -Filter '*.cpp' | ForEach-Object FullName)
$compilerArgs=@('-O2','-std=c++17',"-I$SourceDirectory/third_party","-I$SourceDirectory/src")+$sources+@("$SourceDirectory/third_party/cJSON.c",'-o',"$OutputDirectory/phirender-recorder.exe",'-lopengl32','-lgdi32','-luser32','-static-libgcc','-static-libstdc++')
& $Compiler @compilerArgs
if($LASTEXITCODE){throw 'Native recorder build failed'}
$runtime=Join-Path (Split-Path $Compiler) 'libwinpthread-1.dll'
if(Test-Path -LiteralPath $runtime){Copy-Item -LiteralPath $runtime -Destination $OutputDirectory -Force}
foreach($name in @('noise','blockarea','hud')){
    $dest=Join-Path $OutputDirectory "recorder-assets/$name"
    New-Item -ItemType Directory -Force $dest | Out-Null
    Copy-Item -Path "$SourceDirectory/assets/default/$name/*" -Destination $dest -Recurse -Force
}
Write-Output "Native recorder: $OutputDirectory/phirender-recorder.exe"
Write-Output 'See THIRD_PARTY_NOTICES.md for the project-specific code permission and separate asset licenses.'

foreach($notice in @('LICENSE-RPEN.txt','NOTICE.txt')) {
    Copy-Item -LiteralPath "$SourceDirectory/assets/default/$notice" -Destination (Join-Path $OutputDirectory 'recorder-assets') -Force
}
