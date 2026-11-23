# B4 offline web verification only. No Docker, cloud, registry enrollment or application database.
param([string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) ('Claude outputs/b4-web-' + [guid]::NewGuid().ToString('N').Substring(0,10))))
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$web = Join-Path $repo 'apps/web'
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists; use a new directory.' }
[void](New-Item -ItemType Directory -Path $OutputDirectory)
$work = (Resolve-Path -LiteralPath $OutputDirectory).Path
$utf8 = [Text.UTF8Encoding]::new($false)
function Inventory([string]$directory) {
    if (-not (Test-Path -LiteralPath $directory)) { return @('ABSENT') }
    return @(Get-ChildItem -LiteralPath $directory -File -Recurse | Sort-Object FullName | ForEach-Object {
        $_.FullName.Substring($directory.Length) + ' ' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    })
}
function Run([string]$label,[string[]]$arguments) {
    $command = $arguments[0]
    $rest = @($arguments | Select-Object -Skip 1)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $lines = @(& $command @rest 2>&1 | ForEach-Object {[string]$_}); $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $previous }
    [IO.File]::WriteAllLines((Join-Path $work ($label + '.log')),[string[]]$lines,$utf8)
    if ($code -ne 0) { $lines | Select-Object -Last 50 | Write-Output; throw ($label + ' failed; exit ' + $code + '; retained log: ' + (Join-Path $work ($label + '.log'))) }
    Write-Output ($label + ': PASS')
}
$dist = Join-Path $web 'dist'
$migrations = Join-Path $repo 'database/migrations'
$beforeDist = Inventory $dist
$beforeMigrations = Inventory $migrations
[IO.File]::WriteAllLines((Join-Path $work 'dist-before.txt'),[string[]]$beforeDist,$utf8)
[IO.File]::WriteAllLines((Join-Path $work 'migrations-before.txt'),[string[]]$beforeMigrations,$utf8)
Push-Location -LiteralPath $web
try {
    Run 'assets' @('node',(Join-Path $repo 'tools/avatar-placeholder-assets.mjs'))
    $rows = @(& node (Join-Path $repo 'tools/avatar-placeholder-assets.mjs') --registry-json)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare review-only registry data.' }
    [IO.File]::WriteAllLines((Join-Path $work 'registry-review-only.json'),[string[]]$rows,$utf8)
    Run 'focused' @('node','node_modules/vitest/vitest.mjs','run','tests/unit/avatar.spec.ts','tests/unit/avatar-client.spec.ts')
    Run 'web-full' @('node','node_modules/vitest/vitest.mjs','run')
    Run 'vue-typecheck' @('node','node_modules/vue-tsc/bin/vue-tsc.js','--noEmit','-p','tsconfig.json')
    Run 'node-typecheck' @('node','node_modules/typescript/bin/tsc','--noEmit','-p','tsconfig.node.json')
    $output = Join-Path $work 'web-dist'
    if (Test-Path -LiteralPath $output) { throw 'Fresh isolated build directory required.' }
    Run 'production-build' @('node','node_modules/vite/bin/vite.js','build','--outDir',$output)
    $sw = [IO.File]::ReadAllText((Join-Path $output 'sw.js'))
    if ($sw.Contains('avatars/')) { throw 'Avatar images unexpectedly appeared in the service-worker precache.' }
    $manifest = [IO.File]::ReadAllText((Join-Path $web 'src/assets/avatar-manifest.json')) | ConvertFrom-Json
    $files = @($manifest.base)
    if ($manifest.bases) { foreach ($base in $manifest.bases.PSObject.Properties) { $files += $base.Value } }
    foreach ($entry in $manifest.assets.PSObject.Properties) { foreach ($layer in $entry.Value.layers.PSObject.Properties) { $files += $layer.Value } }
    foreach ($file in $files) {
        if ((Get-FileHash -LiteralPath (Join-Path $output ('avatars/' + $file.file)) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256) { throw 'Built asset hash mismatch.' }
    }
    if (((Inventory $dist) -join "`n") -cne ($beforeDist -join "`n")) { throw 'Existing dist changed.' }
    if (((Inventory $migrations) -join "`n") -cne ($beforeMigrations -join "`n")) { throw 'Frozen migration changed.' }
    Write-Output ('BUILT ASSETS VERIFIED: ' + $files.Count + '; no avatar-art precache entry')
    Write-Output ('UNCHANGED: dist ' + $beforeDist.Count + ' files; migrations ' + $beforeMigrations.Count + ' files')
    Write-Output ('B4_OFFLINE_WEB_PASSED; evidence=' + $work)
} finally { Pop-Location }
