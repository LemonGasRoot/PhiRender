param()
$ErrorActionPreference = 'Stop'

$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$work = Join-Path $project 'work'
$target = Join-Path $work 'Phira-Pro'
$temporary = Join-Path $work 'Phira-Pro-bootstrap'
$snapshot = Join-Path $project 'vendor-snapshots\phira-pro'
$commit = (Get-Content -LiteralPath (Join-Path $snapshot 'BASE_COMMIT.txt') -Raw).Trim()
$patch = Join-Path $snapshot 'local-changes.patch'
$untracked = Join-Path $snapshot 'untracked'

if (Test-Path -LiteralPath $target) {
    throw "Refusing to overwrite existing checkout: $target. Review and move it yourself before retrying."
}
if (Test-Path -LiteralPath $temporary) {
    throw "Temporary restore path already exists: $temporary. Review it manually before retrying."
}

New-Item -ItemType Directory -Path $work -Force | Out-Null
& git clone --filter=blob:none --no-checkout https://github.com/Phira-Pro/Phira-Pro.git $temporary
if ($LASTEXITCODE -ne 0) { throw 'Could not clone Phira Pro.' }
& git -C $temporary checkout --detach $commit
if ($LASTEXITCODE -ne 0) { throw "Could not check out pinned Phira Pro commit $commit." }
& git -C $temporary apply --check $patch
if ($LASTEXITCODE -ne 0) { throw 'The Phira Pro patch does not apply cleanly to the pinned commit.' }
& git -C $temporary apply $patch
if ($LASTEXITCODE -ne 0) { throw 'Could not apply the PhiRender changes.' }

Copy-Item -Path (Join-Path $untracked '*') -Destination $temporary -Recurse -Force
Move-Item -LiteralPath $temporary -Destination $target
Write-Output "Restored patched Phira Pro source to $target"
Write-Output 'Review upstream asset licensing before redistributing any files from its assets directory.'

