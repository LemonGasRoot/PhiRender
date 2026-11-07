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
    $SourceDirectory=Join-Path $root 'work/phi-recorder'
    if(!(Test-Path -LiteralPath $SourceDirectory)){
        & git clone --no-checkout https://github.com/RyderLyx/phi-recorder.git $SourceDirectory
        if($LASTEXITCODE){throw 'Native source clone failed'}
        & git -C $SourceDirectory checkout $revision
        if($LASTEXITCODE){throw 'Native source revision checkout failed'}
        & git -C $SourceDirectory apply --whitespace=nowarn (Join-Path $snapshot 'local-changes.patch')
        if($LASTEXITCODE){throw 'Native source patch failed'}
        Copy-Item -Path (Join-Path $snapshot 'untracked/*') -Destination $SourceDirectory -Recurse -Force
        $revision | Set-Content (Join-Path $SourceDirectory 'PHIRENDER_SNAPSHOT.txt')
    }elseif(!(Test-Path -LiteralPath (Join-Path $SourceDirectory 'PHIRENDER_SNAPSHOT.txt'))){
        throw 'Existing native checkout is not a restored PhiRender snapshot. Review it or pass -SourceDirectory explicitly.'
    }
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
Write-Output 'This is a local development integration. See vendor-snapshots/phi-recorder/README.md for upstream license status.'
