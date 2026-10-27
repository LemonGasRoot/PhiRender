param()
$ErrorActionPreference = 'Stop'
$project = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$upstream = Join-Path $project 'work\Phira-Pro'
$snapshot = Join-Path $project 'vendor-snapshots\phira-pro'
if (-not (Test-Path -LiteralPath (Join-Path $upstream '.git'))) {
    throw "Missing Phira Pro Git checkout: $upstream"
}
$head = (& git -C $upstream rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Could not read the Phira Pro commit.' }
$expected = (Get-Content -LiteralPath (Join-Path $snapshot 'BASE_COMMIT.txt') -Raw).Trim()
if ($head -ne $expected) {
    throw "Phira Pro is at $head; snapshot expects $expected. Review the new base before replacing the snapshot."
}
$untracked = Join-Path $snapshot 'untracked'
foreach ($existing in Get-ChildItem -LiteralPath $untracked -File -Recurse) {
    $relative = $existing.FullName.Substring($untracked.Length).TrimStart('\', '/')
    if (-not (Test-Path -LiteralPath (Join-Path $upstream $relative) -PathType Leaf)) {
        throw "Previously snapshotted file is missing from the upstream checkout: $relative. Review it manually."
    }
}
$patchFile = Join-Path $snapshot 'local-changes.patch'
& git -C $upstream -c core.safecrlf=false diff --binary HEAD "--output=$patchFile"
if ($LASTEXITCODE -ne 0) { throw 'Could not save the Phira Pro diff.' }
foreach ($existing in Get-ChildItem -LiteralPath $untracked -File -Recurse) {
    $relative = $existing.FullName.Substring($untracked.Length).TrimStart('\', '/')
    Copy-Item -LiteralPath (Join-Path $upstream $relative) -Destination $existing.FullName -Force
}
$sourceSkin = Join-Path $upstream 'assets\respack-open'
$targetSkin = Join-Path $untracked 'assets\respack-open'
Copy-Item -Path (Join-Path $sourceSkin '*') -Destination $targetSkin -Recurse -Force
$extra = @(& git -C $upstream ls-files --others --exclude-standard)
$known = @('THIRD_PARTY_NOTICES.md', 'docs/development/phi_video-skin-assets-audit.md', 'tools/generate_open_respack.py')
$outside = @($extra | Where-Object { $_ -notin $known -and -not $_.StartsWith('assets/respack-open/') })
if ($outside.Count -gt 0) {
    Write-Warning "Other untracked Phira Pro files are not included automatically; review them before committing: $($outside -join ', ')"
}
Write-Output 'Phira Pro snapshot refreshed. Review git diff, then commit the snapshot with the rest of the project.'
