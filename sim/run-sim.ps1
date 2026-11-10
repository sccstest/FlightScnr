# Build and launch the FlightScnr desktop simulator (Windows).
#
#   .\sim\run-sim.ps1                         # last saved location
#   .\sim\run-sim.ps1 -Lat 27.9755 -Lon -82.5332
#   .\sim\run-sim.ps1 -Scale 0.8 -NoBuild
#   .\sim\run-sim.ps1 -Extra '--tap','8:360:677','--shot','20','--exit-after','25'
#
# Mouse = finger (click = tap, drag = swipe). F12 = screenshot, Esc = quit.
# Settings and caches are kept in .\sim_data\ (delete it to start fresh).
param(
  [double]$Lat = [double]::NaN,
  [double]$Lon = [double]::NaN,
  [double]$Scale = 1.0,
  [switch]$NoBuild,
  [string[]]$Extra = @()
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# Pick up compilers installed by winget since this shell started.
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')
Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

if (-not $NoBuild) {
  if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Host 'No g++ found. Install it with:  winget install BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor Yellow
    exit 1
  }
  $pio = Join-Path $env:USERPROFILE '.platformio\penv\Scripts\pio.exe'
  if (-not (Test-Path $pio)) { $pio = 'pio' }
  & $pio run -e sim
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$argsList = @('--scale', "$Scale")
if (-not [double]::IsNaN($Lat) -and -not [double]::IsNaN($Lon)) {
  $argsList += @('--lat', "$Lat", '--lon', "$Lon")
}
$argsList += $Extra
& (Join-Path $root '.pio\build\sim\program.exe') @argsList
