param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repo ('Claude outputs/b5-admin-' + [guid]::NewGuid().ToString('N').Substring(0,10)) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Fresh evidence directory required.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$work = (Resolve-Path -LiteralPath $OutputDirectory).Path
$utf8 = [Text.UTF8Encoding]::new($false)
function Inventory([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return @('ABSENT') }
  return @(Get-ChildItem -LiteralPath $path -Recurse -File | Sort-Object FullName | ForEach-Object {
    $_.FullName.Substring($path.Length) + ' ' + (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
  })
}
$paths = @('apps/admin/dist','apps/web/dist','database/migrations','contracts')
$before = @{}
foreach ($p in $paths) {
  $before[$p] = Inventory (Join-Path $repo $p)
  [IO.File]::WriteAllLines((Join-Path $work ($p.Replace('/','-') + '-before.txt')),[string[]]$before[$p],$utf8)
}
function Run([string]$name,[string[]]$arguments) {
  $command = $arguments[0]; $rest = @($arguments | Select-Object -Skip 1)
  $prior = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  try { $lines = @(& $command @rest 2>&1 | ForEach-Object { [string]$_ }); $code = $LASTEXITCODE }
  finally { $ErrorActionPreference = $prior }
  [IO.File]::WriteAllLines((Join-Path $work ($name+'.log')),[string[]]$lines,$utf8)
  if ($code -ne 0) { $lines | Select-Object -Last 35; throw ($name + ' failed, exit '+$code) }
  Write-Output ($name+': PASS')
}
Push-Location (Join-Path $repo 'apps/admin')
try {
  Run 'focused' @('node','node_modules/vitest/vitest.mjs','run','tests/unit/avatar-catalog.spec.ts','tests/unit/avatar-admin-client.spec.ts')
  Run 'admin-full' @('node','node_modules/vitest/vitest.mjs','run')
  Run 'vue-typecheck' @('node','node_modules/vue-tsc/bin/vue-tsc.js','--noEmit','-p','tsconfig.json')
  Run 'node-typecheck' @('node','node_modules/vue-tsc/bin/vue-tsc.js','--noEmit','-p','tsconfig.node.json')
  Run 'production-build' @('node','node_modules/vite/bin/vite.js','build','--outDir',(Join-Path $work 'admin-dist'))
  foreach ($p in $paths) {
    if (((Inventory (Join-Path $repo $p)) -join "`n") -cne ($before[$p] -join "`n")) { throw ($p+' changed') }
    Write-Output ($p+': UNCHANGED')
  }
  Write-Output ('B5_OFFLINE_ADMIN_PASSED; evidence='+$work)
} finally { Pop-Location }
