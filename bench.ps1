[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$Clean,
    [int]$Runs = 5,
    [string]$Fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq -",
    [int]$Depth = 5
)

$ErrorActionPreference = "Stop"
$projectRoot = $PSScriptRoot

# Binary now built directly to %TEMP%\perft_build\ (see CMakeLists.txt) — Defender
# is much less aggressive there than under C:\personal.
$srcExe = Join-Path $projectRoot "build\Release\perft_cpu.exe"

if ($Build -or $Clean) {
    Write-Host "Building..."
    $buildArgs = @("--build", (Join-Path $projectRoot "build"), "--config", "Release")
    if ($Clean) { $buildArgs += "--clean-first" }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

if (-not (Test-Path $srcExe)) { throw "$srcExe not found - pass -Build first" }

# Even with %TEMP%, copy to a fresh name with retry (2 min budget).
$runExe = Join-Path $env:TEMP ("perft_" + [Guid]::NewGuid().ToString("N").Substring(0,8) + ".exe")
$deadline = (Get-Date).AddSeconds(120)
$copied = $false
while ((Get-Date) -lt $deadline) {
    try {
        Copy-Item $srcExe $runExe -Force -ErrorAction Stop
        $copied = $true
        break
    } catch {
        Start-Sleep -Milliseconds 500
    }
}
if (-not $copied) { throw "could not copy exe within 2 min" }

$pattern = "Perft\(0" + $Depth + "\)"
for ($i = 0; $i -lt $Runs; $i++) {
    & $runExe $Fen $Depth -nott | Select-String $pattern
}

Remove-Item $runExe -Force -ErrorAction SilentlyContinue
